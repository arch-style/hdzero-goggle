#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <log/log.h>
#include <lvgl/lvgl.h>
#include <minIni.h>

#include "lang/language.h"

#ifdef EMULATOR_BUILD
#include "SDLaccess.h"
SDL_mutex *global_sdl_mutex;
#endif

#include "bmi270/accel_gyro.h"
#include "core/app_state.h"
#include "core/common.hh"
#include "core/elrs.h"
#include "core/ht.h"
#include "core/input_device.h"
#include "core/osd.h"
#include "core/self_test.h"
#include "core/settings.h"
#include "core/sleep_mode.h"
#include "core/thread.h"
#include "driver/beep.h"
#include "driver/dm5680.h"
#include "driver/esp32.h"
#include "driver/fans.h"
#include "driver/gpadc.h"
#include "driver/gpio.h"
#include "driver/hardware.h"
#include "driver/i2c.h"
#include "driver/it66021.h"
#include "driver/it66121.h"
#include "driver/mcp3021.h"
#include "driver/rtc.h"
#include "driver/screen.h"
#include "driver/tp2825.h"
#include "ui/page_power.h"
#include "ui/page_scannow.h"
#include "ui/page_source.h"
#include "ui/page_storage.h"
#include "ui/ui_image_setting.h"
#include "ui/ui_main_menu.h"
#include "ui/ui_osd_element_pos.h"
#include "ui/ui_porting.h"
#include "ui/ui_statusbar.h"
#include "util/sdcard.h"
#include "util/time.h"
#include "util/system.h"

int gif_cnt = 0;

static void *thread_autoscan(void *ptr) {
    log_thread_id("autoscan");
    for (;;) {
        pthread_mutex_lock(&lvgl_mutex);
        main_menu_show(true);
        app_state_push(APP_STATE_SUBMENU);
        submenu_enter();
        pthread_mutex_unlock(&lvgl_mutex);

        if (g_autoscan_exit)
            goto a_exit;

        sleep(5);

        if (g_autoscan_exit)
            goto a_exit;
    }

a_exit:
    pthread_exit(NULL);
    return NULL;
}

static int boot_source(void) {
    if (g_setting.autoscan.source == SETTING_AUTOSCAN_SOURCE_LAST)
        return g_setting.autoscan.last_source;

    return g_setting.autoscan.source;
}

// The menu is where start-up ends only for HDZero with auto scan off; every
// other route ends in video, and there the pages can be built afterwards.
static bool boot_ends_in_menu(void) {
    return boot_source() == SETTING_AUTOSCAN_SOURCE_HDZERO &&
           g_setting.autoscan.status == SETTING_AUTOSCAN_STATUS_OFF;
}

// True when start_running() will go straight to HDZero video, which is the
// one path that needs the tuner up as early as it can be.
static bool boot_goes_to_hdzero_video(void) {
    return boot_source() == SETTING_AUTOSCAN_SOURCE_HDZERO &&
           g_setting.autoscan.status == SETTING_AUTOSCAN_STATUS_LAST;
}

void start_running(void) {
    int source = boot_source();

    if (source == SETTING_AUTOSCAN_SOURCE_HDZERO) { // HDZero
        g_source_info.source = SOURCE_HDZERO;
        // go autoscan only if no dial up/down during initialization
        if ((g_setting.autoscan.status == SETTING_AUTOSCAN_STATUS_ON) && (g_init_done == 0)) {
            pthread_t pid;
            g_autoscan_exit = false;
            pthread_create(&pid, NULL, thread_autoscan, NULL);
        } else if (g_setting.autoscan.status == SETTING_AUTOSCAN_STATUS_LAST) {
            app_state_push(APP_STATE_VIDEO);
            app_switch_to_hdzero(true);
        } else { // auto scan disabled, go to go directly to last saved channel
            app_state_push(APP_STATE_MAINMENU);
            main_menu_show(true); // the menu is the destination here
        }
    } else {
        app_state_push(APP_STATE_VIDEO);
        if (source == SETTING_AUTOSCAN_SOURCE_AV_MODULE) { // AV Module
            g_hw_stat.av_pal[1] = g_setting.source.analog_format;
            app_switch_to_analog(0);
            g_source_info.source = SOURCE_AV_MODULE;
        } else if (source == SETTING_AUTOSCAN_SOURCE_AV_IN) { // AV in
            g_hw_stat.av_pal[0] = g_setting.source.analog_format;
            app_switch_to_analog(1);
            g_source_info.source = SOURCE_AV_IN;
        } else { // HDMI in
            sleep(2);
            // g_source_info.hdmi_in_status = IT66021_Sig_det();
            // if (g_source_info.hdmi_in_status) {
            app_switch_to_hdmi_in();
            g_source_info.source = SOURCE_HDMI_IN;
            //} else {
            //    g_source_info.source = SOURCE_HDZERO;
            //    app_state_push(APP_STATE_MAINMENU);
            //}
        }
    }

    if (g_setting.elrs.enable)
        enable_esp32();
}

// The motion sensor takes 686ms of I2C to come up and nothing between here
// and a picture needs it. The motion timer in ht.c would start reading it a
// second after ht_init() whether it was up or not, so that timer stays idle
// until ht_set_imu_ready() is called after the join below.
//
// Written by the worker, read after the join, so no barrier of its own.
static bool imu_up = false;

static void *imu_init_worker(void *arg) {
    (void)arg;
    log_thread_id("imu init");

    imu_up = enable_bmi270();

    return NULL;
}

static pthread_t imu_init_thread;
static bool imu_init_running = false;

// The tuner init and dispw are the two longest things in a start-up and
// neither is wanted before the switch, so both go on workers as early as
// there is anything for them to talk to. Everything after this point in
// start-up is either on another I2C bus or no bus at all.
static void boot_workers_start(void) {
    LOGI("hw: GOGGLE_VER_2=%d revision=%d", GOGGLE_VER_2, (int)getHwRevision());

    // DM6302_init() takes the main I2C bus to 1MHz for its whole run. Of the
    // devices on that bus the only one start-up still has to set up is
    // TP2825, which is why it is configured before this and the HDMI chips
    // after: IT66121 is on port 1 and IT66021 on port 3, so neither cares.
    if (g_setting.speed.async_tuner && boot_goes_to_hdzero_video())
        HDZero_open_async_start(g_setting.source.hdzero_bw);

    // dispw touches no bus at all, only the SoC's display output, so the one
    // thing it waits for is the panel blank at the start of
    // vdpo_start_timing_async(). Skip Display Setup is required or
    // Display_UI_init() runs a second dispw for the menu's own timing and the
    // switch a third; Skip Boot Menu is required because the panel then stays
    // dark until the video arrives rather than being lit halfway through the
    // reconfiguration.
    if (g_setting.speed.boot_display_early && boot_goes_to_hdzero_video()) {
        if (g_setting.speed.async_display && g_setting.speed.boot_display &&
            g_setting.speed.skip_boot_menu)
            start_display_timing_early();
        else
            LOGW("vdpo: Early Video Timing needs Async Display Setup, "
                 "Skip Display Setup and Skip Boot Menu; not starting early");
    }
}

static void device_init(void) {
    self_test();

    if (g_setting.speed.async_imu) {
        if (pthread_create(&imu_init_thread, NULL, imu_init_worker, NULL) == 0)
            imu_init_running = true;
        else
            imu_up = enable_bmi270();
    } else {
        imu_up = enable_bmi270();
    }

    // On the tuner's own bus, so before the workers raise its clock.
    TP2825_init(1, g_setting.source.analog_format);
    DM5680_req_ver();

    boot_workers_start();

    // On ports 1 and 3, and nothing before the switch reads them.
    IT66021_init();
    IT66121_init();
    fans_top_setspeed(g_setting.fans.top_speed);
}

void lvgl_init() {
    lv_init();
    style_init();
    lvgl_init_porting();
    lv_disp_t *dispp = lv_disp_get_default();
    lv_theme_t *theme = lv_theme_default_init(dispp, lv_color_make(0xff, 0xff, 0xff), lv_palette_main(LV_PALETTE_RED),
                                              false, LV_FONT_DEFAULT);
    lv_disp_set_theme(dispp, theme);
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_make(64, 64, 64), 0);
}

// lv_timer_handler() is where LVGL actually draws. Menu navigation is much
// heavier than video, and with the menu scaled to fit 720p every change
// inside it is rendered into a layer and then resampled, so log the passes
// that run long rather than guessing which part is slow.
//
// A line per slow pass was the first form of this, and with the menu scaled
// *every* pass is slow: 1985 lines and 143KB of a 305KB log, all of them
// saying 27ms. What the number is read for is how bad it gets and how much of
// the time, so report that instead, once per window, and say nothing at all
// while nothing is slow.
#define UI_DRAW_SLOW_MS   20
#define UI_DRAW_REPORT_MS 5000

static void ui_draw_timed(void) {
    static uint32_t window_ms = 0;
    static uint32_t window_passes = 0;
    static uint32_t slow_passes = 0;
    static uint32_t slow_worst = 0;

    uint32_t t0 = time_ms();

    lv_timer_handler();

    uint32_t dt = time_ms() - t0;

    if (window_ms == 0)
        window_ms = t0;

    window_passes++;

    if (dt >= UI_DRAW_SLOW_MS) {
        slow_passes++;
        if (dt > slow_worst)
            slow_worst = dt;
    }

    if (t0 - window_ms >= UI_DRAW_REPORT_MS) {
        if (slow_passes)
            LOGI("ui: %u of %u draws over %ums in %ums, worst %ums",
                 slow_passes, window_passes, UI_DRAW_SLOW_MS,
                 t0 - window_ms, slow_worst);

        window_ms = t0;
        window_passes = 0;
        slow_passes = 0;
        slow_worst = 0;
    }
}

int main(int argc, char *argv[]) {
    // First line of every log. Everything below it is read against a
    // particular binary -- which toggles exist, which bugs are fixed, which
    // measurements are comparable -- and a log with no build in it is a log
    // that has to be dated by guesswork. Kept to one line and one format so
    // it can be grepped out of somebody else's file.
    LOGI("build: %s-%s, %s %s", APP_BASE_VERSION, APP_BUILD_ID, __DATE__, __TIME__);

    // Anchored here so the total covers everything, device_init() included.
    // Started after that, it missed the very work Async Motion Sensor moves
    // and reported a 169ms change for what was really 641ms.
    g_boot_start_ms = time_ms();

    pthread_mutex_init(&lvgl_mutex, NULL);

#ifdef EMULATOR_BUILD
    global_sdl_mutex = SDL_CreateMutex();
    if (global_sdl_mutex == NULL) {
        LOGE("Failed to create an SDL mutex!");
    }
#endif

    // 1. Recall configuration
    settings_init();
    settings_load();

    // The two font sets are about 850ms of card reads and only settings_load()
    // has to have happened first, so they start here: in before anything asks,
    // and out of the way of the menu build they used to run alongside.
    osd_font_prefetch_start();

    // 2. Initialize communications.
    rtc_init();
    iic_init();
    gpio_init();
    uart_init();

#if defined(HDZGOGGLE2) || defined(HDZBOXPRO)
    gpadc_init();
#endif

    // 3. Initialize core devices. device_init() puts the tuner and the
    // display timing on workers partway through, as soon as the buses they
    // need are up, so everything below overlaps them.
    mcp3021_init();
    input_device_init();
    hw_stat_init();
    device_init();

    // No hardware in any of these, and nothing before this point needed them,
    // so they run after the workers are away rather than delaying them: it is
    // a tenth of a second of file parsing and logging.
    language_init();
#ifdef HDZGOGGLE2
    vclk_phase_init();
#endif
    pclk_phase_init();

    esp32_init();
    elrs_init();
    ht_init();
    beep_init();

    // 4. Initilize UI
    // Building the menu pages is the largest thing between here and a
    // picture and nothing needs them until someone opens the menu, so when
    // start-up is heading for video they are built afterwards instead.
    bool defer_menu = g_setting.speed.defer_menu && !boot_ends_in_menu();

    uint32_t phase_ms = time_ms();
    uint32_t step_ms = phase_ms;

    // Named so an i2c contention report can be read: everything from here to
    // the picture, the OLED and display set-up included, is this thread.
    log_thread_id("main");

    lvgl_init();
    LOGI("boot phase: lvgl %ums", time_ms() - step_ms);

    step_ms = time_ms();
    if (!defer_menu)
        main_menu_init();
    LOGI("boot phase: menu pages %ums%s", time_ms() - step_ms, defer_menu ? " (deferred)" : "");

    step_ms = time_ms();
    statusbar_init();
    lv_timer_handler();
    LOGI("boot phase: statusbar and first draw %ums", time_ms() - step_ms);
    LOGI("boot phase: ui %ums", time_ms() - phase_ms);

    // 5. Prepare Display
    // Broken out step by step: one boot in six has spent ten seconds
    // somewhere in here while the tuner worker was equally stuck in its own
    // I2C, and the single phase total could not say where.
#define BOOT_STEP(name, call)                        \
    do {                                             \
        step_ms = time_ms();                         \
        call;                                        \
        LOGI("boot step: " name " %ums",             \
             time_ms() - step_ms);                   \
    } while (0)

    phase_ms = time_ms();
    // Everything below reaches the OLED through the FPGA on the main I2C bus,
    // and Async Tuner Init has that bus at 1MHz until its init finishes -- a
    // speed the FPGA does not answer at. Overlapping the two does not divide
    // the bus between them, it slows every transfer on it by about a thousand
    // times: one boot spent 9955ms in screen.start_up() and 10147ms on the M0
    // load that was running beside it, for work that takes 94ms and 92ms when
    // they do not meet. Waiting is free on the boots where the worker has
    // already finished, which is most of them.
    BOOT_STEP("wait for the tuner bus", HDZero_open_async_wait());
    BOOT_STEP("oled startup", screen.start_up());
    BOOT_STEP("display ui init", Display_UI_init());
    BOOT_STEP("oled pattern", screen.pattern(0, 0, 0));
    BOOT_STEP("osd init", osd_init());
    BOOT_STEP("ims init", ims_init());
    BOOT_STEP("osd element pos", ui_osd_element_pos_init());
    LOGI("boot phase: display and osd %ums", time_ms() - phase_ms);

#undef BOOT_STEP

    // 6. Enable functionality
    phase_ms = time_ms();
    if (g_setting.ht.enable) {
        ht_enable();
    } else {
        ht_disable();
    }
    LOGI("boot phase: head tracker %ums", time_ms() - phase_ms);

    // 7 set initial analog module power state
    Analog_Module_Power(1); // must before start_running()

    // The motion timer armed in ht_init() has been firing since a second after
    // that call, but it does nothing until told the sensor is up. Wait for the
    // async init if there was one, then open the gate. The gate is opened on
    // every path, async or not: without it the head tracker never reads.
    if (imu_init_running) {
        step_ms = time_ms();
        pthread_join(imu_init_thread, NULL);
        imu_init_running = false;
        LOGI("boot phase: waited %ums for the motion sensor", time_ms() - step_ms);
    }
    // Only if it actually came up. get_bmi270() polls a data-ready flag in an
    // unbounded loop with the I2C mutex held, so a 100Hz timer reading a
    // sensor that never answers is a thread that never returns, on the bus the
    // FPGA and the tuner also use.
    if (imu_up)
        ht_set_imu_ready();
    else
        LOGE("motion sensor did not come up, head tracking stays idle");

    // 8. Start threads
    g_bootup_sdcard_state = sdcard_mounted() ? 1 : 0;
    start_running();

    // Every path to a picture collects the background timing change on the
    // way, but the panel stays dark until something does, so make sure. Costs
    // nothing when there is nothing outstanding.
    vdpo_timing_collect();

    if (defer_menu) {
        step_ms = time_ms();
        main_menu_init();
        // Created after the OSD screen now, so it would be drawn over the
        // video. Put it back underneath.
        main_menu_move_behind_osd();
        LOGI("boot phase: menu pages after video %ums", time_ms() - step_ms);
    }

    // This one is the end of the switch, not the picture. They used to be the
    // same moment; they stopped being it when the audio and DVR set-up moved
    // behind the picture, so the number to compare boots by is the "boot:
    // picture at" line that app_switch_to_hdzero() logs.
    LOGI("boot total: app start to switch done %ums", time_ms() - g_boot_start_ms);

    create_threads();

    // 8. Synthetic counter for gif refresh
    gif_cnt = 0;

    // 8.1 set initial analog module power state
    Analog_Module_Power(0);

    // Head alarm
    head_alarm_init();

    // 10. Execute main loop
    g_init_done = 1;
    for (;;) {
        // The status bar and the source status page rewrite their labels on
        // every pass, and a rewrite invalidates them whether the text changed
        // or not, so at one pass per 5ms they were being redrawn around 200
        // times a second. Nothing on them changes that fast, and the redraws
        // compete for lvgl_mutex with the dial and button handlers.
        bool slow_tick = true;

        if (g_setting.speed.ui_throttle) {
            static uint32_t last_slow_ms = 0;
            uint32_t now = time_ms();

            slow_tick = (uint32_t)(now - last_slow_ms) >= 50;
            if (slow_tick)
                last_slow_ms = now;
        }

        // The dial and button handlers take lvgl_mutex too, so held across the
        // whole batch it makes them wait for a full pass. Released between the
        // stages they get several chances per pass instead. Each stage still
        // runs under the lock, so LVGL is never touched unprotected; what
        // changes is that input can land between two stages rather than only
        // between passes.
        bool split = g_setting.speed.split_lock;

#define UI_STAGE(call)                         \
    do {                                       \
        if (split)                             \
            pthread_mutex_lock(&lvgl_mutex);   \
        call;                                  \
        if (split)                             \
            pthread_mutex_unlock(&lvgl_mutex); \
    } while (0)

        if (!split)
            pthread_mutex_lock(&lvgl_mutex);

        UI_STAGE(main_menu_update());
        UI_STAGE(sleep_reminder());
        if (slow_tick)
            UI_STAGE(statubar_update());
        UI_STAGE(osd_hdzero_update());
        UI_STAGE(ims_update());
        UI_STAGE(ui_osd_element_pos_update());
        UI_STAGE(ht_detect_motion());
        UI_STAGE(ui_draw_timed());
        if (slow_tick)
            UI_STAGE(source_status_timer());

        if (!split)
            pthread_mutex_unlock(&lvgl_mutex);

#undef UI_STAGE

        usleep(5000);
        gif_cnt++;
    }
    return 0;
}
