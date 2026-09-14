#include "page_bugfix.h"

#include <stdio.h>

#include <log/log.h>

#include "core/settings.h"
#include "lang/language.h"
#include "page_common.h"
#include "ui/ui_style.h"

// Fixes for faults in the stock firmware, each one off by default so that all
// off is the behaviour the goggles shipped with. Separate from the two speed
// pages: nothing here trades one thing for another, it is only about being
// correct, so it does not belong in a column of measured savings.

enum {
    ROW_RETRY_TUNER = 0,
    ROW_WAIT_RECORDING,
    ROW_I2C_TIMEOUT,
    ROW_QUEUED_PRESSES,
    ROW_GIVE_UP_DVR,
    ROW_BACK,
    ROW_COUNT
};

static lv_coord_t col_dsc[] = {90, 340, 175, 175, 180, 0, LV_GRID_TEMPLATE_LAST};
static lv_coord_t row_dsc[] = {60, 60, 60, 60, 60, 60, 60, 60, 60, 60, LV_GRID_TEMPLATE_LAST};

static btn_group_t btn_group_retry_tuner;
static btn_group_t btn_group_wait_recording;
static btn_group_t btn_group_i2c_timeout;
static btn_group_t btn_group_queued_presses;
static btn_group_t btn_group_give_up_dvr;

static lv_obj_t *page_bugfix_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[1024];

    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, 1053, 900);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);
    lv_obj_set_style_pad_top(page, 94, 0);

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, 1053, 894);

    snprintf(buf, sizeof(buf), "%s:", _lang("Fixes"));
    create_text(NULL, section, false, buf, LV_MENU_ITEM_BUILDER_VARIANT_2);

    lv_obj_t *cont = lv_obj_create(section);
    lv_obj_set_size(cont, 960, 894);
    lv_obj_set_pos(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_GRID);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(cont, &style_context, LV_PART_MAIN);

    lv_obj_set_style_grid_column_dsc_array(cont, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(cont, row_dsc, 0);

    create_select_item(arr, cont, GRID_ROWS(row_dsc));

    create_btn_group_item(&btn_group_retry_tuner, cont, 2, _lang("Retry Tuner Init"),
                          _lang("Off"), _lang("On"), "", "", ROW_RETRY_TUNER);
    btn_group_set_sel(&btn_group_retry_tuner, g_setting.bugfix.retry_tuner_init ? 1 : 0);

    create_btn_group_item(&btn_group_wait_recording, cont, 2, _lang("Wait For Recording"),
                          _lang("Off"), _lang("On"), "", "", ROW_WAIT_RECORDING);
    btn_group_set_sel(&btn_group_wait_recording, g_setting.bugfix.wait_for_recording ? 1 : 0);

    create_btn_group_item(&btn_group_i2c_timeout, cont, 2, _lang("Short I2C Timeout"),
                          _lang("Off"), _lang("On"), "", "", ROW_I2C_TIMEOUT);
    btn_group_set_sel(&btn_group_i2c_timeout, g_setting.bugfix.short_i2c_timeout ? 1 : 0);

    create_btn_group_item(&btn_group_queued_presses, cont, 2, _lang("Ignore Queued Presses"),
                          _lang("Off"), _lang("On"), "", "", ROW_QUEUED_PRESSES);
    btn_group_set_sel(&btn_group_queued_presses, g_setting.bugfix.drop_queued_presses ? 1 : 0);

    create_btn_group_item(&btn_group_give_up_dvr, cont, 2, _lang("Give Up Auto DVR"),
                          _lang("Off"), _lang("On"), "", "", ROW_GIVE_UP_DVR);
    btn_group_set_sel(&btn_group_give_up_dvr, g_setting.bugfix.dvr_give_up ? 1 : 0);

    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    create_label_item(cont, buf, 1, ROW_BACK, 3);

    pp_bugfix.p_arr.max = ROW_COUNT;

    lv_obj_t *note = lv_label_create(cont);
    snprintf(buf, sizeof(buf), "%s:\n    - %s\n    - %s\n    - %s\n%s:\n    - %s\n    - %s\n%s:\n    - %s\n    - %s\n    - %s\n%s:\n    - %s\n    - %s\n    - %s\n%s:\n    - %s\n    - %s\n    - %s",
             _lang("Retry Tuner Init"),
             _lang("The receivers sometimes fail to start: no picture, noise, or two dead antennas"),
             _lang("Stock calls the tuner open even when its init failed, so nothing ever retried"),
             _lang("On retries immediately, then again on the next switch to video"),
             _lang("Wait For Recording"),
             _lang("A source change always stops the recorder now; the file is finalised a moment later"),
             _lang("On waits for that, up to 2s, so a clip under 3s long cannot end in the old picture"),
             _lang("Short I2C Timeout"),
             _lang("One transfer on the main bus has been seen taking 5006ms and then succeeding"),
             _lang("That is the driver's own timeout and reset; the bus, and the goggles, wait it out"),
             _lang("On gives the adapter a 500ms timeout instead. Needs a restart"),
             _lang("Ignore Queued Presses"),
             _lang("A right button press made during a source switch is queued behind it, not lost"),
             _lang("Stock then acts on it: two such presses make a double click, Go Sleep by default"),
             _lang("On drops presses that arrived during an action that took over 250ms"),
             _lang("Give Up Auto DVR"),
             _lang("A card the kernel has turned read-only refuses every recording; stock retries every 3s all flight"),
             _lang("Each retry holds the DVR lock for 2s, which is what makes the right button wait"),
             _lang("On stops after 3 failed starts and shows the reason by the SD icon until the card is re-inserted"));
    lv_label_set_text(note, buf);

    lv_obj_set_style_text_font(note, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(note, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_pad_top(note, 12, 0);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_grid_cell(note, LV_GRID_ALIGN_START, 1, 4, LV_GRID_ALIGN_START, ROW_COUNT + 1, 4);

    return page;
}

static void on_click(uint8_t key, int sel) {
    switch (sel) {
    case ROW_RETRY_TUNER:
        btn_group_toggle_sel(&btn_group_retry_tuner);
        g_setting.bugfix.retry_tuner_init = btn_group_get_sel(&btn_group_retry_tuner) == 1;
        settings_put_bool("bugfix", "retry_tuner_init", g_setting.bugfix.retry_tuner_init);
        LOGI("bugfix: retry_tuner_init=%s", g_setting.bugfix.retry_tuner_init ? "on" : "off");
        break;

    case ROW_WAIT_RECORDING:
        btn_group_toggle_sel(&btn_group_wait_recording);
        g_setting.bugfix.wait_for_recording = btn_group_get_sel(&btn_group_wait_recording) == 1;
        settings_put_bool("bugfix", "wait_for_recording", g_setting.bugfix.wait_for_recording);
        LOGI("bugfix: wait_for_recording=%s", g_setting.bugfix.wait_for_recording ? "on" : "off");
        break;

    case ROW_QUEUED_PRESSES:
        btn_group_toggle_sel(&btn_group_queued_presses);
        g_setting.bugfix.drop_queued_presses = btn_group_get_sel(&btn_group_queued_presses) == 1;
        settings_put_bool("bugfix", "drop_queued_presses", g_setting.bugfix.drop_queued_presses);
        LOGI("bugfix: drop_queued_presses=%s", g_setting.bugfix.drop_queued_presses ? "on" : "off");
        break;

    case ROW_GIVE_UP_DVR:
        btn_group_toggle_sel(&btn_group_give_up_dvr);
        g_setting.bugfix.dvr_give_up = btn_group_get_sel(&btn_group_give_up_dvr) == 1;
        settings_put_bool("bugfix", "dvr_give_up", g_setting.bugfix.dvr_give_up);
        LOGI("bugfix: dvr_give_up=%s", g_setting.bugfix.dvr_give_up ? "on" : "off");
        break;

    case ROW_I2C_TIMEOUT:
        // Applied when the ports are opened, which has already happened.
        btn_group_toggle_sel(&btn_group_i2c_timeout);
        g_setting.bugfix.short_i2c_timeout = btn_group_get_sel(&btn_group_i2c_timeout) == 1;
        settings_put_bool("bugfix", "short_i2c_timeout", g_setting.bugfix.short_i2c_timeout);
        LOGI("bugfix: short_i2c_timeout=%s (from the next start)", g_setting.bugfix.short_i2c_timeout ? "on" : "off");
        break;

    default:
        break;
    }
}

page_pack_t pp_bugfix = {
    .p_arr = {
        .cur = 0,
        .max = ROW_COUNT,
    },
    .name = "Fixes",
    .create = page_bugfix_create,
    .enter = NULL,
    .exit = NULL,
    .on_created = NULL,
    .on_update = NULL,
    .on_roller = NULL,
    .on_click = on_click,
    .on_right_button = NULL,
};
