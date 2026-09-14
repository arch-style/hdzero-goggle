#include "page_favorites.h"

#include <stdio.h>

#include <log/log.h>
#include <minIni.h>

#include "core/app_state.h"
#include "core/common.hh"
#include "core/favorites.h"
#include "core/osd.h"
#include "core/settings.h"
#include "lang/language.h"
#include "page_common.h"
#include "page_scannow.h"
#include "ui/ui_style.h"

// "CH" rather than "Ch"/"ch": the OSD's own channel readout spells it that
// way, and it is the only channel abbreviation shown to the user.
#define FAVORITES_PAGE_NAME "Favorites CH"

enum {
    ROW_SOURCE = 0,
    ROW_ENABLE,
    ROW_COUNT_SEL,
    ROW_SLOT_FIRST,
    ROW_SLOT_LAST = ROW_SLOT_FIRST + FAVORITES_MAX - 1,
    ROW_BACK,
    ROW_HINT, // not selectable, just the last grid row we occupy
    ROW_COUNT = ROW_BACK + 1
};

static lv_coord_t col_dsc[] = {160, 200, 200, 160, 160, 160, LV_GRID_TEMPLATE_LAST};
// 51 rather than the usual 60: this page needs 11 selectable rows plus a hint
// line, more than any other page, and all of it has to stay on screen. Not
// shrunk further because create_btn_group_item() builds 60px widgets that only
// tolerate so much squeezing. The hint is small text, so its row is shorter.
static lv_coord_t row_dsc[] = {51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 30, LV_GRID_TEMPLATE_LAST};

static btn_group_t btn_group_source;
static btn_group_t btn_group_fav;

// Which list is on screen. It starts on the one being watched and the Source
// row moves it, because the other list is only reachable by changing what the
// goggles are receiving otherwise -- and the analog list in particular is
// worth setting up before the analog module is plugged in.
//
// This is the page's view, never what tunes: favorites_source() still decides
// that, so editing the list you are not watching changes nothing under you.
static favorites_source_t viewed_source = FAVORITES_SOURCE_HDZERO;

static setting_favorites_list_t *viewed_list(void) {
    return favorites_list_of(viewed_source);
}
static lv_obj_t *title_label;
static lv_obj_t *count_label;
static lv_obj_t *slot_label[FAVORITES_MAX];
static lv_obj_t *hint_label;

// Row currently being edited with the dial, or -1 when nothing is focused.
static int editing_row = -1;

static void count_label_update(void) {
    char buf[64];
    uint8_t count = viewed_list()->count;

    if (editing_row == ROW_COUNT_SEL)
        snprintf(buf, sizeof(buf), "%s: #FFFF00 %d#", _lang("Number of channels"), count);
    else
        snprintf(buf, sizeof(buf), "%s: %d", _lang("Number of channels"), count);

    lv_label_set_text(count_label, buf);
}

// The heading names the list on screen, which is not always the one in use.
static void title_label_update(void) {
    char buf[64];

    snprintf(buf, sizeof(buf), "%s (%s):", _lang(FAVORITES_PAGE_NAME),
             _lang(favorites_source_name_of(viewed_source)));
    lv_label_set_text(title_label, buf);
    btn_group_set_sel(&btn_group_source, (viewed_source == FAVORITES_SOURCE_ANALOG) ? 1 : 0);
    btn_group_set_sel(&btn_group_fav, viewed_list()->enable ? 1 : 0);
}

static void slot_label_update(int slot) {
    char buf[64];
    char off_band[32];
    setting_favorites_list_t *list = viewed_list();
    bool is_hdzero = (viewed_source == FAVORITES_SOURCE_HDZERO);
    uint8_t ch = list->channel[slot];
    const char *value;

    if (ch == 0) {
        value = _lang("Empty");
    } else if (ch > favorites_channel_max_of(viewed_source)) {
        // Registered on the other band: the low band has eight channels and
        // the race band twelve, and a slot is kept rather than clamped when
        // the band changes. channel2str() clamps instead, so F2 seen from the
        // low band came out as "L1" -- a channel that exists, that the dial
        // then skipped over. Name it from the band it belongs to and say so.
        // Plain text, no recolour markup: the row being edited wraps the
        // whole value in a colour of its own and the two would nest.
        snprintf(off_band, sizeof(off_band), "%s (%s)",
                 channel2str(is_hdzero, SETTING_SOURCES_HDZERO_BAND_RACEBAND, ch),
                 _lang("other band"));
        value = off_band;
    } else {
        value = channel2str(is_hdzero, g_setting.source.hdzero_band, ch);
    }

    if (editing_row == ROW_SLOT_FIRST + slot)
        snprintf(buf, sizeof(buf), "%s %d: #FFFF00 %s#", _lang("Slot"), slot + 1, value);
    else if (favorites_slot_duplicate_of(viewed_source, slot))
        snprintf(buf, sizeof(buf), "%s %d: %s #FF8000 (%s)#", _lang("Slot"), slot + 1, value, _lang("duplicate"));
    else
        snprintf(buf, sizeof(buf), "%s %d: %s", _lang("Slot"), slot + 1, value);

    lv_label_set_text(slot_label[slot], buf);

    // Slots past the configured count stay on screen but greyed out, and the
    // dial skips over them.
    bool in_use = (slot < list->count);
    lv_obj_t *panel = pp_favorites.p_arr.panel[ROW_SLOT_FIRST + slot];

    if (in_use) {
        lv_obj_add_flag(panel, FLAG_SELECTABLE);
        lv_obj_clear_state(slot_label[slot], STATE_DISABLED);
    } else {
        lv_obj_clear_flag(panel, FLAG_SELECTABLE);
        lv_obj_add_state(slot_label[slot], STATE_DISABLED);
    }
}

static void hint_label_update(void) {
    char buf[128];
    int valid = favorites_valid_count_of(viewed_source);

    bool enabled = viewed_list()->enable;

    if (editing_row >= 0)
        snprintf(buf, sizeof(buf), "%s", _lang("Turn the dial to change, click to confirm"));
    else if (viewed_source != favorites_source())
        // Editing the list for the source that is not being watched, which is
        // the point of the Source row but worth saying out loud.
        snprintf(buf, sizeof(buf), "%s", _lang("This list applies when that source is being watched"));
    else if (enabled && (valid == 0))
        snprintf(buf, sizeof(buf), "#FF8000 %s#", _lang("Register at least 1 channel to use favorites"));
    else if (enabled && (valid == 1))
        snprintf(buf, sizeof(buf), "%s", _lang("The dial stays locked to this channel"));
    else
        snprintf(buf, sizeof(buf), " ");

    lv_label_set_text(hint_label, buf);
}

static void page_favorites_update(void) {
    title_label_update();
    count_label_update();
    for (int i = 0; i < FAVORITES_MAX; i++)
        slot_label_update(i);
    hint_label_update();
}

static lv_obj_t *page_favorites_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[128];
    int row = 0;

    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, 1053, 900);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);
    lv_obj_set_style_pad_top(page, 24, 0); // other pages use 94; we need the height

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, 1053, 894);
    // style_submenu leaves 96px above the title. Reclaim most of it: this is
    // the page that needs the height, and it lifts the title and the grid
    // together, so nothing below has to move.
    lv_obj_set_style_pad_top(section, 36, 0);

    // The heading names the source as well as the page, so it has to change
    // when the source does. Built here rather than with create_text(), which
    // keeps no handle on its label; the styling matches what it would apply.
    lv_obj_t *heading = lv_menu_cont_create(section);
    title_label = lv_label_create(heading);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_26, 0);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);

    lv_obj_t *cont = lv_obj_create(section);
    lv_obj_set_size(cont, 960, 894);
    lv_obj_set_pos(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_GRID);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(cont, &style_context, LV_PART_MAIN);

    lv_obj_set_style_grid_column_dsc_array(cont, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(cont, row_dsc, 0);

    create_select_item(arr, cont, GRID_ROWS(row_dsc));

    create_btn_group_item(&btn_group_source, cont, 2, _lang("Source"),
                          _lang("HDZero"), _lang("Analog"), "", "", row++);
    create_btn_group_item(&btn_group_fav, cont, 2, _lang("Enable"), _lang("Off"), _lang("On"), "", "", row++);

    count_label = create_label_item(cont, "", 1, row++, 3);

    for (int i = 0; i < FAVORITES_MAX; i++)
        slot_label[i] = create_label_item(cont, "", 1, row++, 3);

    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    create_label_item(cont, buf, 1, row++, 3);

    hint_label = lv_label_create(cont);
    lv_label_set_recolor(hint_label, true);
    lv_label_set_text(hint_label, " ");
    lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(hint_label, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_pad_top(hint_label, 12, 0);
    lv_label_set_long_mode(hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_grid_cell(hint_label, LV_GRID_ALIGN_START, 1, 4, LV_GRID_ALIGN_START, ROW_HINT, 1);

    pp_favorites.p_arr.max = row;

    page_favorites_update();
    return page;
}

static void on_enter(void) {
    // The band can have changed on the Source page since the last visit, which
    // changes both the channel names and which slots are usable.
    editing_row = -1;
    // Start on the list that is in use; the Source row moves off it.
    viewed_source = favorites_source();
    page_favorites_update();
}

static void on_exit(void) {
    editing_row = -1;
}

static void on_roller(uint8_t key) {
    if (g_app_state != APP_STATE_SUBMENU_ITEM_FOCUSED)
        return;

    setting_favorites_list_t *list = viewed_list();

    if (editing_row == ROW_COUNT_SEL) {
        uint8_t count = list->count;

        if (key == DIAL_KEY_UP)
            count = (count >= FAVORITES_MAX) ? 1 : count + 1;
        else if (key == DIAL_KEY_DOWN)
            count = (count <= 1) ? FAVORITES_MAX : count - 1;

        list->count = count;
        page_favorites_update();
        return;
    }

    if ((editing_row < ROW_SLOT_FIRST) || (editing_row > ROW_SLOT_LAST))
        return;

    int slot = editing_row - ROW_SLOT_FIRST;

    // 0 is the "empty" entry, so the value wraps over [0, channel count].
    uint8_t ch = list->channel[slot];
    uint8_t max = favorites_channel_max_of(viewed_source);

    if (key == DIAL_KEY_UP)
        ch = (ch >= max) ? 0 : ch + 1;
    else if (key == DIAL_KEY_DOWN)
        ch = (ch == 0) ? max : ch - 1;

    list->channel[slot] = ch;
    slot_label_update(slot);
}

// Which ini section the list on screen belongs to.
static const char *favorites_ini_section(void) {
    return (viewed_source == FAVORITES_SOURCE_ANALOG) ? FAVORITES_INI_ANALOG : FAVORITES_INI_HDZERO;
}

static void favorites_save_row(int row) {
    const setting_favorites_list_t *list = viewed_list();
    const char *section = favorites_ini_section();
    char key[8];

    if (row == ROW_COUNT_SEL) {
        ini_putl(section, "count", list->count, SETTING_INI);
        LOGI("favorites(%s): count = %d", section, list->count);
        return;
    }

    int slot = row - ROW_SLOT_FIRST;
    snprintf(key, sizeof(key), "ch%d", slot + 1);
    ini_putl(section, key, list->channel[slot], SETTING_INI);
    LOGI("favorites(%s): slot %d = channel %d", section, slot + 1, list->channel[slot]);
}

static void on_click(uint8_t key, int sel) {
    if (sel == ROW_SOURCE) {
        // Nothing is saved by this: it changes which list the rest of the page
        // is showing, and each row still writes its own ini section on confirm.
        btn_group_toggle_sel(&btn_group_source);
        viewed_source = (btn_group_get_sel(&btn_group_source) == 1) ? FAVORITES_SOURCE_ANALOG
                                                                   : FAVORITES_SOURCE_HDZERO;
        editing_row = -1;
        page_favorites_update();
        return;
    }

    if (sel == ROW_ENABLE) {
        setting_favorites_list_t *list = viewed_list();

        btn_group_toggle_sel(&btn_group_fav);
        list->enable = btn_group_get_sel(&btn_group_fav) == 1;
        settings_put_bool((char *)favorites_ini_section(), "enable", list->enable);
        hint_label_update();
        return;
    }

    if ((sel != ROW_COUNT_SEL) && ((sel < ROW_SLOT_FIRST) || (sel > ROW_SLOT_LAST)))
        return;

    if (editing_row == sel) {
        favorites_save_row(sel);
        editing_row = -1;
        app_state_push(APP_STATE_SUBMENU);
    } else {
        editing_row = sel;
        app_state_push(APP_STATE_SUBMENU_ITEM_FOCUSED);
    }

    page_favorites_update();
}

page_pack_t pp_favorites = {
    .p_arr = {
        .cur = 0,
        .max = ROW_COUNT,
    },
    .name = FAVORITES_PAGE_NAME,
    .create = page_favorites_create,
    .enter = on_enter,
    .exit = on_exit,
    .on_created = NULL,
    .on_update = NULL,
    .on_roller = on_roller,
    .on_click = on_click,
    .on_right_button = NULL,
};
