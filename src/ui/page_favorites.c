#include "page_favorites.h"

#include <stdio.h>
#include <string.h>

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
    ROW_ACTIVE = 0,
    ROW_COUNT_SEL,
    ROW_SLOT_FIRST,
    ROW_SLOT_LAST = ROW_SLOT_FIRST + FAVORITES_MAX - 1,
    ROW_BACK,
    ROW_HINT, // not selectable, just the last grid row we occupy
    ROW_COUNT = ROW_BACK + 1
};

static lv_coord_t col_dsc[] = {160, 200, 200, 160, 160, 160, LV_GRID_TEMPLATE_LAST};
// 51 rather than the usual 60: this page needs 11 selectable rows plus a hint
// line, more than any other page, and all of it has to stay on screen. The
// hint is small text, so its row is shorter.
static lv_coord_t row_dsc[] = {51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 30, LV_GRID_TEMPLATE_LAST};

// The set on screen is always the one in use: choosing it on the first row is
// also what opens it for editing. Only called while a set is in use; with
// favorites off the rows that would read it are hidden.
static setting_favorites_set_t *edited(void) {
    return favorites_set(g_setting.favorites.active);
}
static lv_obj_t *title_label;
static lv_obj_t *active_label;
static lv_obj_t *count_label;
static lv_obj_t *slot_label[FAVORITES_MAX];
static lv_obj_t *back_label;
static lv_obj_t *hint_label;

// Row currently being edited with the dial, or -1 when nothing is focused.
static int editing_row = -1;

// "<what>: <value>", the value in yellow while its row is being edited.
static void value_label_set(lv_obj_t *label, int row, const char *what, const char *value) {
    char buf[96];

    if (editing_row == row)
        snprintf(buf, sizeof(buf), "%s: #FFFF00 %s#", what, value);
    else
        snprintf(buf, sizeof(buf), "%s: %s", what, value);

    lv_label_set_text(label, buf);
}

static void active_label_update(void) {
    char value[16];
    uint8_t active = g_setting.favorites.active;

    if (active == 0)
        snprintf(value, sizeof(value), "%s", _lang("Off"));
    else
        snprintf(value, sizeof(value), "%s %d", _lang("Set"), active);

    value_label_set(active_label, ROW_ACTIVE, _lang("Set in use"), value);
}

static void count_label_update(void) {
    char buf[64];
    uint8_t count = edited()->count;

    if (editing_row == ROW_COUNT_SEL)
        snprintf(buf, sizeof(buf), "%s: #FFFF00 %d#", _lang("Number of channels"), count);
    else
        snprintf(buf, sizeof(buf), "%s: %d", _lang("Number of channels"), count);

    lv_label_set_text(count_label, buf);
}

static void title_label_update(void) {
    char buf[64];

    snprintf(buf, sizeof(buf), "%s:", _lang(FAVORITES_PAGE_NAME));
    lv_label_set_text(title_label, buf);
}

static void slot_label_update(int slot) {
    char buf[64];
    char named[32];
    setting_favorites_set_t *fs = edited();
    uint8_t band = g_setting.source.hdzero_band;
    uint8_t ch = fs->channel[slot];
    const char *value;

    if (ch == 0) {
        value = _lang("Empty");
    } else {
        // The set is shared, so say what HDZero makes of the slot whenever it
        // is not simply the same channel: skipped because HDZero has no such
        // channel, skipped because it is on the band not selected, or tuned
        // under another name (F8 is R7's frequency). Plain text, no recolour
        // markup: the row being edited wraps the whole value in a colour of
        // its own and the two would nest.
        const char *name = channel2str(0, 0, ch);
        uint8_t hdz = favorites_hdzero_channel(band, ch);

        if (hdz != 0) {
            const char *hdz_name = channel2str(1, band, hdz);

            if (strcmp(name, hdz_name) == 0)
                snprintf(named, sizeof(named), "%s", name);
            else
                snprintf(named, sizeof(named), "%s (HDZero %s)", name, hdz_name);
        } else if (favorites_hdzero_channel(!band, ch) != 0) {
            snprintf(named, sizeof(named), "%s (%s)", name, _lang("other HDZero band"));
        } else {
            snprintf(named, sizeof(named), "%s (%s)", name, _lang("analog only"));
        }
        value = named;
    }

    if (editing_row == ROW_SLOT_FIRST + slot)
        snprintf(buf, sizeof(buf), "%s %d: #FFFF00 %s#", _lang("Slot"), slot + 1, value);
    else if (favorites_slot_duplicate(g_setting.favorites.active, slot))
        snprintf(buf, sizeof(buf), "%s %d: %s #FF8000 (%s)#", _lang("Slot"), slot + 1, value, _lang("duplicate"));
    else
        snprintf(buf, sizeof(buf), "%s %d: %s", _lang("Slot"), slot + 1, value);

    lv_label_set_text(slot_label[slot], buf);

    // Slots past the configured count stay on screen but greyed out, and the
    // dial skips over them.
    bool in_use = (slot < fs->count);
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
    int set = g_setting.favorites.active;
    int valid_hdzero = favorites_valid_count(set, FAVORITES_SOURCE_HDZERO);
    int valid_analog = favorites_valid_count(set, FAVORITES_SOURCE_ANALOG);
    int valid_here = favorites_valid_count(set, favorites_source());

    if (editing_row >= 0)
        snprintf(buf, sizeof(buf), "%s", _lang("Turn the dial to change, click to confirm"));
    else if (valid_analog == 0)
        // Analog takes every channel, so this is a set with nothing in it.
        snprintf(buf, sizeof(buf), "#FF8000 %s#", _lang("Register at least 1 channel to use favorites"));
    else if (valid_hdzero == 0)
        snprintf(buf, sizeof(buf), "#FF8000 %s#", _lang("No channel here is on HDZero's band; HDZero walks the whole band"));
    else if (valid_here == 1)
        snprintf(buf, sizeof(buf), "%s", _lang("The dial stays locked to this channel"));
    else
        snprintf(buf, sizeof(buf), " ");

    lv_label_set_text(hint_label, buf);
}

static void show(lv_obj_t *obj, bool on) {
    if (on)
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

// With favorites off there is nothing to edit, so the page comes down to the
// first row and Back. Back moves up under the first row rather than staying
// at the bottom of a blank page: grid rows keep their height when what is in
// them is hidden. Its selection panel moves with it, and keeps its index, so
// it is still the last row as far as submenu_click() is concerned.
static void rows_show(bool on) {
    int back_row = on ? ROW_BACK : ROW_ACTIVE + 1;
    lv_obj_t *back_panel = pp_favorites.p_arr.panel[ROW_BACK];

    show(count_label, on);
    for (int i = 0; i < FAVORITES_MAX; i++)
        show(slot_label[i], on);
    show(hint_label, on);

    if (on)
        lv_obj_add_flag(pp_favorites.p_arr.panel[ROW_COUNT_SEL], FLAG_SELECTABLE);
    else
        lv_obj_clear_flag(pp_favorites.p_arr.panel[ROW_COUNT_SEL], FLAG_SELECTABLE);

    lv_obj_set_grid_cell(back_label, LV_GRID_ALIGN_START, 1, 3, LV_GRID_ALIGN_CENTER, back_row, 1);
    lv_obj_set_grid_cell(back_panel, LV_GRID_ALIGN_STRETCH, 0, 6, LV_GRID_ALIGN_STRETCH, back_row, 1);
}

static void page_favorites_update(void) {
    bool on = (g_setting.favorites.active != 0);

    title_label_update();
    active_label_update();
    rows_show(on);
    if (!on) {
        // The slot panels are left selectable by whatever set was last shown.
        for (int i = 0; i < FAVORITES_MAX; i++)
            lv_obj_clear_flag(pp_favorites.p_arr.panel[ROW_SLOT_FIRST + i], FLAG_SELECTABLE);
        return;
    }

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

    // Built here rather than with create_text(), which keeps no handle on its
    // label; the styling matches what it would apply.
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

    // Chosen with the dial like the channel count, because
    // create_btn_group_item() stops at three buttons and Off plus four sets
    // makes five.
    active_label = create_label_item(cont, "", 1, row++, 3);

    count_label = create_label_item(cont, "", 1, row++, 3);

    for (int i = 0; i < FAVORITES_MAX; i++)
        slot_label[i] = create_label_item(cont, "", 1, row++, 3);

    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    back_label = create_label_item(cont, buf, 1, row++, 3);

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
    // changes what HDZero makes of each slot.
    editing_row = -1;
    page_favorites_update();
}

static void on_exit(void) {
    editing_row = -1;
}

static void on_roller(uint8_t key) {
    if (g_app_state != APP_STATE_SUBMENU_ITEM_FOCUSED)
        return;

    setting_favorites_set_t *fs = edited();

    if (editing_row == ROW_ACTIVE) {
        // The rest of the page follows as the dial turns, so a set can be
        // recognised by its channels before the click.
        uint8_t active = g_setting.favorites.active;

        if (key == DIAL_KEY_UP)
            active = (active >= FAVORITES_SETS) ? 0 : active + 1;
        else if (key == DIAL_KEY_DOWN)
            active = (active == 0) ? FAVORITES_SETS : active - 1;

        g_setting.favorites.active = active;
        page_favorites_update();
        return;
    }

    if (editing_row == ROW_COUNT_SEL) {
        uint8_t count = fs->count;

        if (key == DIAL_KEY_UP)
            count = (count >= FAVORITES_MAX) ? 1 : count + 1;
        else if (key == DIAL_KEY_DOWN)
            count = (count <= 1) ? FAVORITES_MAX : count - 1;

        fs->count = count;
        page_favorites_update();
        return;
    }

    if ((editing_row < ROW_SLOT_FIRST) || (editing_row > ROW_SLOT_LAST))
        return;

    int slot = editing_row - ROW_SLOT_FIRST;

    // 0 is the "empty" entry, so the value wraps over [0, channel count].
    // Every slot holds an analog channel, the superset HDZero's are part of.
    uint8_t ch = fs->channel[slot];
    uint8_t max = ANALOG_CHANNEL_NUM;

    if (key == DIAL_KEY_UP)
        ch = (ch >= max) ? 0 : ch + 1;
    else if (key == DIAL_KEY_DOWN)
        ch = (ch == 0) ? max : ch - 1;

    fs->channel[slot] = ch;
    slot_label_update(slot);
    hint_label_update();
}

static void favorites_save_row(int row) {
    const setting_favorites_set_t *fs = edited();
    char section[24];
    char key[8];

    if (row == ROW_ACTIVE) {
        ini_putl(FAVORITES_INI_SETS, "active", g_setting.favorites.active, SETTING_INI);
        LOGI("favorites: set %d in use", g_setting.favorites.active);
        return;
    }

    snprintf(section, sizeof(section), FAVORITES_INI_SET, g_setting.favorites.active);

    if (row == ROW_COUNT_SEL) {
        ini_putl(section, "count", fs->count, SETTING_INI);
        LOGI("favorites(%s): count = %d", section, fs->count);
        return;
    }

    int slot = row - ROW_SLOT_FIRST;
    snprintf(key, sizeof(key), "ch%d", slot + 1);
    ini_putl(section, key, fs->channel[slot], SETTING_INI);
    LOGI("favorites(%s): slot %d = channel %d", section, slot + 1, fs->channel[slot]);
}

static void on_click(uint8_t key, int sel) {
    if ((sel != ROW_ACTIVE) && (sel != ROW_COUNT_SEL) && ((sel < ROW_SLOT_FIRST) || (sel > ROW_SLOT_LAST)))
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
