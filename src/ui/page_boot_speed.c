#include "page_boot_speed.h"

#include <stdio.h>

#include "core/settings.h"
#include "lang/language.h"
#include "page_speed_common.h"
#include "ui/ui_style.h"

// Everything here applies at the next start-up and nothing here changes what
// the goggles do once they are running, which is why it is a page of its own:
// it is tested by writing the card, booting, and reading the boot marks in the
// log, where the switch page is tested by pressing the button.

enum {
    ROW_BOOT_DISPLAY = 0,
    ROW_BOOT_FONTS,
    ROW_SKIP_BOOT_MENU,
    ROW_DEFER_MENU,
    ROW_ASYNC_IMU,
    ROW_ASYNC_DISPLAY,
    ROW_EARLY_TIMING,
    ROW_ASYNC_TUNER,
    ROW_SKIP_WIFI_STOP,
    ROW_BACK,
    ROW_COUNT
};

// Measured on the goggles with the boot instrumented, so the cost of leaving
// one off is visible rather than implied. Each figure is what that switch is
// worth ON ITS OWN; where two of them hide work in the same window they cannot
// both claim it, and the comment under the list says so.
#define SAVING_BOOT_DISPLAY   "-1140ms"
#define SAVING_BOOT_FONTS     "-550..-1000ms"
#define SAVING_SKIP_BOOT_MENU "no menu flash"
#define SAVING_DEFER_MENU     "est. -70..-240"
#define SAVING_ASYNC_IMU      "-686ms"
#define SAVING_ASYNC_DISPLAY  "-1092ms alone"
#define SAVING_EARLY_TIMING   "est. -364ms"
#define SAVING_ASYNC_TUNER    "-844ms alone"
#define SAVING_SKIP_WIFI_STOP "-1080ms after"

_Static_assert(ROW_COUNT <= MAX_PANELS, "Boot Speed has more rows than MAX_PANELS");

#define BOOT_ROW_H 60

static lv_coord_t col_dsc[] = SPEED_COL_DSC;
static lv_coord_t row_dsc[] = {BOOT_ROW_H, BOOT_ROW_H, BOOT_ROW_H, BOOT_ROW_H,
                               BOOT_ROW_H, BOOT_ROW_H, BOOT_ROW_H, BOOT_ROW_H,
                               BOOT_ROW_H, BOOT_ROW_H, LV_GRID_TEMPLATE_LAST};

static speed_page_t pg;

static btn_group_t btn_group_boot_display;
static btn_group_t btn_group_boot_fonts;
static btn_group_t btn_group_skip_boot_menu;
static btn_group_t btn_group_defer_menu;
static btn_group_t btn_group_async_imu;
static btn_group_t btn_group_async_display;
static btn_group_t btn_group_early_timing;
static btn_group_t btn_group_async_tuner;
static btn_group_t btn_group_skip_wifi_stop;

// A switch can be on and still have nothing to do, because the code behind it
// refuses to run without its neighbours. Leaving a measured figure next to one
// of those reads as a saving the user is not getting.
static bool row_inert(int row) {
    if (row != ROW_EARLY_TIMING)
        return false;

    // boot_workers_start() checks all three and logs a warning instead of
    // starting the timing, so on its own this row does nothing at all. All
    // three are on this page, above it, so the reason is on screen with it.
    return !(g_setting.speed.async_display && g_setting.speed.boot_display &&
             g_setting.speed.skip_boot_menu);
}

static void rows_refresh(void) {
    speed_row_inert(&pg, ROW_EARLY_TIMING, row_inert(ROW_EARLY_TIMING));
}

static const char *comment_text(int row) {
    if (row == ROW_EARLY_TIMING && row_inert(row))
        return _lang("Does nothing until Async Display Setup, Skip Display Setup and Skip Boot Menu are all on.");

    switch (row) {
    case ROW_BOOT_DISPLAY:
        return _lang("Applies at the next start-up. The boot screen keeps the kernel's mode.");

    case ROW_BOOT_FONTS:
        return _lang("Applies at the next start-up. Worth more when the fonts are on the card.");

    case ROW_SKIP_BOOT_MENU:
        return _lang("The menu is on screen during start-up until the video covers it.");

    case ROW_DEFER_MENU:
        return _lang("Moves 976ms off the path, but exposes the OSD font wait and the tuner under it.");

    case ROW_ASYNC_IMU:
        return _lang("Brought up alongside the rest of start-up, waited for before the threads run.");

    case ROW_ASYNC_DISPLAY:
        return _lang("Runs with the tuner init. The panel blanks a little earlier.");

    case ROW_EARLY_TIMING:
        return _lang("Next start-up: the video timing starts before the rest of the boot path.");

    case ROW_ASYNC_TUNER:
        return _lang("Next start-up, straight to HDZero video only. Near zero unless Early Video Timing is on.");

    case ROW_SKIP_WIFI_STOP:
        return _lang("Frees the main loop for a second after the video, not before it.");

    default:
        break;
    }

    return _lang("All off is the original behaviour.");
}

static void comment_update(void) {
    speed_comment_set(&pg, comment_text(pp_boot_speed.p_arr.cur));
}

static lv_obj_t *page_boot_speed_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[64];

    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, 1053, 900);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);
    lv_obj_set_style_pad_top(page, 24, 0);

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, 1053, 894);
    lv_obj_set_style_pad_top(section, 36, 0);

    snprintf(buf, sizeof(buf), "%s:", _lang("Boot Speed"));
    create_text(NULL, section, false, buf, LV_MENU_ITEM_BUILDER_VARIANT_2);

    lv_obj_t *cont = lv_obj_create(section);
    lv_obj_set_size(cont, 960, SPEED_LIST_H(BOOT_ROW_H));
    lv_obj_set_pos(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_GRID);
    lv_obj_add_style(cont, &style_context, LV_PART_MAIN);
    lv_obj_set_style_pad_top(cont, 0, 0);

    lv_obj_set_style_grid_column_dsc_array(cont, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(cont, row_dsc, 0);

    create_select_item(arr, cont, GRID_ROWS(row_dsc));
    speed_page_begin(&pg, cont);

    speed_toggle(&pg, &btn_group_boot_display, "Skip Display Setup",
                 g_setting.speed.boot_display, SAVING_BOOT_DISPLAY, ROW_BOOT_DISPLAY);
    speed_toggle(&pg, &btn_group_boot_fonts, "Preload OSD Fonts",
                 g_setting.speed.boot_fonts, SAVING_BOOT_FONTS, ROW_BOOT_FONTS);
    speed_toggle(&pg, &btn_group_skip_boot_menu, "Skip Boot Menu",
                 g_setting.speed.skip_boot_menu, SAVING_SKIP_BOOT_MENU, ROW_SKIP_BOOT_MENU);
    speed_toggle(&pg, &btn_group_defer_menu, "Defer Menu Build",
                 g_setting.speed.defer_menu, SAVING_DEFER_MENU, ROW_DEFER_MENU);
    speed_toggle(&pg, &btn_group_async_imu, "Async Motion Sensor",
                 g_setting.speed.async_imu, SAVING_ASYNC_IMU, ROW_ASYNC_IMU);
    speed_toggle(&pg, &btn_group_async_display, "Async Display Setup",
                 g_setting.speed.async_display, SAVING_ASYNC_DISPLAY, ROW_ASYNC_DISPLAY);
    // Directly under the last of the three it needs.
    speed_toggle(&pg, &btn_group_early_timing, "Early Video Timing",
                 g_setting.speed.boot_display_early, SAVING_EARLY_TIMING, ROW_EARLY_TIMING);
    speed_toggle(&pg, &btn_group_async_tuner, "Async Tuner Init",
                 g_setting.speed.async_tuner, SAVING_ASYNC_TUNER, ROW_ASYNC_TUNER);
    speed_toggle(&pg, &btn_group_skip_wifi_stop, "Skip WiFi Stop",
                 g_setting.speed.skip_wifi_stop, SAVING_SKIP_WIFI_STOP, ROW_SKIP_WIFI_STOP);

    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    create_label_item(cont, buf, 1, ROW_BACK, 3);

    pp_boot_speed.p_arr.max = ROW_COUNT;

    speed_comment(&pg, section, BOOT_ROW_H); // a row clear of the list
    rows_refresh();
    comment_update();

    return page;
}

static void on_enter(void) {
    if (pg.cont)
        lv_obj_scroll_to(pg.cont, 0, 0, LV_ANIM_OFF);

    rows_refresh();
    comment_update();
}

static void on_roller(uint8_t key) {
    (void)key;
    speed_scroll_to(&pg, &pp_boot_speed.p_arr);
    comment_update();
}

static void on_click(uint8_t key, int sel) {
    (void)key;

    switch (sel) {
    case ROW_BOOT_DISPLAY:
        speed_store(&btn_group_boot_display, &g_setting.speed.boot_display, "speed", "boot_display");
        break;

    case ROW_BOOT_FONTS:
        speed_store(&btn_group_boot_fonts, &g_setting.speed.boot_fonts, "speed", "boot_fonts");
        break;

    case ROW_SKIP_BOOT_MENU:
        speed_store(&btn_group_skip_boot_menu, &g_setting.speed.skip_boot_menu, "speed", "skip_boot_menu");
        break;

    case ROW_DEFER_MENU:
        speed_store(&btn_group_defer_menu, &g_setting.speed.defer_menu, "speed", "defer_menu");
        break;

    case ROW_ASYNC_IMU:
        speed_store(&btn_group_async_imu, &g_setting.speed.async_imu, "speed", "async_imu");
        break;

    case ROW_ASYNC_DISPLAY:
        speed_store(&btn_group_async_display, &g_setting.speed.async_display, "speed", "async_display");
        break;

    case ROW_EARLY_TIMING:
        speed_store(&btn_group_early_timing, &g_setting.speed.boot_display_early, "speed", "boot_display_early");
        break;

    case ROW_ASYNC_TUNER:
        speed_store(&btn_group_async_tuner, &g_setting.speed.async_tuner, "speed", "async_tuner");
        break;

    case ROW_SKIP_WIFI_STOP:
        speed_store(&btn_group_skip_wifi_stop, &g_setting.speed.skip_wifi_stop, "speed", "skip_wifi_stop");
        break;

    default:
        return;
    }

    // Three of these decide whether Early Video Timing does anything, and the
    // comment changes with them, so both are redone after any click rather
    // than listing which rows affect which.
    rows_refresh();
    comment_update();
}

page_pack_t pp_boot_speed = {
    .p_arr = {
        .cur = 0,
        .max = ROW_COUNT,
    },
    .name = "Boot Speed",
    .create = page_boot_speed_create,
    .enter = on_enter,
    .exit = NULL,
    .on_created = NULL,
    .on_update = NULL,
    .on_roller = on_roller,
    .on_click = on_click,
    .on_right_button = NULL,
};
