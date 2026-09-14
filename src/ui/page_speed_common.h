#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ui/page_common.h"
#include "ui/ui_main_menu.h"

// Boot Speed, Switch Speed and Input Feel are the same page with different
// rows: a name, an Off/On pair, a column saying what the switch is worth, and
// a line under the list about whichever row is selected. This is that page;
// the three keep only their own rows, their own comment text and their own
// idea of when a row has nothing to do.

// Shown in place of the figure when a row's switch cannot do anything in the
// combination that is currently set. Short because the column is 180px; the
// reason goes in the comment under the list.
#define SPEED_SAVING_INERT "(no effect)"

// The grid every one of these pages uses. create_btn_group_item() gives its
// label a 320px box at column 1 and puts the first button's arrow at the start
// of column 2, so column 1 has to be wider than that box or the arrow lands on
// the name. The last column holds the saving and is clipped at the container
// edge, so it gets the rest. 90 + 340 + 175 + 175 + 180 fills 960 exactly, and
// the sixth track is there for the two-column spans to reach into.
#define SPEED_COL_DSC \
    { 90, 340, 175, 175, 180, 0, LV_GRID_TEMPLATE_LAST }

// Usable height of the list container, one row given back to the comment.
// Every one of these pages uses it, and every one of them scrolls, even the
// ones whose rows fit: the grid puts the theme's pad_row between rows and
// style_context does not override it, so a container sized at rows * height is
// short by one gap per row after the first. Sizing it by that arithmetic put
// Back off the bottom of the two pages that were not scrolling. The height is
// measured rather than derived now, and scrolling is what covers being wrong
// about it -- the list moves instead of losing its last row.
#define SPEED_LIST_H(row_h) (780 - (row_h))

typedef struct {
    lv_obj_t *cont;
    lv_obj_t *comment;
    // Per row, so a row can be greyed and its figure swapped after any switch
    // that changes whether it does anything.
    lv_obj_t *name[MAX_PANELS];
    lv_obj_t *saving[MAX_PANELS];
    const char *saving_text[MAX_PANELS];
} speed_page_t;

// Call once at the top of the page's create, before any row. Gives the
// container the scrolling every one of these pages relies on.
void speed_page_begin(speed_page_t *pg, lv_obj_t *cont);

// A group title, not a choice, so the dial passes over it.
void speed_heading(speed_page_t *pg, panel_arr_t *arr, const char *name, int row);

// One Off/On row: name on the left, the buttons in the middle, what it is
// worth on the right. name and saving are untranslated.
void speed_toggle(speed_page_t *pg, btn_group_t *group, const char *name,
                  bool value, const char *saving, int row);
// A row with three named choices and no figure: the third button lands where
// the figure would go. value is the chosen index.
void speed_choice(speed_page_t *pg, btn_group_t *group, const char *name,
                  const char *c0, const char *c1, const char *c2, int value, int row);

// A slider row, moved into the columns the toggles use: create_slider_item()
// puts its value label in column 5, which these pages have no room for.
void speed_slider(speed_page_t *pg, slider_group_t *slider, int row);

// The line under the list. pad_top clears it of the last row.
void speed_comment(speed_page_t *pg, lv_obj_t *section, lv_coord_t pad_top);
void speed_comment_set(speed_page_t *pg, const char *text);

// Grey the row and swap its figure for SPEED_SAVING_INERT, or put both back.
void speed_row_inert(speed_page_t *pg, int row, bool inert);

// Flips one toggle and stores it, so each row is a line rather than a block.
void speed_store(btn_group_t *group, bool *value, const char *section, const char *key);

// Brings the selection into view on a page taller than its container, with the
// row above it, because the dial skips over the headings and without this a
// heading scrolls off the moment the selection reaches the first item under
// it -- which is exactly when it is most wanted.
void speed_scroll_to(speed_page_t *pg, panel_arr_t *arr);

#ifdef __cplusplus
}
#endif
