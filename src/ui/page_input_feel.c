#include "page_input_feel.h"

#include <stdio.h>

#include <log/log.h>
#include <minIni.h>

#include "core/app_state.h"
#include "core/common.hh"
#include "core/settings.h"
#include "lang/language.h"
#include "page_speed_common.h"
#include "ui/ui_style.h"

// How the buttons and the dial answer, rather than how fast anything runs.
// These came off the Performance page because none of them buys time: the
// beeps in particular are feedback that happens to be cheap, and their column
// says how long the beep is, not what is saved.

enum {
    ROW_HEAD_LONG_PRESS = 0,
    ROW_TIMED_LONG_PRESS,
    ROW_LONG_PRESS_MS,
    ROW_HEAD_SOUND,
    ROW_BUTTON_BEEP,
    ROW_DIAL_BEEP,
    ROW_BACK,
    ROW_COUNT
};

#define SAVING_LONG_PRESS    "500ms fixed"
#define DURATION_BUTTON_BEEP "beep 50/200ms"
#define DURATION_DIAL_BEEP   "beep 15ms"

_Static_assert(ROW_COUNT <= MAX_PANELS, "Input Feel has more rows than MAX_PANELS");

#define FEEL_ROW_H 60

static lv_coord_t col_dsc[] = SPEED_COL_DSC;
static lv_coord_t row_dsc[] = {FEEL_ROW_H, FEEL_ROW_H, FEEL_ROW_H, FEEL_ROW_H,
                               FEEL_ROW_H, FEEL_ROW_H, FEEL_ROW_H,
                               LV_GRID_TEMPLATE_LAST};

static speed_page_t pg;

static btn_group_t btn_group_long_press;
static btn_group_t btn_group_button_beep;
static btn_group_t btn_group_dial_beep;
static slider_group_t slider_long_press;

// The slider carries an index into long_press_choices, and shows the value.
static void long_press_slider_update(void) {
    char buf[16];
    int idx = long_press_choice_index(g_setting.input.long_press_ms);

    if (idx < 0)
        idx = 0;

    lv_slider_set_value(slider_long_press.slider, idx, LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "%dms", long_press_choices[idx]);
    lv_label_set_text(slider_long_press.label, buf);
}

static bool row_inert(int row) {
    // input_device.c only reads the time on the timed path.
    return row == ROW_LONG_PRESS_MS && !g_setting.speed.timed_long_press;
}

static void rows_refresh(void) {
    speed_row_inert(&pg, ROW_LONG_PRESS_MS, row_inert(ROW_LONG_PRESS_MS));
}

static const char *comment_text(int row) {
    if (row == ROW_LONG_PRESS_MS && row_inert(row))
        return _lang("Only used while Timed Long Press is on.");

    switch (row) {
    case ROW_TIMED_LONG_PRESS:
        return _lang("A long press is timed instead of counting key repeats.");

    case ROW_LONG_PRESS_MS:
        return _lang("How long the button is held before a long press is reported.");

    case ROW_BUTTON_BEEP:
        return _lang("Dial button and right button. 50ms short, 200ms long.");

    case ROW_DIAL_BEEP:
        return _lang("One 15ms beep per detent.");

    default:
        break;
    }

    return _lang("All off is the original behaviour.");
}

static void comment_update(void) {
    speed_comment_set(&pg, comment_text(pp_input_feel.p_arr.cur));
}

static lv_obj_t *page_input_feel_create(lv_obj_t *parent, panel_arr_t *arr) {
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

    snprintf(buf, sizeof(buf), "%s:", _lang("Input Feel"));
    create_text(NULL, section, false, buf, LV_MENU_ITEM_BUILDER_VARIANT_2);

    lv_obj_t *cont = lv_obj_create(section);
    lv_obj_set_size(cont, 960, SPEED_LIST_H(FEEL_ROW_H));
    lv_obj_set_pos(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_GRID);
    lv_obj_add_style(cont, &style_context, LV_PART_MAIN);
    lv_obj_set_style_pad_top(cont, 0, 0);

    lv_obj_set_style_grid_column_dsc_array(cont, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(cont, row_dsc, 0);

    create_select_item(arr, cont, GRID_ROWS(row_dsc));
    speed_page_begin(&pg, cont);

    speed_heading(&pg, arr, "Long Press", ROW_HEAD_LONG_PRESS);
    speed_toggle(&pg, &btn_group_long_press, "Timed Long Press",
                 g_setting.speed.timed_long_press, SAVING_LONG_PRESS, ROW_TIMED_LONG_PRESS);
    create_slider_item(&slider_long_press, cont, _lang("Long Press Time"),
                       LONG_PRESS_CHOICE_NUM - 1, 0, ROW_LONG_PRESS_MS);
    speed_slider(&pg, &slider_long_press, ROW_LONG_PRESS_MS);
    long_press_slider_update();

    speed_heading(&pg, arr, "Sound", ROW_HEAD_SOUND);
    speed_toggle(&pg, &btn_group_button_beep, "Button Beep",
                 g_setting.input.button_beep, DURATION_BUTTON_BEEP, ROW_BUTTON_BEEP);
    speed_toggle(&pg, &btn_group_dial_beep, "Dial Beep",
                 g_setting.input.dial_beep, DURATION_DIAL_BEEP, ROW_DIAL_BEEP);

    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    create_label_item(cont, buf, 1, ROW_BACK, 3);

    pp_input_feel.p_arr.max = ROW_COUNT;

    speed_comment(&pg, section, FEEL_ROW_H); // a row clear of the list
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
    int cur = pp_input_feel.p_arr.cur;

    if (g_app_state == APP_STATE_SUBMENU_ITEM_FOCUSED && cur == ROW_LONG_PRESS_MS) {
        int idx = long_press_choice_index(g_setting.input.long_press_ms);

        if (idx < 0)
            idx = 0;

        if (key == DIAL_KEY_UP && idx < LONG_PRESS_CHOICE_NUM - 1)
            idx++;
        else if (key == DIAL_KEY_DOWN && idx > 0)
            idx--;

        g_setting.input.long_press_ms = long_press_choices[idx];
        long_press_slider_update();
        return;
    }

    speed_scroll_to(&pg, &pp_input_feel.p_arr);
    comment_update();
}

static void on_click(uint8_t key, int sel) {
    (void)key;

    switch (sel) {
    case ROW_TIMED_LONG_PRESS:
        speed_store(&btn_group_long_press, &g_setting.speed.timed_long_press, "speed", "timed_long_press");
        break;

    case ROW_LONG_PRESS_MS:
        // Same two-step edit the fans page uses: click to take the dial, turn
        // to change, click to put it back.
        if (g_app_state == APP_STATE_SUBMENU_ITEM_FOCUSED) {
            ini_putl("input", "long_press_ms", g_setting.input.long_press_ms, SETTING_INI);
            LOGI("input: long_press_ms=%d", g_setting.input.long_press_ms);
            lv_obj_add_style(slider_long_press.slider, &style_silder_main, LV_PART_MAIN);
            app_state_push(APP_STATE_SUBMENU);
        } else {
            app_state_push(APP_STATE_SUBMENU_ITEM_FOCUSED);
            lv_obj_add_style(slider_long_press.slider, &style_silder_select, LV_PART_MAIN);
        }
        break;

    case ROW_BUTTON_BEEP:
        speed_store(&btn_group_button_beep, &g_setting.input.button_beep, "input", "button_beep");
        break;

    case ROW_DIAL_BEEP:
        speed_store(&btn_group_dial_beep, &g_setting.input.dial_beep, "input", "dial_beep");
        break;

    default:
        return;
    }

    rows_refresh();
    comment_update();
}

page_pack_t pp_input_feel = {
    .p_arr = {
        .cur = 0,
        .max = ROW_COUNT,
    },
    .name = "Input Feel",
    .create = page_input_feel_create,
    .enter = on_enter,
    .exit = NULL,
    .on_created = NULL,
    .on_update = NULL,
    .on_roller = on_roller,
    .on_click = on_click,
    .on_right_button = NULL,
};
