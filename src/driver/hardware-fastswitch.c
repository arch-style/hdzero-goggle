// The fast-switch fork's display-timing and tuner machinery, shared by the
// goggle 2 and the goggle v1: dispw run on a worker and collected where the
// timing is needed, DM6302_init() run on a worker at start-up, the tuner held
// in standby over the menu, and an init that fails left closed to be retried.
// It was written in hardware-goggle2.c and moved here unchanged; the v1 had
// stubs that ran everything inline and made standby a close.
#include "hardware.h"

#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <log/log.h>

#include "../core/msp_displayport.h"
#include "../core/settings.h"
#include "../util/system.h"
#include "../util/time.h"
#include "dm5680.h"
#include "dm6302.h"
#include "screen.h"

extern pthread_mutex_t hardware_mutex;

// dispw reconfigures the display pipeline and measured 1.1s on this SoC, so
// never run it to set the timing the display already has. Callers used to
// exec it unconditionally and then record the result in g_hw_stat.vdpo_tmg;
// this keeps that record and adds the guard. The first call always execs,
// because the boot-time value of vdpo_tmg is an assumption, not an
// observation.
static bool vdpo_applied_once = false;

bool vdpo_timing_applied(void) {
    return vdpo_applied_once;
}

static bool vdpo_pending = false;

bool vdpo_timing_pending(void) {
    return vdpo_pending;
}

// vdpo_pending only says a worker was started and not yet joined; it stays
// true long after dispw itself has exited, because the join happens at the
// point the timing is needed. Anyone deciding what to do *while* dispw runs
// needs to know whether the child is actually still there, so the worker
// clears this the moment system_exec() returns. A stale read costs one
// ordering decision, never correctness, so a plain flag is enough.
static volatile bool vdpo_child_running = false;

bool vdpo_timing_running(void) {
    return vdpo_child_running;
}

// dispw is over a second and it configures the SoC's display output, which
// has nothing to do with the tuner coming up on the FPGA's I2C. Run it on a
// worker started before the tuner init, and collect it where the timing is
// actually needed. The panel is blanked first, because the stock order blanks
// before changing the mode and a mode change on a live panel is not something
// to find out about the hard way.
static pthread_t vdpo_thread;
static vdpo_tmg_t vdpo_pending_tmg;
static char vdpo_pending_mode[16];

static void *vdpo_worker(void *arg) {
    log_thread_id("display timing");
    char buf[64];

    (void)arg;
    snprintf(buf, sizeof(buf), "dispw -s vdpo %s", vdpo_pending_mode);
    system_exec(buf);
    vdpo_child_running = false;

    return NULL;
}

static void vdpo_collect_locked(void);

// For a caller that just wants any outstanding timing change finished, with
// no timing of its own to ask for. Does nothing when none is outstanding.
void vdpo_timing_collect(void) {
    pthread_mutex_lock(&hardware_mutex);
    vdpo_collect_locked();
    pthread_mutex_unlock(&hardware_mutex);
}

static void vdpo_collect_locked(void) {
    if (!vdpo_pending)
        return;

    pthread_join(vdpo_thread, NULL);
    vdpo_pending = false;
    vdpo_applied_once = true;
    g_hw_stat.vdpo_tmg = vdpo_pending_tmg;
}

void vdpo_start_timing_async(vdpo_tmg_t tmg, const char *mode) {
    // Under hardware_mutex like every other display change: vdpo_set_timing()
    // reads and clears this same state, and the source detect thread reaches
    // it through Display_720P60_50_t() while this runs from the switch path.
    pthread_mutex_lock(&hardware_mutex);

    if (vdpo_pending)
        goto done;

    if (vdpo_applied_once && g_hw_stat.vdpo_tmg == tmg)
        goto done; // already there, nothing to run

    screen.display(0);

    snprintf(vdpo_pending_mode, sizeof(vdpo_pending_mode), "%s", mode);
    vdpo_pending_tmg = tmg;

    // Raised before the create so the worker cannot clear it first.
    vdpo_child_running = true;
    if (pthread_create(&vdpo_thread, NULL, vdpo_worker, NULL) != 0) {
        vdpo_child_running = false;
        LOGE("vdpo: could not start the async timing change");
        goto done;
    }

    vdpo_pending = true;
    LOGI("vdpo: %s started in the background", mode);

done:
    pthread_mutex_unlock(&hardware_mutex);
}

void vdpo_set_timing(vdpo_tmg_t tmg, const char *mode) {
    // g_hw_stat.vdpo_tmg is the state of record: the HDMI-in paths that still
    // exec dispw directly assign it too, so reading it here cannot go stale.
    char buf[64];

    if (vdpo_pending) {
        // Whatever is in flight has to finish before another one starts, and
        // if it was this one there is nothing left to do.
        vdpo_tmg_t started = vdpo_pending_tmg;

        vdpo_collect_locked();
        if (started == tmg) {
            LOGI("vdpo: collected %s", mode);
            return;
        }
    }

    if (vdpo_applied_once && g_hw_stat.vdpo_tmg == tmg) {
        LOGI("vdpo: already %s", mode);
        return;
    }

    snprintf(buf, sizeof(buf), "dispw -s vdpo %s", mode);
    system_exec(buf);
    vdpo_applied_once = true;
    g_hw_stat.vdpo_tmg = tmg;
}

// DM6302_init() is 1.8s of I2C to the FPGA and nothing in the boot UI phase
// needs the tuner, so start-up can run it on a worker alongside that phase.
// Every entry point that touches the tuner collects the worker first, and
// HDZero_open() then finds the tuner already open and does nothing more.
static pthread_t hdz_async_thread;
static bool hdz_async_pending = false;
static int hdz_async_bw;

// The worker goes through HDZero_open() and, on a bandwidth change,
// HDZero_Close(), both of which collect the worker first. It must recognise
// itself there: joining oneself does not fail on musl, it waits forever, and
// the first build did exactly that while the main thread ran the init
// inline, none the wiser.
static __thread bool hdz_in_worker = false;

// Set when the worker gave up on a tuner that would not answer, so the main
// thread's own HDZero_open() right afterwards does not spend a second retry
// on it: four ten-cycle DM6302_init() runs back to back is the best part of
// ten seconds of nothing. Cleared again as soon as an open gets anywhere.
static bool hdz_async_init_failed = false;

// /mnt/extsd/early-tune (card experiments): tune the boot channel here, as
// soon as the receivers are up, instead of at the switch some 100 ms later.
// On video the received picture follows the panel lighting by about a
// quarter of a second, and if that is the link coming up after the tune, it
// can come up while the main thread is still building the UI. The switch
// then skips its own DM6302_SetChannel(): that rewrites the PLL and restarts
// its calibration (0x118 strobes), which would throw the head start away.
#define EARLY_TUNE_FLAG "/mnt/extsd/early-tune"
static int hdz_early_tuned = -1; // (band << 8) | ch, or -1

static void hdz_early_tune(void) {
    if (access(EARLY_TUNE_FLAG, F_OK) != 0 || !g_hw_stat.hdzero_open)
        return;

    uint8_t band = g_setting.source.hdzero_band;
    uint8_t ch = (g_setting.scan.channel - 1) & 0x7f;
    uint32_t t0 = time_ms();

    DM6302_SetChannel(band, ch);
    DM5680_clear_vldflg();
    DM5680_req_vldflg();
    hdz_early_tuned = (band << 8) | ch;
    LOGI("HDZero: early tune band %u ch %u in the worker (%ums)", band, ch, time_ms() - t0);
    DM5680_link_watch_start();
}

// /mnt/extsd/early-fps (card experiments): send the camera mode to the
// baseband (DM5680_SetFPS) here too, from the card's note of the last VTX
// (cam-mode.txt), instead of only in Display_720P60_50_t() at the switch.
// Boots 282-283 had the link valid 21-22 ms after the tune, before the panel
// lit, and OSD packets flowing right after it, yet the picture came about
// 0.27 s after the panel: the wait is after the link. If it is the baseband
// settling into the mode it was just given, giving it the mode early moves
// that wait off the path. The switch skips its own SetFPS for the same mode.
#define EARLY_FPS_FLAG "/mnt/extsd/early-fps"
static int hdz_early_fps = -1;

static void hdz_early_fps_send(void) {
    if (access(EARLY_FPS_FLAG, F_OK) != 0 || !g_hw_stat.hdzero_open)
        return;

    DM5680_SetFPS(CAM_MODE);
    hdz_early_fps = CAM_MODE;
    LOGI("HDZero: early SetFPS %d in the worker", (int)CAM_MODE);
}

bool HDZero_take_early_fps(int mode) {
    bool hit = hdz_early_fps == mode;

    hdz_early_fps = -1;
    return hit;
}

bool HDZero_take_early_tune(uint8_t band, uint8_t ch) {
    bool hit = hdz_early_tuned == ((band << 8) | ch);

    hdz_early_tuned = -1;
    return hit;
}

static void *hdz_async_worker(void *arg) {
    (void)arg;

    hdz_in_worker = true;
    log_thread_id("tuner init");

    // Behind the main thread for the CPU: the UI build is on the critical
    // path and the init mostly waits on the bus anyway. Linux applies the
    // priority per thread when given the thread id.
    setpriority(PRIO_PROCESS, syscall(SYS_gettid), 10);

    HDZero_open(hdz_async_bw);
    hdz_early_tune();
    hdz_early_fps_send();

    return NULL;
}

bool HDZero_open_pending(void) {
    return hdz_async_pending;
}

static void hdz_async_collect(void) {
    if (!hdz_async_pending || hdz_in_worker)
        return;

    pthread_join(hdz_async_thread, NULL);
    hdz_async_pending = false;
    LOGI("HDZero: async open collected");
}

// DM6302_init() holds the main I2C bus at 1MHz for its whole run, and the FPGA
// the OLED is reached through does not take 1MHz. When the two overlap, every
// transfer on that port slows by about a thousand times and both sides crawl:
// measured on the goggles, a boot where the OLED start-up landed in the middle
// of the M0 image load spent 9955ms on what is normally 94ms, while the same
// M0 load took 10147ms for its normal 92ms. Total boot 12957ms against 2400ms.
//
// So anything on the main thread that talks to the OLED during start-up waits
// for the worker first. Costs nothing on the boots where it has already
// finished, which is most of them. No hardware_mutex, matching every other
// caller of hdz_async_collect(): the worker being joined runs HDZero_open(),
// and taking a lock it might want would be a way to wait forever.
void HDZero_open_async_wait(void) {
    hdz_async_collect();
}

void HDZero_open_async_start(int bw) {
    if (hdz_async_pending)
        return;

    hdz_async_bw = bw;
    // Raised before the create so the worker sees it from its first line.
    hdz_async_pending = true;
    if (pthread_create(&hdz_async_thread, NULL, hdz_async_worker, NULL) != 0) {
        hdz_async_pending = false;
        LOGE("HDZero: could not start the async open, it will run inline");
        return;
    }

    LOGI("HDZero: async open started");
}

void HDZero_open(int bw) {
    hdz_async_collect();

    if (bw != g_hw_stat.hdz_bw) // reopen with different bw
        HDZero_Close();

    if (g_hw_stat.hdzero_open == 0) {
        g_hw_stat.hdz_bw = bw;
        DM5680_SetBR(g_hw_stat.hdz_bw);

        // DM6302_init() gives up after ten tries and returns non-zero, and
        // the receivers are then unconfigured: no picture, or noise, or one
        // module dead and its two antennas with it. Marking the tuner open
        // anyway told the rest of the app it was fine and left nothing to
        // retry. Leaving it closed means the next switch tries again.
        int init_failed = DM6302_init(0, g_hw_stat.hdz_bw);

        if (init_failed && g_setting.bugfix.retry_tuner_init && !hdz_async_init_failed) {
            // Straight away, because at boot there is no next switch to wait
            // for: without this the picture stays wrong until the user
            // happens to open the menu and come back. One extra attempt only,
            // since DM6302_init() has already cycled the reset ten times and
            // each attempt costs a couple of seconds.
            LOGE("HDZero: receivers did not come up, trying once more");
            init_failed = DM6302_init(0, g_hw_stat.hdz_bw);
        }

        if (init_failed && g_setting.bugfix.retry_tuner_init) {
            LOGE("HDZero: receivers still not up, leaving closed to retry");
            g_hw_stat.hdzero_open = 0;
            g_hw_stat.hdz_standby = 0;
            // Only the worker's failure carries forward; a failure here has
            // already skipped its retry, so the next switch starts clean.
            hdz_async_init_failed = hdz_in_worker;
            return;
        }

        hdz_async_init_failed = false;
        DM5680_SetBB(1);
        g_hw_stat.hdzero_open = 1;
        g_hw_stat.hdz_standby = 0;
    } else if (g_hw_stat.hdz_standby) {
        // Back from standby: DM6302 kept its configuration and its M0 image,
        // so only the baseband has to be restarted. This is the whole point
        // of standby -- DM6302_init() is around a thousand SPI writes and a
        // 100ms reset wait.
        DM5680_SetBB(1);
        g_hw_stat.hdz_standby = 0;
    }
    LOGI("HDZero: open");
}

void HDZero_Close() {
    hdz_async_collect();
    hdz_early_tuned = -1;
    hdz_early_fps = -1;

    DM5680_SetBB(0);
    DM5680_ResetRF(0);
    g_hw_stat.hdzero_open = 0;
    g_hw_stat.hdz_standby = 0;
    g_hw_stat.m0_open = 0;

    LOGI("HDZero: close");
}

// Like HDZero_Close(), but leaves DM6302 out of reset and configured so that
// the next HDZero_open() is a single command instead of a full re-init. The
// tuner stays powered meanwhile, so this is only for short absences such as
// the menu being open -- never for sleep.
void HDZero_Standby() {
    hdz_async_collect();

    if (g_hw_stat.hdzero_open == 0) {
        // Nothing configured to hold on to; a close is all this can mean.
        // This also covers an init that failed: holding on to a receiver that
        // never came up would keep a bad picture bad, where a close makes the
        // next switch initialise from scratch.
        HDZero_Close();
        return;
    }

    DM5680_SetBB(0);
    g_hw_stat.hdz_standby = 1;

    LOGI("HDZero: standby");
}

#endif
