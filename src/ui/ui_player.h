#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include <lvgl/lvgl.h>

#include "player/media.h"

#define LV_COLOR_MAKE_32(a, r, g, b) \
    _LV_COLOR_MAKE_TYPE_HELPER {     \
        {                            \
            b, g, r, a               \
        }                            \
    }

#define MAX_STARS 10

// How far one dial click moves. PB_STEP_FRAME is one frame of whatever the
// file was recorded at; the rest are wall-clock seconds.
typedef enum {
    PB_STEP_FRAME = 0,
    PB_STEP_1S,
    PB_STEP_5S,
    PB_STEP_10S,
    PB_STEP_30S,
    PB_STEP_COUNT,
} pb_step_t;

typedef enum {
    PB_SPEED_1_1 = 0,
    PB_SPEED_1_2,
    PB_SPEED_1_4,
    PB_SPEED_1_8,
    PB_SPEED_COUNT,
} pb_speed_t;

typedef struct {
    lv_obj_t *_btn;
    lv_obj_t *_slider;
    lv_obj_t *_label; // text format as "mm:ss(playing)/mm:ss(total)"
    lv_obj_t *_mode;  // step size and play speed, under the timeline
    lv_obj_t *_stars[MAX_STARS];
    bool is_playing;
    int32_t value;
    int32_t range;
    uint32_t frame_ms; // one frame of the open file

    bool enable;
    lv_obj_t *bg;
    lv_obj_t *bar;
} player_control_t;

uint8_t mplayer_on_key(uint8_t key);
void mplayer_set_time(uint32_t now, uint32_t duration);
void mplayer_file(char *fname);

#ifdef __cplusplus
}
#endif
