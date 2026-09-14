#include "dvr.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <log/log.h>
#include <minIni.h>

#include "../conf/targets.h"

#include "core/msp_displayport.h"
#include "core/settings.h"
#include "driver/hardware.h"
#include "record/record_definitions.h"
#include "ui/page_common.h"
#include "util/sdcard.h"
#include "util/system.h"
#include "util/time.h"

bool dvr_is_recording = false;
bool record_pending = false;

static time_t dvr_recording_start = 0;
static pthread_mutex_t dvr_mutex;

///////////////////////////////////////////////////////////////////
// The record process's own status, as it writes it to REC_dataFILE:
//-1=error;
// 0=idle,1=recording,2=stopped,3=No SD card,4=recorf file path error,
// 5=SD card Full,6=Encoder error
#define DVR_STATUS_RECORDING 1

// The words behind those numbers, from src/record/record.h. A day of flying
// went unrecorded because every start timed out and nothing said why: the
// record process had written its reason to this file the whole time.
static const char *dvr_status_name(int status) {
    switch (status) {
    case -1:
        return "unreadable";
    case 0:
        return "idle";
    case 1:
        return "recording";
    case 2:
        return "stopped";
    case 3:
        return "no card";
    case 4:
        return "record path failed";
    case 5:
        return "card full";
    case 6:
        return "no frames";
    case 7:
        return "file open failed";
    default:
        return "unknown";
    }
}

// The record process's own source warns that a power-off during a recording
// can leave the card mounted read-only until it is re-inserted. Worth a word
// in the log when a start fails, since nothing else would show it.
static bool dvr_card_readonly(void) {
    struct statvfs vfs;

    if (statvfs("/mnt/extsd", &vfs) != 0)
        return false;

    return (vfs.f_flag & ST_RDONLY) != 0;
}

// Set by a stop request before it takes dvr_mutex, so a start still polling
// for the record process can give up early and let the stop through. The
// menu switch and the source switch both stop the recorder first, and both
// were seen waiting up to two seconds here while the recorder was refusing
// to start at all.
static volatile bool dvr_stop_wanted = false;

// The flat two seconds the waits below replace. Kept as the cap, so a record
// process that never answers costs exactly what it used to, no more.
#define DVR_WAIT_MS 2000
#define DVR_POLL_MS 20

static int dvr_read_status(void) {
    int status = -1;
    FILE *fp = fopen(REC_dataFILE, "r");

    if (!fp)
        return -1;

    if (fscanf(fp, "%d", &status) != 1)
        status = -1;

    fclose(fp);

    return status;
}

static bool dvr_stop_outstanding;

///////////////////////////////////////////////////////////////////
// Starts that never became a recording.
//
// A day of flying on a card the kernel had turned read-only went like this:
// each time the picture came back the auto-record started, the record process
// said 7 (file open failed), dvr_update_status() stopped it, and three seconds
// later it started again -- 178 times over four boots, holding dvr_mutex for
// two seconds each time and showing nothing. With Give Up Auto DVR (Fixes) on,
// a few of those in a row end the automatic starts for the boot and put the
// record process's reason next to the SD icon. A manual start still goes
// through, and a start that works, or pulling the card, lifts it.
#define DVR_GIVE_UP_AFTER 3

// A recording the record process abandons this soon after the start never
// really began: the file open or the encoder failed, not the flight.
#define DVR_FAILED_START_S 10

static int dvr_failed_starts;
static bool dvr_halted;
static int dvr_halt_status;
static bool dvr_card_state_logged;

// What the kernel thinks of the card, once per boot, the first time a start
// fails. dvr_card_readonly() never fired on the day the record process got
// EROFS on every open, and nothing in the log said why, so this takes the
// same answer three ways: statvfs with its errno, the /proc/mounts line, and
// what dmesg has said about the card. Runs under dvr_mutex, after the two
// seconds the start already cost, so its own few milliseconds do not matter.
static void dvr_log_card_state(void) {
    char line[512];

    if (dvr_card_state_logged)
        return;
    dvr_card_state_logged = true;

    struct statvfs vfs;
    if (statvfs("/mnt/extsd", &vfs) == 0)
        LOGI("dvr: statvfs /mnt/extsd f_flag=0x%lx%s", (unsigned long)vfs.f_flag,
             (vfs.f_flag & ST_RDONLY) ? " (read-only)" : "");
    else
        LOGI("dvr: statvfs /mnt/extsd failed, errno %d", errno);

    FILE *fp = fopen("/proc/mounts", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, " /mnt/extsd ")) {
                line[strcspn(line, "\r\n")] = 0;
                LOGI("dvr: mounts: %s", line);
            }
        }
        fclose(fp);
    }

    fp = popen("dmesg | grep -i -E 'fat|mmc|read-only|remount' | tail -n 6", "r");
    if (fp) {
        int n = 0;
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = 0;
            LOGI("dvr: dmesg: %s", line);
            n++;
        }
        pclose(fp);
        if (n == 0)
            LOGI("dvr: dmesg: nothing about the card");
    }
}

// Called under dvr_mutex.
static void dvr_note_failed_start(int status) {
    dvr_failed_starts++;
    LOGW("dvr: failed start %d in a row, record process says %d (%s)",
         dvr_failed_starts, status, dvr_status_name(status));
    dvr_log_card_state();

    if (!g_setting.bugfix.dvr_give_up || dvr_halted)
        return;

    if (dvr_failed_starts >= DVR_GIVE_UP_AFTER) {
        dvr_halted = true;
        dvr_halt_status = status;
        LOGW("dvr: giving up auto record for this boot: %s. Re-insert or repair the card",
             dvr_status_name(status));
    }
}

static void dvr_note_started(void) {
    if (dvr_halted)
        LOGI("dvr: a recording started, auto record back on");
    dvr_failed_starts = 0;
    dvr_halted = false;
}

bool dvr_auto_is_halted(void) {
    return dvr_halted;
}

const char *dvr_auto_halt_reason(void) {
    return dvr_halted ? dvr_status_name(dvr_halt_status) : NULL;
}

void dvr_card_removed(void) {
    if (dvr_halted)
        LOGI("dvr: card removed, auto record back on");
    dvr_failed_starts = 0;
    dvr_halted = false;
}

void dvr_update_status() {
    pthread_mutex_lock(&dvr_mutex);
    if (dvr_is_recording) {
        int status = dvr_read_status();

        if (status != DVR_STATUS_RECORDING) {
            LOGW("dvr: record process says %d (%s), stopping", status, dvr_status_name(status));
            dvr_is_recording = false;
            system_script(REC_STOP);
            record_pending = false;

            if (time(NULL) - dvr_recording_start <= DVR_FAILED_START_S)
                dvr_note_failed_start(status);

            // The two seconds here were held under dvr_mutex, so a source
            // switch that arrived meanwhile waited them out. With the
            // deferred stop on, the next start collects it instead, the same
            // as any other stop.
            if (g_setting.speed.dvr_defer_stop)
                dvr_stop_outstanding = true;
            else
                sleep(2); // wait for record process
        }
    }
    pthread_mutex_unlock(&dvr_mutex);
}

void dvr_enable_line_out(bool enable) {
    // audio_sel.sh forks amixer once per mixer control: out_off measured
    // ~100ms and out_on three times that. The menu/video switch calls this
    // every time with the state it already has, so only touch the mixer on a
    // real change. Nothing outside these two functions drives audio_sel.sh.
    static int last_enable = -1;
    char buf[128];

    if (g_setting.speed.skip_audio && last_enable == (int)enable)
        return;
    last_enable = enable;

    if (enable) {
        snprintf(buf, sizeof(buf), "%s out_on", AUDIO_SEL_SH);
        system_exec(buf);
        snprintf(buf, sizeof(buf), "%s out_linein_on", AUDIO_SEL_SH);
        system_exec(buf);
        snprintf(buf, sizeof(buf), "%s out_dac_off", AUDIO_SEL_SH);
        system_exec(buf);
    } else {
        snprintf(buf, sizeof(buf), "%s out_off", AUDIO_SEL_SH);
        system_exec(buf);
    }
}

void dvr_select_audio_source(uint8_t source) {
    char buf[128];
    char *audio_source[3] = {
        "in_mic1",
        "in_mic2",
        "in_linein"};

    if (source > 2)
        source = 2;

    // in_mic2 and friends clear all eight input switches before setting two,
    // twelve amixer processes in all, measured at ~365ms. The source does not
    // change from one switch to the next.
    static int last_source = -1;
    if (g_setting.speed.skip_audio && last_source == (int)source)
        return;
    last_source = source;

    snprintf(buf, sizeof(buf), "%s %s", AUDIO_SEL_SH, audio_source[source]);
    system_exec(buf);
}

// video input config
void dvr_update_vi_conf(video_resolution_t fmt) {
    pthread_mutex_lock(&dvr_mutex);
    switch (fmt) {
    case VR_720P50:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 50, REC_CONF);
        break;
    case VR_720P60:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 60, REC_CONF);
        break;
    case VR_720P30:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 30, REC_CONF);
        break;
    case VR_540P90:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 90, REC_CONF);
        break;
    case VR_540P60:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 60, REC_CONF);
        break;
    case VR_960x720P60:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 60, REC_CONF);
        break;
    case VR_540P90_CROP:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 90, REC_CONF);
        break;
#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
    case VR_1080P30:
        ini_putl("vi", "width", 1920, REC_CONF);
        ini_putl("vi", "height", 1080, REC_CONF);
        ini_putl("vi", "fps", 30, REC_CONF);
        break;
    case VR_1080P24:
        ini_putl("vi", "width", 1920, REC_CONF);
        ini_putl("vi", "height", 1080, REC_CONF);
        ini_putl("vi", "fps", 50, REC_CONF);
        break;
    case VR_1080P50:
        ini_putl("vi", "width", 1920, REC_CONF);
        ini_putl("vi", "height", 1080, REC_CONF);
        ini_putl("vi", "fps", 50, REC_CONF);
        break;
    case VR_1080P60:
        ini_putl("vi", "width", 1920, REC_CONF);
        ini_putl("vi", "height", 1080, REC_CONF);
        ini_putl("vi", "fps", 59, REC_CONF); // If set fps to 60, DVR is wrong. I don't know why. 59 or 61 is ok.
        break;
#elif defined HDZBOXPRO
    case VR_1080P30:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 60, REC_CONF);
        break;
    case VR_1080P24:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 50, REC_CONF);
        break;
    case VR_1080P50:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 50, REC_CONF);
        break;
    case VR_1080P60:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 59, REC_CONF); // If set fps to 60, DVR is wrong. I don't know why. 59 or 61 is ok.
        break;
#endif
    }
    pthread_mutex_unlock(&dvr_mutex);
    sync();

    LOGI("update_record_vi_conf: fmt=%d", fmt);
}

void dvr_toggle() {
    dvr_cmd(DVR_TOGGLE);
}

void dvr_star() {
    pthread_mutex_lock(&dvr_mutex);
    LOGI("dvr: star%s", dvr_is_recording ? "" : " requested, but nothing is recording");
    if (dvr_is_recording) {
        char current_dvr_file[256] = "";
        FILE *now_recording_file = fopen(NOW_RECORDING_FILE, "r");
        if (now_recording_file) {
            const size_t read_count = fread(current_dvr_file, 1, sizeof(current_dvr_file) - 1, now_recording_file);
            if (ferror(now_recording_file) == 0) {
                current_dvr_file[read_count] = '\0';
                strcat(current_dvr_file, REC_starSUFFIX);
                FILE *like_file = fopen(current_dvr_file, "a");
                if (like_file) {
                    unsigned recording_duration_s = time(NULL) - dvr_recording_start;
                    unsigned minutes = recording_duration_s / 60;
                    unsigned seconds = recording_duration_s % 60;
                    fprintf(like_file, REC_starFORMAT, minutes, seconds);
                    fclose(like_file);
                }
            }
            fclose(now_recording_file);
        }
    }
    pthread_mutex_unlock(&dvr_mutex);
}

static void dvr_update_record_conf() {
    int bitrate_scale;
    switch (g_setting.record.bitrate_scale) {
    case SETTING_RECORD_BITRATE_SCALE_NORMAL:
        bitrate_scale = 1;
        break;
    case SETTING_RECORD_BITRATE_SCALE_HALF:
        bitrate_scale = 2;
        break;
    case SETTING_RECORD_BITRATE_SCALE_QUARTER:
        bitrate_scale = 4;
        break;
    default:
        bitrate_scale = 1;
        break;
    }
    if (g_setting.record.format_ts)
        ini_puts("record", "type", "ts", REC_CONF);
    else
        ini_puts("record", "type", "mp4", REC_CONF);

    if (g_source_info.source == SOURCE_HDZERO) {
        LOGI("CAM_MODE=%d", CAM_MODE);
        if (CAM_MODE == VR_1080P30 || CAM_MODE == VR_1080P24) {
            ini_putl("venc", "width", 1920, REC_CONF);
            ini_putl("venc", "height", 1080, REC_CONF);
        } else {
            ini_putl("venc", "width", 1280, REC_CONF);
            ini_putl("venc", "height", 720, REC_CONF);
        }

        if (CAM_MODE == VR_1080P30) { // 1080p30
            ini_putl("venc", "fps", 60, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
        } else if (CAM_MODE == VR_1080P24) { // 1080p24
            ini_putl("venc", "fps", 50, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
        } else if (CAM_MODE == VR_540P90 || CAM_MODE == VR_540P90_CROP) { // 90fps
            ini_putl("venc", "fps", 90, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
        } else {
            ini_putl("venc", "fps", 60, REC_CONF);
            ini_putl("venc", "kbps", 24000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 1, REC_CONF);
        }
    } else if (g_source_info.source == SOURCE_AV_IN || g_source_info.source == SOURCE_AV_MODULE) { // Analog
        ini_putl("venc", "width", 1280, REC_CONF);
        ini_putl("venc", "height", 720, REC_CONF);

        ini_putl("venc", "kbps", 24000 / bitrate_scale, REC_CONF);
        ini_putl("venc", "h265", 1, REC_CONF);
        if (g_hw_stat.av_pal[g_hw_stat.is_av_in])
            ini_putl("venc", "fps", 50, REC_CONF);
        else
            ini_putl("venc", "fps", 60, REC_CONF);
    } else if (g_source_info.source == SOURCE_HDMI_IN) {
        LOGI("g_hw_stat.hdmiin_vtmg=%d", g_hw_stat.hdmiin_vtmg);
        switch (g_hw_stat.hdmiin_vtmg) {
        case HDMIIN_VTMG_1080P60:
            ini_putl("venc", "width", 1920, REC_CONF);
            ini_putl("venc", "height", 1080, REC_CONF);
            ini_putl("venc", "fps", 60, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        case HDMIIN_VTMG_1080P50:
            ini_putl("venc", "width", 1920, REC_CONF);
            ini_putl("venc", "height", 1080, REC_CONF);
            ini_putl("venc", "fps", 50, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        case HDMIIN_VTMG_1080Pother:
            ini_putl("venc", "width", 1920, REC_CONF);
            ini_putl("venc", "height", 1080, REC_CONF);
            ini_putl("venc", "fps", 50, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        case HDMIIN_VTMG_720P50:
            ini_putl("venc", "width", 1280, REC_CONF);
            ini_putl("venc", "height", 720, REC_CONF);
            ini_putl("venc", "fps", 50, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        case HDMIIN_VTMG_720P60:
            ini_putl("venc", "width", 1280, REC_CONF);
            ini_putl("venc", "height", 720, REC_CONF);
            ini_putl("venc", "fps", 60, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        case HDMIIN_VTMG_720P100:
            ini_putl("venc", "width", 1280, REC_CONF);
            ini_putl("venc", "height", 720, REC_CONF);
            ini_putl("venc", "fps", 90, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        default:
            ini_putl("venc", "width", 1280, REC_CONF);
            ini_putl("venc", "height", 720, REC_CONF);
            ini_putl("venc", "fps", 60, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        }
    }

    ini_putl("record", "audio", g_setting.record.audio, REC_CONF);
    dvr_select_audio_source(g_setting.record.audio_source);
    ini_putl("record", "naming", g_setting.record.naming, REC_CONF);

    sync();
}

// gogglecmd only asks the record process to do something; it has not done it
// when the command returns, which is what the sleeps were covering. But the
// record process writes its status to REC_dataFILE either side of the work --
// REC_statusRun once the encoder is going and the file is open, the stop
// status after ffpack_close() -- so the file says when it is actually done.
//
// Anything other than the state asked for ends the wait, an unreadable file
// included: none of those can mean the recording is in the state we are
// waiting to leave.
static void dvr_poll_status(bool want_recording, const char *what) {
    uint32_t t0 = time_ms();
    int status;

    for (;;) {
        status = dvr_read_status();
        if ((status == DVR_STATUS_RECORDING) == want_recording) {
            LOGI("dvr: record %s took %ums", what, time_ms() - t0);
            return;
        }

        if (want_recording && dvr_stop_wanted) {
            LOGI("dvr: record %s wait cut short by a stop after %ums", what, time_ms() - t0);
            return;
        }

        if (time_ms() - t0 >= DVR_WAIT_MS)
            break;

        usleep(DVR_POLL_MS * 1000);
    }

    LOGE("dvr: record %s gave up after %ums, record process says %d (%s)%s", what, time_ms() - t0,
         status, dvr_status_name(status), want_recording && dvr_card_readonly() ? ", card mounted read-only" : "");
}

// dvr_stop_outstanding is left true when a stop was issued and not waited
// for. The next start is the one thing that has to see the old file closed,
// so it collects it.

static void dvr_wait_stopped(void) {
    if (!g_setting.speed.dvr_stop_wait) {
        sleep(2); // wait for record process
        return;
    }

    dvr_poll_status(false, "stop");
}

// was_running is the status from before the start command went out, and it
// should be anything but "recording". If it already said "recording" the file
// is stale -- an earlier run that never wrote its stop -- and polling for a
// value that is already there would return at once, so the flat wait stands.
static void dvr_wait_started(bool was_running) {
    if (!g_setting.speed.dvr_start_wait || was_running) {
        sleep(2); // wait for record process
        return;
    }

    dvr_poll_status(true, "start");
}

// A deferred stop is the record process still working on the file it has been
// told to close: it goes on taking frames from VI until it finalises, about
// three seconds after the recording started. Whatever the comment above says,
// not everything between the stop and the next start is its own -- pulling the
// video out from under it in that window lands in the file. A channel change
// leaves the pipeline as it was and only loses a moment of signal; closing the
// tuner for a bandwidth change, or switching the source outright, writes
// several seconds of a picture that is no longer there.
//
// So callers about to do one of those collect the stop first. It costs the
// wait that deferring saved, but only for a recording that was actually
// running, and only on the two actions that break it.
void dvr_collect_stop(void) {
    pthread_mutex_lock(&dvr_mutex);

    if (dvr_stop_outstanding) {
        dvr_poll_status(false, "stop collected");
        dvr_stop_outstanding = false;
    }

    pthread_mutex_unlock(&dvr_mutex);
}

void dvr_cmd(osd_dvr_cmd_t cmd) {
    LOGI("dvr_cmd: sdcard=%d, recording=%d, cmd=%d", g_sdcard_enable, dvr_is_recording, cmd);

    if (!g_sdcard_enable)
        return;

    if (cmd == DVR_STOP)
        dvr_stop_wanted = true;

    pthread_mutex_lock(&dvr_mutex);

    if (cmd == DVR_STOP)
        dvr_stop_wanted = false;

    bool start_rec = dvr_is_recording;

    switch (cmd) {
    case DVR_TOGGLE:
        start_rec = !dvr_is_recording && !record_pending;
        break;
    case DVR_STOP:
        start_rec = false;
        break;
    case DVR_START:
        start_rec = true;
        break;
    }

    if (!g_sdcard_enable) {
        record_pending = start_rec;
        return;
    }

    pthread_mutex_lock(&dvr_mutex);

    if (start_rec) {
        if (!dvr_is_recording && !sdcard_is_full()) {
            // A stop that was left to finish has to be finished now: this is
            // the only thing between here and there that needed the old file
            // closed. Polled rather than slept whatever the stop switch says,
            // since deferring the stop is what asked for this.
            if (dvr_stop_outstanding) {
                dvr_poll_status(false, "stop collected");
                dvr_stop_outstanding = false;
            }

            // Read before the command goes out, so the wait can tell a fresh
            // "recording" from one left behind by an earlier run. Not read at
            // all with the poll off, so that path is the original one exactly.
            bool was_running = g_setting.speed.dvr_start_wait &&
                               dvr_read_status() == DVR_STATUS_RECORDING;

            dvr_update_record_conf();
            if (g_sdcard_ready) {
                dvr_is_recording = true;
                record_pending = false;
                usleep(100 * 1000);
                system_script(REC_START);
                dvr_recording_start = time(NULL);
                // Held under dvr_mutex, which dvr_update_vi_conf() on the switch
                // path also wants, so this blocks the menu switch as well as the
                // thread it runs on.
                dvr_wait_started(was_running);

                if (dvr_read_status() == DVR_STATUS_RECORDING)
                    dvr_note_started();
            } else {
                record_pending = true;
            }
        }
    } else {
        if (dvr_is_recording) {
            dvr_is_recording = false;
            system_script(REC_STOP);

            // A recording that ran this long did start, whatever the wait
            // above saw at the time (a stop can cut it short).
            if (time(NULL) - dvr_recording_start > DVR_FAILED_START_S)
                dvr_note_started();

            if (g_setting.speed.dvr_defer_stop) {
                // Measured on the goggles: the record process will not
                // finalise a recording until about three seconds after it
                // started, so stopping sooner waits out the remainder. That is
                // the two seconds on a channel change, and on going to the
                // menu straight after the picture arrives.
                //
                // Nothing between here and the next recording needs the old
                // file closed -- the channel change, the display timing and
                // the vi conf write are all its own -- so the wait moves to
                // the start, which does need it.
                dvr_stop_outstanding = true;
                LOGI("dvr: record stop left to finish in the background");
            } else {
                // On the menu switch this runs with lvgl_mutex held, so
                // whatever it costs is time the menu is not drawn.
                dvr_wait_stopped();
            }
        }
    }

    pthread_mutex_unlock(&dvr_mutex);
}
