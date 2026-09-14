#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <lvgl/lvgl.h>

int statusbar_init(void);
void statubar_update(void);
void statusbar_set_zoom(lv_coord_t zoom);
void statusbar_show(bool show);

#ifdef __cplusplus
}
#endif
