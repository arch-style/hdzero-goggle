#include "ui_player.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <log/log.h>
#include <lvgl/lvgl.h>

#include "../conf/ui.h"
#include "common.hh"
#include "core/app_state.h"
#include "core/dvr.h"
#include "driver/hardware.h"
#include "player/avplay.h"
#include "record/record_definitions.h"
#include "ui/ui_porting.h"
#include "ui/ui_style.h"

///////////////////////////////////////////////////////////////////////////////
// locals
//  UI
static player_control_t controller;
static lv_style_t style_bg;
static lv_style_t style_bar;

LV_IMG_DECLARE(img_Play_0);
LV_IMG_DECLARE(img_Stop_0);
LV_IMG_DECLARE(img_star);

// The recording being played, through decoders of our own: see
// src/player/avplay.h for why not MPP's player. Pause, step, seek and resume
// all land on the frame, and the picture keeps time with the sound.
static AvPlay_t *pb_ap = NULL;

// Step size and play speed. Both survive the file being closed: picking them
// again for every clip is the kind of thing that makes a feature not worth
// having.
static const struct {
    const char *name;
    uint32_t ms; // 0 = one frame of the open file
} pb_steps[PB_STEP_COUNT] = {
    {"1 frame", 0},
    {"1s", 1000},
    {"5s", 5000},
    {"10s", 10000},
    {"30s", 30000},
};

static const struct {
    const char *name;
    uint8_t divider;
} pb_speeds[PB_SPEED_COUNT] = {
    {"1x", 1},
    {"1/2x", 2},
    {"1/4x", 4},
    {"1/8x", 8},
};

static uint8_t pb_step = PB_STEP_5S;
static uint8_t pb_speed = PB_SPEED_1_1;

// Dialling is collected into one seek. The dial sends its clicks in bursts,
// and each seek decodes from a keyframe, so the target moves with every click
// and is seeked to once the dial stops.
#define PB_SEEK_COALESCE_MS 120

static lv_timer_t *pb_seek_timer = NULL;
static bool pb_seek_pending = false;

// Keeps the bar in step with the picture.
#define PB_TICK_MS 100
static lv_timer_t *pb_tick_timer = NULL;

// The player runs the display at 1080p60, so that 60fps recordings are shown
// frame for frame; the menu's 1080p50 shows them 6:5 (Display_Playback()).
// Switching costs dispw's second each way, so it is done at the first file
// and undone when the Playback page is left, not per file.
static bool pb_display60 = true;
// Being tried: the live side at 720p60 in place of 1080p60, and which of the
// two is up.
static bool pb_display720 = false;
static bool pb_display720_up = false;
// Set by an autotest that compares displays: the bar says which is up.
static bool pb_display_label = false;
// Set by an autotest that tries variants: which one is up, in its words.
static char pb_label[48] = "";

// Open with the bar and nothing playing under it, for an autotest that drives
// the output itself (src/player/vosend.c). Only a long press is taken then:
// the rest of the keys would reach for a player that is not there.
static bool pb_bare = false;

static bool stars_positioned_on_timeline = false;
static size_t stars_count = 0;
static size_t stars_timestamps_s[MAX_STARS] = {
    0,
};

///////////////////////////////////////////////////////////////////////////////
static void time2str(uint32_t t1, uint32_t t2, char *s) {
    int m1, s1, m2, s2;
    m1 = (int)(t1 / 60000.0);
    s1 = (int)((t1 % 60000) / 1000.0);
    m2 = (int)(t2 / 60000.0);
    s2 = (int)((t2 % 60000) / 1000.0);

    sprintf(s, "%02d:%02d/%02d:%02d", m1, s1, m2, s2); // not considering for video over a hour
}

void mplayer_set_time(uint32_t now, uint32_t duration) {
    controller.value = (int32_t)now;
    controller.range = (int32_t)duration;
}

static void update_time_label(bool mediaOK) {
    char s[200];
    int percent;

    if (mediaOK) {
        uint32_t now = controller.value;
        uint32_t duration = controller.range;

        time2str(now, duration, s);
        lv_label_set_text(controller._label, s);
        percent = duration ? (now * 100 / duration) : 0;
        lv_slider_set_value(controller._slider, percent, LV_ANIM_OFF);

        if (!stars_positioned_on_timeline) {
            for (size_t i = 0; i < stars_count; i++) {
                int star_percent = duration ? (stars_timestamps_s[i] * 1000 * 100 / duration) : 0;

                lv_obj_set_pos(controller._stars[i],
                               UI_MPLAYER_BTN_GAP + UI_MPLAYER_BTN_WIDTH + UI_MPLAYER_BTN_GAP + UI_MPLAYER_SLD_WIDTH * star_percent / 100 - (img_star.header.w / 2), 20);
            }
            if (duration) {
                stars_positioned_on_timeline = true;
            }
        }

    } else {
        lv_label_set_text(controller._label, "Bad file");
        lv_slider_set_value(controller._slider, 0, LV_ANIM_OFF);
    }
}

static void update_mode_label() {
    char s[64];

    if (pb_label[0]) {
        snprintf(s, sizeof(s), "Step %s   Speed %s   [%s]", pb_steps[pb_step].name,
                 pb_speeds[pb_speed].name, pb_label);
    } else
#if defined(HDZGOGGLE2)
    if (pb_display_label) {
        if (pb_display60) {
            snprintf(s, sizeof(s), "Step %s   Speed %s   [60Hz %d %s]", pb_steps[pb_step].name,
                     pb_speeds[pb_speed].name, Display_Playback_color_index(),
                     Display_Playback_color_name());
        } else {
            snprintf(s, sizeof(s), "Step %s   Speed %s   [50Hz menu]", pb_steps[pb_step].name,
                     pb_speeds[pb_speed].name);
        }
    } else
#endif
        snprintf(s, sizeof(s), "Step %s   Speed %s", pb_steps[pb_step].name, pb_speeds[pb_speed].name);
    lv_label_set_text(controller._mode, s);
}

static void update_mplayer() {
    if (!controller.enable) {
        lv_obj_add_flag(controller.bg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(controller.bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_img_set_src(controller._btn, controller.is_playing ? &img_Stop_0 : &img_Play_0);
        update_mode_label();
        update_time_label(pb_ap != NULL);
    }
}

static void mplayer_create_btn(lv_obj_t *parent, int16_t x, int16_t y) {
    controller._btn = lv_img_create(parent);
    lv_obj_set_size(controller._btn, UI_MPLAYER_BTN_WIDTH, UI_MPLAYER_BTN_HEIGHT);
    lv_obj_set_pos(controller._btn, x, y);
}

static void mplayer_create_slider(lv_obj_t *parent, int16_t x, int16_t y) {
    char buf[25];
    controller._slider = lv_slider_create(parent);
    lv_obj_remove_style_all(controller._slider);
    lv_obj_add_style(controller._slider, &style_silder_main, LV_PART_MAIN);
    lv_obj_add_style(controller._slider, &style_silder_indicator, LV_PART_INDICATOR);
    lv_obj_add_style(controller._slider, &style_silder_pressed_color, LV_PART_INDICATOR | LV_STATE_PRESSED);
    lv_obj_add_style(controller._slider, &style_silder_knob, LV_PART_KNOB);
    lv_obj_add_style(controller._slider, &style_silder_pressed_color, LV_PART_KNOB | LV_STATE_PRESSED);

    lv_obj_set_pos(controller._slider, x, y + 20);
    lv_obj_set_size(controller._slider, UI_MPLAYER_SLD_WIDTH, UI_MPLAYER_SLD_HEIGHT);
    lv_slider_set_range(controller._slider, 0, 100); // in percentage
    lv_slider_set_value(controller._slider, 0, LV_ANIM_OFF);

    controller._label = lv_label_create(parent);
    snprintf(buf, sizeof(buf), "00:00/00:00");
    lv_label_set_text(controller._label, buf);
    lv_obj_set_style_text_font(controller._label, UI_PLAYER_TEXT_FONT, 0);
    lv_obj_set_style_text_align(controller._label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(controller._label, 12, 0);
    lv_label_set_long_mode(controller._label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(controller._label, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_pos(controller._label, x + UI_MPLAYER_SLD_WIDTH + UI_MPLAYER_BTN_GAP, y);
    lv_obj_set_size(controller._label, 160, UI_MPLAYER_BTN_HEIGHT);

    // Second line of the bar, clear of the slider's knob, which the knob's
    // 20px padding stretches well past the 3px track.
    controller._mode = lv_label_create(parent);
    lv_label_set_text(controller._mode, "");
    lv_obj_set_style_text_font(controller._mode, UI_PLAYER_TEXT_FONT, 0);
    lv_obj_set_style_text_align(controller._mode, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(controller._mode, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_pos(controller._mode, x, y + UI_MPLAYER_BTN_HEIGHT);
    lv_obj_set_width(controller._mode, UI_MPLAYER_SLD_WIDTH);

    for (size_t i = 0; i < stars_count; i++) {
        controller._stars[i] = lv_img_create(parent);
        lv_img_set_src(controller._stars[i], &img_star);
        lv_obj_set_pos(controller._stars[i], x, y + 20);
    }
}

static void init_mplayer() {
    int16_t x, y;

    // Taken from the display rather than from UI_MPLAYER_SCR_*: the player
    // runs on the UI canvas, which is 720p on the goggle 2 and 1080p on the
    // goggle 1, and a bar placed for 1080p sits off the bottom of a 720p
    // screen. The bar keeps its own size, so it covers proportionally more of
    // the smaller screen, which is the readable direction to err in.
    //
    // The canvas carries DISP_OVERSCAN that the flush does not put on screen,
    // so the visible area is that much smaller than the driver's resolution.
    const lv_coord_t scr_w = lv_disp_get_hor_res(NULL) - DISP_OVERSCAN;
    const lv_coord_t scr_h = lv_disp_get_ver_res(NULL) - DISP_OVERSCAN;

    // Background
    controller.bg = lv_obj_create(lv_scr_act());
    lv_obj_clear_flag(controller.bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(controller.bg, scr_w, scr_h);
    lv_obj_set_pos(controller.bg, 0, 0);

    // Controller
    controller.bar = lv_obj_create(lv_scr_act());
    lv_obj_clear_flag(controller.bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(controller.bar, UI_MPLAYER_CB_WIDTH, UI_MPLAYER_CB_HEIGHT);
    lv_obj_set_pos(controller.bar, (scr_w - UI_MPLAYER_CB_WIDTH) >> 1,
                   scr_h - UI_MPLAYER_CB_HEIGHT - UI_MPLAYER_CB_BOTTOM_GAP);

    // Buttons
    x = UI_MPLAYER_BTN_GAP;
    y = 0;
    mplayer_create_btn(controller.bar, x, y);

    // Slider
    x += (UI_MPLAYER_BTN_WIDTH + UI_MPLAYER_BTN_GAP);
    mplayer_create_slider(controller.bar, x, y);
    mplayer_set_time(1000 * 60, 1000 * 240);

    // Styles bg
    lv_style_reset(&style_bg);
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, LV_COLOR_MAKE_32(0x0, 0x0, 0x0, 0x0));
    lv_style_set_border_width(&style_bg, 0);
    lv_style_set_radius(&style_bg, 0);
    lv_obj_add_style(controller.bg, &style_bg, 0);

    // Controller bar
    lv_style_reset(&style_bar);
    lv_style_init(&style_bar);
    lv_style_set_bg_color(&style_bar, lv_color_make(0x26, 0x26, 0x26));
    lv_style_set_opa(&style_bar, 90);
    lv_style_set_radius(&style_bar, 0);
    lv_style_set_border_width(&style_bar, 0);
    lv_obj_add_style(controller.bar, &style_bar, 0);

    controller.enable = true;
    controller.is_playing = true;
    controller.value = controller.range = 0;
}

static void free_mplayer() {
    controller.enable = false;
    lv_obj_del(controller._btn);
    lv_obj_del(controller._label);
    lv_obj_del(controller._mode);
    lv_obj_del(controller._slider);
    for (size_t i = 0; i < stars_count; i++) {
        lv_obj_del(controller._stars[i]);
    }
    lv_obj_del(controller.bar);
    lv_obj_del(controller.bg);
}

///////////////////////////////////////////////////////////////////////////////
// key:
//   1 = dial up
//   2 = dial down
//   3 = click
//   4 = long press
// return
//   0 = still in mplayer page
//   1 = exit from mplayer page
void mplayer_exit();

static void pb_seek_cancel(void) {
    if (pb_seek_timer) {
        lv_timer_del(pb_seek_timer);
        pb_seek_timer = NULL;
    }
    pb_seek_pending = false;
}

// The resume tunables of the MPP player (the seek it had to aim short) have
// nothing left to tune: play carries on from the frame on screen.
bool mplayer_tune_set(const char *name, int value) {
    LOGW("tune: nothing to tune in this player ('%s'=%d ignored)", name, value);
    return false;
}

static void pb_play(void) {
    pb_seek_cancel();
    controller.is_playing = true;
    avplay_play(pb_ap, pb_speeds[pb_speed].divider);
}

static void pb_pause(void) {
    pb_seek_cancel();
    controller.is_playing = false;
    avplay_pause(pb_ap);
    controller.value = avplay_position_ms(pb_ap);
}

static void pb_frame_step(bool forward) {
    pb_seek_cancel();
    controller.is_playing = false;

    const int from = avplay_position_ms(pb_ap);
    const uint32_t t0 = lv_tick_get();
    const bool ok = avplay_step(pb_ap, forward);
    const int to = avplay_position_ms(pb_ap);

    controller.value = to;

    // frames is in hundredths, so a step of exactly one frame reads 100 and a
    // 16.6ms frame does not get rounded away.
    const int frame_ms = controller.frame_ms ? (int)controller.frame_ms : 16;
    LOGI("AT step dir=%s from=%d to=%d frames=%d ms=%u result=%s", forward ? "+" : "-", from, to,
         (to - from) * 100 / frame_ms, lv_tick_elaps(t0),
         !ok ? "end" : (forward ? to > from : to < from) ? "ok" : "wrongway");
}

static void pb_seek_now(void) {
    pb_seek_timer = NULL;
    pb_seek_pending = false;

    avplay_seek(pb_ap, controller.value); // keeps playing if it was
    controller.value = avplay_position_ms(pb_ap);
}

static void pb_seek_cb(lv_timer_t *timer) {
    (void)timer; // a one-shot timer deletes itself after this
    pb_seek_now();
    update_mplayer();
}

// Move the target and (re)start the collection window. Whatever the dial ends
// on is the one place the player seeks to.
static void pb_seek_by(int32_t delta_ms) {
    controller.value += delta_ms;

    if (controller.value > controller.range)
        controller.value = controller.range;
    if (controller.value < 0)
        controller.value = 0;

    pb_seek_pending = true;

    if (pb_seek_timer) {
        lv_timer_reset(pb_seek_timer);
    } else {
        pb_seek_timer = lv_timer_create(pb_seek_cb, PB_SEEK_COALESCE_MS, NULL);
        lv_timer_set_repeat_count(pb_seek_timer, 1);
    }
}

static void pb_tick_cb(lv_timer_t *timer) {
    (void)timer;

    if (!pb_ap || !controller.enable) {
        return;
    }

    // While clicks are still being collected, the label shows where the dial
    // is heading, not where the picture still is.
    if (!pb_seek_pending) {
        controller.value = avplay_position_ms(pb_ap);
    }

    // Played to the end: the last frame stays up and the button says play.
    if (controller.is_playing && avplay_state(pb_ap) == AVPLAY_ENDED) {
        controller.is_playing = false;
        controller.value = controller.range;
        update_mplayer();
        return;
    }

    update_time_label(true);
}

uint8_t mplayer_on_key(uint8_t key) {
    if (!controller.enable)
        return 0;

    LOGI("mplayer_on_key: %d", key);

    if (pb_bare && key != DIAL_KEY_PRESS)
        return 0;

    if (!pb_ap && key != DIAL_KEY_PRESS)
        return 0; // a file that would not open: only leaving makes sense

    switch (key) {
    case DIAL_KEY_PRESS:
        mplayer_exit();
        return 1;

    case DIAL_KEY_CLICK:
        if (controller.is_playing)
            pb_pause();
        else
            pb_play();
        break;

    case DIAL_KEY_UP:
        if (pb_steps[pb_step].ms == 0)
            pb_frame_step(true);
        else
            pb_seek_by((int32_t)pb_steps[pb_step].ms);
        break;

    case DIAL_KEY_DOWN:
        if (pb_steps[pb_step].ms == 0)
            pb_frame_step(false);
        else
            pb_seek_by(-(int32_t)pb_steps[pb_step].ms);
        break;

    // The dial is taken (click plays, long press leaves), so the function
    // button carries the two new settings.
    case RIGHT_KEY_CLICK:
        pb_step = (pb_step + 1) % PB_STEP_COUNT;
        break;

    case RIGHT_KEY_PRESS:
        pb_speed = (pb_speed + 1) % PB_SPEED_COUNT;
        if (controller.is_playing)
            pb_play(); // the new speed, with or without the sound
        break;
    }

    update_mplayer();

    return 0;
}

void load_stars(char *fname) {
    stars_positioned_on_timeline = false;
    char stars_filename[256] = "";
    snprintf(stars_filename, sizeof(stars_filename), "%s" REC_starSUFFIX, fname);

    stars_count = 0;
    FILE *stars_file = fopen(stars_filename, "r");

    if (stars_file) {
        unsigned mins = 0;
        unsigned secs = 0;
        while (fscanf(stars_file, REC_starFORMAT, &mins, &secs) == 2) {
            stars_timestamps_s[stars_count] = mins * 60 + secs;
            stars_count++;

            if (stars_count == MAX_STARS) {
                break;
            }
        }
        fclose(stars_file);
    }
}

///////////////////////////////////////////////////////////////////////////////
// interface func
static void pb_display_enter(void) {
#if defined(HDZGOGGLE2)
    if (pb_display60 && (!Display_Playback_on() || pb_display720_up != pb_display720)) {
        if (pb_display720) {
            Display_Playback_720();
        } else {
            Display_Playback();
        }
        pb_display720_up = pb_display720;
    }
    avplay_set_screen(pb_display60 && pb_display720 ? 1280 : 1920, pb_display60 && pb_display720 ? 720 : 1080);
#endif
}

void mplayer_release_display(void) {
#if defined(HDZGOGGLE2)
    if (Display_Playback_on()) {
        Display_UI();
    }
#endif
}

void mplayer_file(char *fname) {
    LOGI("mplayer %s", fname);
    load_stars(fname);
    pb_display_enter();
    init_mplayer();

    pb_ap = avplay_open(fname);
    if (pb_ap) {
        controller.range = avplay_duration_ms(pb_ap);
        controller.value = avplay_position_ms(pb_ap);
        controller.frame_ms = (uint32_t)avplay_frame_ms(pb_ap);
        LOGI("frame is %ums, %s", controller.frame_ms, avplay_has_audio(pb_ap) ? "with sound" : "silent");
        // After avplay_open(): starting MPP set the level this raises.
        if (avplay_has_audio(pb_ap)) {
            dvr_playback_audio_async(true);
        }
        pb_play();
    } else {
        LOGE("cannot play %s", fname);
        controller.is_playing = false;
    }

    pb_tick_timer = lv_timer_create(pb_tick_cb, PB_TICK_MS, NULL);
    update_mplayer();
}

void mplayer_exit() {
    pb_seek_cancel();
    if (pb_tick_timer) {
        lv_timer_del(pb_tick_timer);
        pb_tick_timer = NULL;
    }

    if (pb_ap) {
        const bool sound = avplay_has_audio(pb_ap);
        avplay_close(pb_ap);
        pb_ap = NULL;
        if (sound) {
            dvr_playback_audio_async(false);
        }
    }
    pb_bare = false;

    free_mplayer();
    // The display stays at the player's timing for the next file; leaving the
    // Playback page gives it back (mplayer_release_display()).
}

///////////////////////////////////////////////////////////////////////////////
// Entry points for core/autotest.c, which drives the player from a script on
// the card. All are called under lvgl_mutex, like the dial's own path.

void mplayer_autotest_open(const char *path) {
    // What page_playback does on a click, without the file list.
    mplayer_file((char *)path);
    app_state_push(APP_STATE_PLAYBACK);
}

void mplayer_autotest_open_bare(void) {
    init_mplayer();
    pb_bare = true;
    app_state_push(APP_STATE_PLAYBACK);
}

bool mplayer_is_open(void) {
    return controller.enable;
}

bool mplayer_is_playing(void) {
    return controller.enable && controller.is_playing;
}

bool mplayer_is_resuming(void) {
    return controller.enable && avplay_resuming(pb_ap);
}

int mplayer_position_ms(void) {
    return controller.enable ? controller.value : -1;
}

// Both may be called before a file is open; the value then applies to the
// next one. update_mplayer() touches the bar's objects, which only exist
// while a file is open (the first autotest crashed on exactly that).
void mplayer_set_step_size(int index) {
    if (index >= 0 && index < PB_STEP_COUNT) {
        pb_step = (uint8_t)index;
        if (controller.enable) {
            update_mplayer();
        }
    }
}

void mplayer_set_display60(int on) {
    pb_display60 = on != 0;
    pb_display720 = on == 720;
    pb_display_label = true;
    if (!pb_display60) {
        mplayer_release_display();
    }
}

void mplayer_set_display_color(int color) {
    pb_display_label = true;
#if defined(HDZGOGGLE2)
    Display_Playback_color(color);
#else
    (void)color;
#endif
    if (controller.enable) {
        update_mplayer();
    }
}

void mplayer_set_label(const char *text) {
    snprintf(pb_label, sizeof(pb_label), "%s", text);
    if (controller.enable) {
        update_mplayer();
    }
}

void mplayer_set_speed(int index) {
    if (index >= 0 && index < PB_SPEED_COUNT) {
        pb_speed = (uint8_t)index;
        if (controller.enable) {
            if (controller.is_playing && pb_ap) {
                pb_play();
            }
            update_mplayer();
        }
    }
}
