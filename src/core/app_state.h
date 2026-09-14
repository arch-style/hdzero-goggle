#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "ui/page_common.h"

typedef enum {
    APP_STATE_MAINMENU = 0,
    APP_STATE_SUBMENU = 1,
    APP_STATE_PLAYBACK = 2,

    // in this state, the menu pages' on_roller is called,
    // but the selected submenu item selection (e.g. pp_osd.p_arr.cur)
    // is not automatically changed.
    APP_STATE_SUBMENU_ITEM_FOCUSED = 3,

    APP_STATE_VIDEO = 10,

    // the preview for image settings
    APP_STATE_IMS = 11,
    // the preview for osd element positioning settings
    APP_STATE_OSD_ELEMENT_PREV = 12,

    // WiFi configuration page
    APP_STATE_WIFI = 13,

    APP_STATE_USER_INPUT_DISABLED = 20,

    APP_STATE_SLEEP = 30,
} app_state_t;

extern app_state_t g_app_state;

void app_state_push(app_state_t state);

void app_switch_to_menu();
void app_exit_menu();
void app_switch_to_analog(bool is_av_in);
void app_switch_to_hdmi_in();
void app_switch_to_hdzero(bool is_default);
void hdzero_switch_channel(int channel);
// Close and re-initialise the HDZero receivers where they stand: same band,
// same channel, same bandwidth, display untouched. A button action for
// looking at DM6302_init() on its own, every press, with the picture there
// to judge it by. Does nothing on any other source.
void app_tuner_reinit(void);
// Begin the display timing change the next HDZero switch will want, in the
// background. Called from the switch itself, and at start-up by Early Video
// Timing so dispw runs during the UI build instead of after it.
void start_display_timing_early(void);
// Take the display for the UI when Menu Over Video left it on the live source.
// For pages that play video of their own; does nothing if the menu is not
// overlaid.
void app_menu_end_overlay(void);

#ifdef __cplusplus
}
#endif
