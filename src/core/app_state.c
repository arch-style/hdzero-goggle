#include "app_state.h"

#include <log/log.h>
#include <minIni.h>
#include <stdlib.h>
#include <unistd.h>

#include "core/common.hh"
#include "core/dvr.h"
#include "core/input_device.h"
#include "core/msp_displayport.h"
#include "core/osd.h"
#include "core/settings.h"
#include "driver/dm5680.h"
#include "driver/dm6302.h"
#include "driver/hardware.h"
#include "driver/it66121.h"
#include "driver/rtc6715.h"
#include "driver/screen.h"
#include "ui/page_common.h"
#include "ui/page_imagesettings.h"
#include "ui/ui_image_setting.h"
#include "ui/ui_main_menu.h"
#include "ui/ui_porting.h"
#include "util/system.h"
#include "util/time.h"

app_state_t g_app_state = APP_STATE_MAINMENU;

extern int valid_channel_tb[10];
extern int user_select_index;

void app_state_push(app_state_t state) {
    g_app_state = state;

    // Menu Antialias OFF is scoped to the menu being on screen, and the state
    // is what says so. Costs a compare and a store.
    main_menu_apply_antialiasing();
}

// True while the menu is drawn over a running picture rather than having taken
// the display for itself. Set by the switch below, cleared by leaving the menu
// or by app_menu_end_overlay().
static bool menu_over_video = false;

// Menu Over Video leaves the FPGA selecting the live source and the panel on
// that source's timing, with the menu drawn into the UI layer over the top.
// That is all the menu needs and it is not enough for anything that plays
// video of its own: the player builds a 1920x1080 layer and hands its frames
// to the SoC's VO, which is not what the display is showing or the size it is
// showing it at, so the picture comes out sheared and the wrong scale.
//
// This finishes the switch the overlay skipped. Leaving the menu afterwards
// goes through app_switch_to_hdzero(), which sets the timing and reopens the
// tuner whatever state they were left in, so there is nothing to put back.
void app_menu_end_overlay(void) {
    if (!menu_over_video)
        return;

    menu_over_video = false;
    LOGI("switch mark: ending the overlay");

    Display_UI();
    lvgl_switch_to_1080p();
    // The menu was fitted to the 720p screen on the way in and the display has
    // just become 1080p underneath it. Its zoom is worked out once, in
    // main_menu_show(), so without this the page carries on drawing at two
    // thirds size in the top left corner of a screen half as big again.
    main_menu_refit_display();

    if (g_setting.speed.fast_menu)
        HDZero_Standby();
    else
        HDZero_Close();

    Analog_Module_Power(0);
    LOGI("switch mark: overlay ended");
}

// The part of the menu switch that is about recording and audio rather than
// the picture: independent of the display timing, so it can run either side
// of the wait for it. The mirror of hdzero_recording_side() below.
static void menu_recording_side(void) {
    dvr_enable_line_out(false);
    LOGI("switch mark: sources off");
    system_script(REC_STOP_LIVE);
}

void app_switch_to_menu() {
    if (g_app_state == APP_STATE_IMS) {
        ims_save();
        set_slider_value();
    }

    app_state_push(APP_STATE_MAINMENU);
    LOGI("switch mark: to_menu start");

    // Switching the display source to UI on its own is what forces the 1080p50
    // rebuild, and leaving the panel on the video timing while doing it tore
    // the picture into stripes. So with keep_display do not switch the source
    // at all: leave the pipeline composing video with the UI layer over it,
    // exactly as it does for the OSD, and let the menu draw into that layer.
    // The menu is laid out for 1080p, so at 720p it is cropped.
    bool overlay = g_setting.speed.keep_display && vdpo_timing_applied();

    menu_over_video = overlay;

    // The menu is 1080p50 whatever the video was, so unlike the video
    // direction there is nothing to work out: the timing can be asked for
    // before anything else here. Display_UI() collects it further down, where
    // it would otherwise have started it, and everything between the two runs
    // beside dispw instead of in front of it. The panel blanks here rather
    // than at Display_UI(), so the video goes at the button press.
    if (!overlay && g_setting.speed.menu_async_display)
        vdpo_start_timing_async(VDPO_TMG_1080P50, "1080p50");

    // Stop recording if switching to menu mode from video mode regardless
    dvr_cmd(DVR_STOP);

#if defined HDZGOGGLE
    dvr_update_vi_conf(VR_1080P30);
#elif defined HDZBOXPRO
    dvr_update_vi_conf(VR_720P60);
#elif defined HDZGOGGLE2
    dvr_update_vi_conf(VR_1080P30);
#endif
    LOGI("switch mark: dvr stopped");

    // Same liveness test as the video direction, and for the same reason:
    // vdpo_timing_pending() stays true until the join, so testing that would
    // put this in front of the menu on the runs where dispw had already
    // finished, which is exactly what it is here to avoid.
    bool side_done = false;

    if (!overlay && g_setting.speed.menu_async_display && vdpo_timing_running()) {
        LOGI("switch mark: audio and live stop while the display changes");
        menu_recording_side();
        side_done = true;
    }

    if (!overlay) {
        Display_UI();
        lvgl_switch_to_1080p();
    }
    LOGI("switch mark: display to UI");
    exit_tune_channel();
    osd_show(false);
    g_bShowIMS = false;
    main_menu_show(true);
    LOGI("switch mark: menu shown");
    // Overlaying the menu only means anything if the picture underneath keeps
    // running, so leave the tuner alone entirely in that case. Otherwise:
    // resetting the tuner here is what makes coming back cost a full
    // DM6302_init(), measured at 2.3s. Standby skips that at the price of
    // leaving it powered.
    if (overlay)
        ; // video keeps playing under the menu
    else if (g_setting.speed.fast_menu)
        HDZero_Standby();
    else
        HDZero_Close();
    LOGI("switch mark: rf off");
    g_sdcard_det_req = 1;
    if (g_source_info.source == SOURCE_HDMI_IN) // HDMI
        IT66121_init();

    if (!overlay) { // the picture underneath has to keep its source powered
        rtc6715.init(0, 0);
        Analog_Module_Power(0);
    }

    if (!side_done)
        menu_recording_side();
    LOGI("switch mark: to_menu done");
}

void app_exit_menu() {
    // Whatever the overlay left behind, the switch below sets it.
    menu_over_video = false;

    if (SOURCE_HDZERO == g_source_info.source) {
        progress_bar.start = 1;
        app_switch_to_hdzero(true);
    } else if (SOURCE_HDMI_IN == g_source_info.source) {
        app_switch_to_hdmi_in();
    } else {
        app_switch_to_analog(g_source_info.source == SOURCE_AV_IN);
    }
}

void app_switch_to_analog(bool is_av_in) {
    dvr_update_vi_conf(VR_720P50);
    osd_fhd(0);
    osd_show(true);
    lvgl_switch_to_720p();
    osd_clear();
    lv_timer_handler();
    Display_Osd(g_setting.record.osd);

    Source_AV(is_av_in);

    if (is_av_in) {
        rtc6715.init(0, 0);
    } else {
        rtc6715.init(1, g_setting.record.audio_source == SETTING_RECORD_AUDIO_SOURCE_AV_IN);
        rtc6715.set_ch(g_setting.source.analog_channel - 1);
    }

    g_setting.autoscan.last_source = is_av_in ? SETTING_AUTOSCAN_SOURCE_AV_IN : SETTING_AUTOSCAN_SOURCE_AV_MODULE;
    ini_putl("autoscan", "last_source", g_setting.autoscan.last_source, SETTING_INI);

    // audio in&out
    dvr_select_audio_source(g_setting.record.audio_source);
    dvr_enable_line_out(true);

    sleep(1);
    system_script(REC_STOP_LIVE);
}

void app_switch_to_hdmi_in() {
#if defined HDZBOXPRO
    // Restore image settings from av module
    screen.brightness(g_setting.image.oled);
    Set_Contrast(g_setting.image.contrast);
#endif

    rtc6715.init(0, 0);

    Source_HDMI_in();
    IT66121_close();
    sleep(2);

    if (g_hw_stat.hdmiin_vtmg == HDMIIN_VTMG_1080P60 ||
        g_hw_stat.hdmiin_vtmg == HDMIIN_VTMG_1080P50 ||
        g_hw_stat.hdmiin_vtmg == HDMIIN_VTMG_1080Pother)
        lvgl_switch_to_1080p();
    else
        lvgl_switch_to_720p();

    osd_show(true);
    osd_clear();
    lv_timer_handler();

    // update vi conf at HDMI_in_detect()
    // dvr_update_vi_conf((g_hw_stat.hdmiin_vtmg == 1) ? VR_1080P30 : VR_720P60);

    app_state_push(APP_STATE_VIDEO);
    g_source_info.source = SOURCE_HDMI_IN;
    dvr_enable_line_out(false);
    g_setting.autoscan.last_source = SETTING_AUTOSCAN_SOURCE_HDMI_IN;
    ini_putl("autoscan", "last_source", g_setting.autoscan.last_source, SETTING_INI);

    system_script(REC_STOP_LIVE);
}

//////////////////////////////////////////////////////////////////////
// is_default:
//    true = load from g_settings
//    false = user selected from auto scan page
// Which display timing the camera mode below will ask for. Mirrors the switch
// further down, so the two have to stay in step; getting it wrong only costs
// the head start, since vdpo_set_timing() runs the right one either way.
void start_display_timing_early(void) {
    switch (CAM_MODE) {
    case VR_720P50:
    case VR_720P60:
    case VR_960x720P60:
    case VR_540P60:
        vdpo_start_timing_async(VDPO_TMG_720P60, "720p60");
        break;

    case VR_540P90:
    case VR_540P90_CROP:
        vdpo_start_timing_async(VDPO_TMG_720P90, "720p90");
        break;

    case VR_1080P30:
    case VR_1080P24:
        vdpo_start_timing_async(VDPO_TMG_1080P60, "1080p60");
        break;

    default:
        break;
    }
}

// The part of the HDZero switch that is about recording and audio rather than
// the picture: independent of the display timing, so it can run either side
// of the wait for it.
static void hdzero_recording_side(void) {
    g_setting.autoscan.last_source = SETTING_AUTOSCAN_SOURCE_HDZERO;
    ini_putl("autoscan", "last_source", g_setting.autoscan.last_source, SETTING_INI);

    dvr_select_audio_source(g_setting.record.audio_source);
    dvr_enable_line_out(false);

    dvr_update_vi_conf(CAM_MODE);
    LOGI("switch mark: dvr configured");
    system_script(REC_STOP_LIVE);
}

// The Wide/Narrow button was the way to run the tuner init on demand, and it
// is a poor instrument: every other press lands on the bandwidth you did not
// want, and the init it runs is wrapped in a full source switch. This is the
// init and nothing else -- the three calls app_switch_to_hdzero() makes for
// the receivers, on a display that stays where it is.
void app_tuner_reinit(void) {
    if (g_source_info.source != SOURCE_HDZERO) {
        LOGI("tuner: re-init asked for, but the source is not HDZero");
        return;
    }

    uint32_t started_ms = time_ms();
    LOGI("tuner: re-init on request");

    // The receivers go away for the better part of a second, and a recording
    // still being finalised would take that. Same rule as the Wide/Narrow
    // button.
    dvr_cmd(DVR_STOP);
    if (g_setting.bugfix.wait_for_recording)
        dvr_collect_stop();

    HDZero_Close();
    HDZero_open(g_setting.source.hdzero_bw);

    DM6302_SetChannel(g_setting.source.hdzero_band, (g_setting.scan.channel - 1) & 0x7f);
    DM5680_clear_vldflg();
    DM5680_req_vldflg();

    // The display never left the HDZero source, but the close took the M0
    // with it; this is what the display switch does for it on a real switch.
    Display_VO_SWITCH(1);

    LOGI("tuner: re-init took %ums", time_ms() - started_ms);
}

void app_switch_to_hdzero(bool is_default) {
    int ch;

#if defined HDZBOXPRO
    // Restore image settings from av module
    screen.brightness(g_setting.image.oled);
    Set_Contrast(g_setting.image.contrast);
#endif

    rtc6715.init(0, 0);
    LOGI("switch mark: to_hdzero start");

    system_exec("aww 0x0300b084 0x00001555"); // Set vdpo clock driver strength to level 2. Refer datasheet 12.7.5.11

    // After the clock drive strength, which used to run before dispw and now
    // has no reason not to, and before the tuner, so the two run together
    // instead of one after the other. Display_720P60_50() and friends collect
    // it where they would otherwise have started it.
    if (g_setting.speed.async_display)
        start_display_timing_early();
    Analog_Module_Power(0);
    LOGI("switch mark: aww + analog power");

    if (is_default) {
        ch = g_setting.scan.channel - 1;
    } else {
        ch = valid_channel_tb[user_select_index];
        g_setting.scan.channel = ch + 1;
        ini_putl("scan", "channel", g_setting.scan.channel, SETTING_INI);
    }

    HDZero_open(g_setting.source.hdzero_bw);
    LOGI("switch mark: rf open");
    ch &= 0x7f;

    LOGI("switch to bw:%d, band:%d, ch:%d, CAM_MODE=%d 4:3=%d", g_setting.source.hdzero_bw, g_setting.source.hdzero_band, g_setting.scan.channel, CAM_MODE, cam_4_3);
    DM6302_SetChannel(g_setting.source.hdzero_band, ch);
    DM5680_clear_vldflg();
    DM5680_req_vldflg();
    progress_bar.start = 0;
    LOGI("switch mark: channel tuned");

    // The audio and recorder set-up below is half a second of forked
    // scripts that has nothing to do with the display, so it is free as long
    // as dispw is still running and the switch would be waiting anyway. The
    // test has to be liveness, not vdpo_timing_pending(): that stays true
    // until the join, and doing this after dispw had already exited put the
    // whole half second in front of the picture instead of beside it.
    bool side_done = false;

    if (g_setting.speed.async_display && vdpo_timing_running()) {
        LOGI("switch mark: audio and dvr while the display changes");
        hdzero_recording_side();
        side_done = true;
    }

#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
    switch (CAM_MODE) {
    case VR_720P50:
    case VR_720P60:
    case VR_960x720P60:
    case VR_540P60:
        Display_720P60_50(CAM_MODE, cam_4_3);
        break;

    case VR_540P90:
    case VR_540P90_CROP:
        Display_720P90(CAM_MODE);
        break;

    case VR_1080P30:
        Display_1080P30(CAM_MODE);
        break;
    case VR_1080P24:
        Display_1080P24(CAM_MODE);
        break;
    default:
        perror("switch_to_video CaM_MODE error");
        break;
    }

    // Every case above collects the background timing change, except the one
    // that recognises no camera mode at all. Nothing else would, and until
    // something does the panel stays dark, so close that off here. A no-op
    // on every normal path.
    vdpo_timing_collect();

    channel_osd_mode = CHANNEL_SHOWTIME;
    LOGI("switch mark: display mode set");

    // The moment there is a picture, which is the number worth comparing
    // boots by: everything after this point the pilot does not wait for.
    if (g_init_done == 0)
        LOGI("boot: picture at %ums", time_ms() - g_boot_start_ms);

    if (CAM_MODE == VR_1080P30 || CAM_MODE == VR_1080P24)
        lvgl_switch_to_1080p();
    else
        lvgl_switch_to_720p();
    osd_fhd(CAM_MODE == VR_1080P30 || CAM_MODE == VR_1080P24);
#elif defined HDZBOXPRO
    Display_HDZ(CAM_MODE, cam_4_3);
    channel_osd_mode = CHANNEL_SHOWTIME;
    lvgl_switch_to_720p();
    LOGI("lvgl_switch_to_720p");
#endif

    osd_clear();
    osd_show(true);
    lv_timer_handler();
    Display_Osd(g_setting.record.osd);
    LOGI("switch mark: lvgl + osd");

    if (!side_done)
        hdzero_recording_side();
    LOGI("switch mark: to_hdzero done");
}

void hdzero_switch_channel(int channel) {
    channel &= 0x7f;

    LOGI("hdzero_switch_channel to bw:%d, band:%d, ch:%d, CAM_MODE=%d 4:3=%d", g_setting.source.hdzero_bw, g_setting.source.hdzero_band, channel, CAM_MODE, cam_4_3);
    DM6302_SetChannel(g_setting.source.hdzero_band, channel);
    DM5680_clear_vldflg();
    DM5680_req_vldflg();
    channel_osd_mode = CHANNEL_SHOWTIME;

    osd_clear();
    osd_show(true);
    lv_timer_handler();

    system_script(REC_STOP_LIVE);
}