#include "core/channel_scan.h"

#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

#include <log/log.h>
#include <minIni.h>

#include "core/app_state.h"
#include "core/common.hh"
#include "core/defines.h"
#include "core/dvr.h"
#include "core/input_device.h"
#include "core/osd.h"
#include "core/settings.h"
#include "core/thread.h"
#include "driver/dm5680.h"
#include "driver/dm6302.h"
#include "driver/hardware.h"
#include "driver/rtc6715.h"
#include "ui/page_common.h"
#include "ui/page_scannow.h"
#include "ui/page_source.h"
#include "util/system.h"
#include "util/time.h"

// After the retune, before the valid flag is believed: the settle
// scan_channel() gives the Scan Now page. It also lets an answer to a request
// made before the retune come in, so the clear below throws it away rather
// than counting it for the new channel.
#define SCAN_SETTLE_MS 100

// How long a link has to come up on a channel before the scan moves on, and
// how often the receivers are asked meanwhile. The Scan Now page decides on a
// single look after its settle; this waits a little longer, because a channel
// missed here costs a full tuner init on the next probe.
#define SCAN_HDZERO_WINDOW_MS 500
#define SCAN_POLL_MS          50

// Analog: RSSI after the retune, averaged, as a percentage of the Analog RSSI
// page's calibration -- the figure the OSD bar shows. The threshold is a first
// guess; every probe logs what it read, so it can be set from the logs.
#define SCAN_ANALOG_SETTLE_MS 100
#define SCAN_ANALOG_READS     8
#define SCAN_ANALOG_MIN_PCT   30

typedef enum {
    SCAN_FOUND_NONE = 0,
    SCAN_FOUND_HDZERO,
    SCAN_FOUND_ANALOG,
} scan_found_t;

static volatile bool scan_busy = false;

// Whether this goggle has an analog receiver the app tunes itself. The goggle
// v1 has none, and an expansion module in the bay is tuned by its own
// buttons.
static bool scan_has_analog(void) {
#if defined(HDZGOGGLE2)
    return g_setting.source.analog_module == SETTING_SOURCES_ANALOG_MODULE_INTERNAL;
#elif defined(HDZBOXPRO)
    return true;
#else
    return false;
#endif
}

bool channel_scan_dial(void) {
    if (!g_setting.input.channel_scan)
        return false;

    if (g_source_info.source == SOURCE_HDZERO)
        return true;

    return g_source_info.source == SOURCE_AV_MODULE && scan_has_analog();
}

bool channel_scan_busy(void) {
    return scan_busy;
}

uint8_t channel_scan_analog_of(uint8_t band, uint8_t hdz_ch) {
    // R1-R8, E1, F1, F2, F4 in the analog numbering (A B E F R L, 8 each).
    static const uint8_t raceband[BASE_CH_NUM] = {33, 34, 35, 36, 37, 38, 39, 40, 17, 25, 26, 28};

    if (band == SETTING_SOURCES_HDZERO_BAND_LOWBAND)
        return ((hdz_ch >= 1) && (hdz_ch <= 8)) ? 41 + hdz_ch - 1 : 0;

    return ((hdz_ch >= 1) && (hdz_ch <= BASE_CH_NUM)) ? raceband[hdz_ch - 1] : 0;
}

void channel_scan_carry_to_analog(void) {
    if (!g_setting.input.channel_scan || !scan_has_analog())
        return;

    uint8_t ach = channel_scan_analog_of(g_setting.source.hdzero_band, g_setting.scan.channel);

    if ((ach == 0) || (ach == g_setting.source.analog_channel))
        return;

    g_setting.source.analog_channel = ach;
    ini_putl("source", "analog_channel", ach, SETTING_INI);
    LOGI("chscan: analog follows the dial to %s", channel2str(0, 0, ach));
}

static const char *bw_name(int bw) {
    return (bw == SETTING_SOURCES_HDZERO_BW_NARROW) ? "narrow" : "wide";
}

static bool probe_hdzero(uint8_t ch, int bw) {
    const char *name = channel2str(1, g_setting.source.hdzero_band, ch);
    uint32_t t0 = time_ms();

    // Opens the tuner if it is closed and, if it is on the other bandwidth,
    // closes and reopens it: a full DM6302_init() either way, and the
    // expensive step of the scan. On the bandwidth already open, nothing.
    HDZero_open(bw);
    uint32_t open_ms = time_ms() - t0;

    if (!g_hw_stat.hdzero_open) {
        LOGW("chscan: hdzero %s did not open", bw_name(bw));
        return false;
    }

    DM6302_SetChannel(g_setting.source.hdzero_band, (ch - 1) & 0x7f);
    usleep(SCAN_SETTLE_MS * 1000);
    DM5680_clear_vldflg();

    // The answers are parsed on the two UART threads, which this one does
    // not hold up.
    uint32_t t1 = time_ms();

    while (time_ms() - t1 < SCAN_HDZERO_WINDOW_MS) {
        DM5680_req_vldflg();
        usleep(SCAN_POLL_MS * 1000);

        if (rx_status[0].rx_valid || rx_status[1].rx_valid) {
            LOGI("chscan: hdzero %s %s: valid after %ums (open %ums)",
                 bw_name(bw), name, time_ms() - t1, open_ms);
            return true;
        }
    }

    LOGI("chscan: hdzero %s %s: nothing in %ums (open %ums)",
         bw_name(bw), name, time_ms() - t1, open_ms);
    return false;
}

#if defined(HDZGOGGLE2) || defined(HDZBOXPRO)
static int rssi_percent(int mv) {
    int lo = g_setting.analog_rssi.calib_min;
    int hi = g_setting.analog_rssi.calib_max;

    if (hi <= lo)
        return 0;
    if (mv <= lo)
        return 0;
    if (mv >= hi)
        return 100;

    return (mv - lo) * 100 / (hi - lo);
}

// RSSI is power on the frequency and says nothing about what kind of signal
// it is, so a digital VTX would read as analog here. That is why this goes
// last: by the time it runs, HDZero has failed to decode the channel at
// either bandwidth.
static bool probe_analog(uint8_t ach) {
    // On the analog source the receiver is already powered, with the audio
    // the source wants; anywhere else it is powered for the probe only.
    bool powered = g_source_info.source == SOURCE_AV_MODULE;
    int sum = 0;

    if (!powered)
        rtc6715.init(1, 0);

    rtc6715.set_ch(ach - 1);
    usleep(SCAN_ANALOG_SETTLE_MS * 1000);

    for (int i = 0; i < SCAN_ANALOG_READS; i++) {
        sum += rtc6715_get_rssi();
        usleep(10 * 1000);
    }

    if (!powered)
        rtc6715.init(0, 0);

    int mv = sum / SCAN_ANALOG_READS;
    int pct = rssi_percent(mv);
    bool found = pct >= SCAN_ANALOG_MIN_PCT;

    LOGI("chscan: analog %s: rssi %dmV = %d%% (calib %d-%d, needs %d%%)%s",
         channel2str(0, 0, ach), mv, pct, g_setting.analog_rssi.calib_min,
         g_setting.analog_rssi.calib_max, SCAN_ANALOG_MIN_PCT, found ? ", on air" : "");
    return found;
}
#endif

static void set_analog_channel(uint8_t ach) {
    if (g_setting.source.analog_channel == ach)
        return;

    g_setting.source.analog_channel = ach;
    ini_putl("source", "analog_channel", ach, SETTING_INI);
}

// Run under lvgl_mutex, from video, on HDZero or the analog receiver.
static void scan_run(uint8_t ch) {
    const uint8_t band = g_setting.source.hdzero_band;
    const int source = g_source_info.source;
    const int bw = g_setting.source.hdzero_bw;
    const int other_bw = (bw == SETTING_SOURCES_HDZERO_BW_WIDE) ? SETTING_SOURCES_HDZERO_BW_NARROW
                                                                 : SETTING_SOURCES_HDZERO_BW_WIDE;
    const uint8_t ach = channel_scan_analog_of(band, ch);
    const char *name = channel2str(1, band, ch);
    scan_found_t found = SCAN_FOUND_NONE;
    int found_bw = bw;
    uint32_t t0 = time_ms();

    LOGI("chscan: %s start, on %s, hdzero %s", name,
         source == SOURCE_HDZERO ? "hdzero" : "analog", bw_name(bw));

    // The picture is frozen until the scan is over (this holds lvgl_mutex),
    // so put the question up now and leave it there.
    osd_channel_show(true);
    lv_timer_handler();

    // The probes close the receivers, as the Wide/Narrow button does, and the
    // same rule applies to a recording still being finalised.
    source_switch_hold();
    dvr_cmd(DVR_STOP);
    if (g_setting.bugfix.wait_for_recording)
        dvr_collect_stop();

    // The bandwidth in use first: on HDZero it costs no tuner init.
    if (probe_hdzero(ch, bw)) {
        found = SCAN_FOUND_HDZERO;
    } else if (probe_hdzero(ch, other_bw)) {
        found = SCAN_FOUND_HDZERO;
        found_bw = other_bw;
    }
#if defined(HDZGOGGLE2) || defined(HDZBOXPRO)
    else if (scan_has_analog() && (ach != 0) && probe_analog(ach)) {
        found = SCAN_FOUND_ANALOG;
    }
#endif

    // Where the dial is, whatever was found: the next turn starts here.
    if (g_setting.scan.channel != ch) {
        g_setting.scan.channel = ch;
        ini_putl("scan", "channel", ch, SETTING_INI);
    }

    switch (found) {
    case SCAN_FOUND_HDZERO:
        if (found_bw != bw)
            source_set_hdzero_bw(found_bw);

        if (source != SOURCE_HDZERO)
            source_select_hdzero();
        else if (found_bw != bw)
            app_switch_to_hdzero(true); // as the Wide/Narrow button does
        else
            hdzero_switch_channel(ch - 1);
        break;

    case SCAN_FOUND_ANALOG:
        set_analog_channel(ach);
        if (source == SOURCE_AV_MODULE)
            HDZero_Close(); // the receiver is already on it; undo the probes
        else
            source_select_analog();
        break;

    case SCAN_FOUND_NONE:
        // Nothing on air: end up where the plain dial would have, on the
        // source and bandwidth we started from.
        if (source == SOURCE_HDZERO) {
            if (!g_hw_stat.hdzero_open || (g_hw_stat.hdz_bw != bw))
                app_switch_to_hdzero(true);
            else
                hdzero_switch_channel(ch - 1);
        } else {
            set_analog_channel(ach);
            rtc6715.set_ch(ach - 1);
            HDZero_Close();
        }
        break;
    }

    Analog_Module_Power(0);
    source_switch_release();

    LOGI("chscan: %s -> %s, took %ums", name,
         found == SCAN_FOUND_HDZERO ? (found_bw == SETTING_SOURCES_HDZERO_BW_NARROW ? "hdzero narrow" : "hdzero wide")
         : found == SCAN_FOUND_ANALOG ? "analog"
                                      : "nothing",
         time_ms() - t0);
}

static void *scan_thread(void *arg) {
    uint8_t ch = (uint8_t)(uintptr_t)arg;

    log_thread_id("channel scan");
    pthread_mutex_lock(&lvgl_mutex);

    // The click that asked for this held the lock until it returned, and the
    // menu could have been opened in between.
    if (g_app_state == APP_STATE_VIDEO && channel_scan_dial()) {
        scan_run(ch);
        tune_channel_done();
    } else {
        LOGI("chscan: dropped, the app left video (state %d, source %d)", g_app_state, g_source_info.source);
        exit_tune_channel();
    }

    scan_busy = false;
    g_scanning = false;
    pthread_mutex_unlock(&lvgl_mutex);
    return NULL;
}

bool channel_scan_start(uint8_t hdz_ch) {
    pthread_t tid;
    pthread_attr_t attr;
    bool started;

    if (scan_busy) {
        LOGI("chscan: a scan is running, %d ignored", hdz_ch);
        return false;
    }

    if ((hdz_ch < 1) || (hdz_ch > HDZERO_CHANNEL_NUM)) {
        LOGW("chscan: no channel %d in this band", hdz_ch);
        return false;
    }

    // Raised here rather than on the thread, so the dial and the long press
    // are ignored from the click on: input_device.c tests g_scanning.
    scan_busy = true;
    g_scanning = true;

    // Detached: nothing waits for it, and it ends itself.
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    started = pthread_create(&tid, &attr, scan_thread, (void *)(uintptr_t)hdz_ch) == 0;
    pthread_attr_destroy(&attr);

    if (!started) {
        LOGE("chscan: could not start the scan thread");
        scan_busy = false;
        g_scanning = false;
    }

    return started;
}
