#include "page_speed_common.h"

#include <string.h>

#include <log/log.h>

#include "core/settings.h"
#include "lang/language.h"
#include "ui/ui_style.h"

void speed_page_begin(speed_page_t *pg, lv_obj_t *cont) {
    memset(pg, 0, sizeof(*pg));
    pg->cont = cont;

    // Taller than the container whenever the rows ask for it; speed_scroll_to()
    // keeps the selection in view. A page whose rows fit never scrolls, and one
    // whose rows do not keeps its last row instead of clipping it.
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_scroll_to(cont, 0, 0, LV_ANIM_OFF);
}

void speed_heading(speed_page_t *pg, panel_arr_t *arr, const char *name, int row) {
    lv_obj_t *label = create_label_item(pg->cont, _lang(name), 1, row, 3);

    lv_obj_set_style_text_color(label, lv_color_make(0xC0, 0xC0, 0x40), 0);
    lv_obj_clear_flag(arr->panel[row], FLAG_SELECTABLE);
}

// The saving goes in the columns to the right of the Off/On buttons, which
// create_btn_group_item() leaves free; in the row's own label the text would
// run under the buttons. create_label_item_compact() has already given it a
// disabled colour, so speed_row_inert() only has to set the state.
static void speed_saving(speed_page_t *pg, const char *saving, int row) {
    pg->saving[row] = create_label_item_compact(pg->cont, saving, 4, row, 2, 60,
                                                LV_TEXT_ALIGN_LEFT, LV_GRID_ALIGN_START,
                                                &lv_font_montserrat_20);
    pg->saving_text[row] = saving;
}

void speed_toggle(speed_page_t *pg, btn_group_t *group, const char *name,
                  bool value, const char *saving, int row) {
    create_btn_group_item(group, pg->cont, 2, _lang(name), _lang("Off"), _lang("On"), "", "", row);
    btn_group_set_sel(group, value ? 1 : 0);
    speed_saving(pg, saving, row);
    pg->name[row] = group->label;
}

void speed_choice(speed_page_t *pg, btn_group_t *group, const char *name,
                  const char *c0, const char *c1, const char *c2, int value, int row) {
    create_btn_group_item(group, pg->cont, 3, _lang(name), c0, c1, c2, "", row);
    btn_group_set_sel(group, value);
    // An empty figure keeps the row's slot in the arrays without drawing over
    // the third button.
    speed_saving(pg, "", row);
    pg->name[row] = group->label;
}

void speed_slider(speed_page_t *pg, slider_group_t *slider, int row) {
    lv_obj_set_grid_cell(slider->slider, LV_GRID_ALIGN_STRETCH, 3, 1,
                         LV_GRID_ALIGN_CENTER, row, 1);
    lv_obj_set_grid_cell(slider->label, LV_GRID_ALIGN_START, 4, 2,
                         LV_GRID_ALIGN_CENTER, row, 1);

    // The value stays in its column rather than being swapped for a marker: it
    // still has to say what it is set to. saving_text stays NULL to say so.
    pg->name[row] = slider->name;
    pg->saving[row] = slider->label;
}

void speed_comment(speed_page_t *pg, lv_obj_t *section, lv_coord_t pad_top) {
    // Outside the list, so it stays put while a scrolling list moves.
    pg->comment = lv_label_create(section);

    lv_obj_set_width(pg->comment, 960);
    lv_obj_set_style_text_font(pg->comment, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(pg->comment, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(pg->comment, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_pad_left(pg->comment, 90, 0);
    lv_obj_set_style_pad_top(pg->comment, pad_top, 0);
    lv_label_set_long_mode(pg->comment, LV_LABEL_LONG_WRAP);
}

void speed_comment_set(speed_page_t *pg, const char *text) {
    if (pg->comment)
        lv_label_set_text(pg->comment, text);
}

static void set_state(lv_obj_t *obj, bool inert) {
    if (!obj)
        return;

    if (inert)
        lv_obj_add_state(obj, STATE_DISABLED);
    else
        lv_obj_clear_state(obj, STATE_DISABLED);
}

void speed_row_inert(speed_page_t *pg, int row, bool inert) {
    set_state(pg->name[row], inert);
    set_state(pg->saving[row], inert);

    if (pg->saving[row] && pg->saving_text[row])
        lv_label_set_text(pg->saving[row], inert ? _lang(SPEED_SAVING_INERT) : pg->saving_text[row]);
}

void speed_store(btn_group_t *group, bool *value, const char *section, const char *key) {
    btn_group_toggle_sel(group);
    *value = btn_group_get_sel(group) == 1;
    settings_put_bool((char *)section, (char *)key, *value);
    LOGI("%s: %s=%s", section, key, *value ? "on" : "off");
}

void speed_scroll_to(speed_page_t *pg, panel_arr_t *arr) {
    int cur = arr->cur;

    if (!pg->cont)
        return;

    if (cur > 0 && arr->panel[cur - 1])
        lv_obj_scroll_to_view(arr->panel[cur - 1], LV_ANIM_OFF);

    if (arr->panel[cur])
        lv_obj_scroll_to_view(arr->panel[cur], LV_ANIM_OFF);
}
