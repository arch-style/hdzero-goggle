#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <lvgl/lvgl.h>

#include "core/settings.h"
#include "ui/ui_main_menu.h"

extern page_pack_t pp_source;

void source_status_timer();
void source_toggle();
void source_cycle();
bool source_select_named(const char *name);
void source_toggle_hdzero_bw();
// What the Source page's HDZero and Analog rows do: from video, switch there.
void source_select_hdzero();
void source_select_analog();
// Store the HDZero bandwidth and show it on the Source page. Takes effect at
// the next tuner open.
void source_set_hdzero_bw(setting_sources_hdzero_bw_t bw);

#ifdef __cplusplus
}
#endif
