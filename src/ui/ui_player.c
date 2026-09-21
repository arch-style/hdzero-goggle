#include "ui_player.h"

#include <pthread.h>
#include <stdio.h>

#include <log/log.h>
#include <lvgl/lvgl.h>

#include "../conf/ui.h"
#include "common.hh"
#include "player/media.h"
#include "record/record_definitions.h"
#include "ui/ui_porting.h"
#include "ui/ui_style.h"

///////////////////////////////////////////////////////////////////////////////
// locals
//  UI
static player_control_t controller;
static lv_style_t style_bg;
static lv_style_t style_bar;
// media control
media_t *media;
player_cmd_t cmd;

LV_IMG_DECLARE(img_Play_0);
LV_IMG_DECLARE(img_Stop_0);
LV_IMG_DECLARE(img_star);

// Step size and play speed.
//
// The SoC's player has no play-rate call and no frame-step call
// (AW_MPI_VO_StepChn is commented out of mpi_vo.h and is not exported by
// libmedia_mpp.so), so both are paced from here: the pipeline is let run for a
// short burst and paused again. Seeking cannot stand in for a small step,
// because AW_MPI_DEMUX_Seek lands on a keyframe and the recorder writes one
// per second (maxKeyItl = fps), so a one-frame seek either does nothing or
// jumps a whole second.
//
// Step size and speed survive the file being closed: picking them again for
// every clip is the kind of thing that makes a feature not worth having.
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

// Media time per burst while playing slow. One frame would be the smooth
// choice, but lv_timer_handler() runs about every 10ms with work in between,
// so a burst that short would be swallowed by the pacing error.
#define PB_BURST_MS 50

static uint8_t pb_step = PB_STEP_5S;
static uint8_t pb_speed = PB_SPEED_1_1;

// The gate that makes slow play out of full-speed play.
static lv_timer_t *pb_gate_timer = NULL;
static bool pb_gate_running = false;

// Dialling is collected into one seek. Each seek costs the demuxer a keyframe
// search and the decoder a restart, and the dial sends its clicks in bursts,
// so seeking per click made a held dial feel like treacle and gave the
// pipeline a queue of seeks to wedge on. The target moves with every click and
// is seeked to once the dial stops.
#define PB_SEEK_COALESCE_MS 120

static lv_timer_t *pb_seek_timer = NULL;
static bool pb_seek_pending = false;

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
        update_time_label(media != NULL);
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
void media_start();
void media_stop();
void media_pause();
void media_seek(uint32_t seekto);
void mplayer_exit();

// The pause half of a slow-play cycle, in ms of wall clock.
static uint32_t pb_gate_pause_ms(void) {
    return PB_BURST_MS * (pb_speeds[pb_speed].divider - 1);
}

static void pb_gate_stop(void) {
    if (pb_gate_timer) {
        lv_timer_del(pb_gate_timer);
        pb_gate_timer = NULL;
    }
    pb_gate_running = false;
}

static void pb_gate_cb(lv_timer_t *timer) {
    if (pb_gate_running) {
        media_pause();
        pb_gate_running = false;
        lv_timer_set_period(timer, pb_gate_pause_ms());
    } else {
        media_start();
        pb_gate_running = true;
        lv_timer_set_period(timer, PB_BURST_MS);
    }
}

// Run the pipeline in bursts so it covers PB_BURST_MS of the file every
// PB_BURST_MS * divider of wall clock. The sound goes with it, in chunks; the
// point of slow play here is reading what happened on the video.
static void pb_gate_start(void) {
    pb_gate_stop();
    media_start();
    pb_gate_running = true;
    pb_gate_timer = lv_timer_create(pb_gate_cb, PB_BURST_MS, NULL);
}

static void pb_seek_cancel(void) {
    if (pb_seek_timer) {
        lv_timer_del(pb_seek_timer);
        pb_seek_timer = NULL;
    }
    pb_seek_pending = false;
}

static void pb_play(void) {
    pb_seek_cancel();
    controller.is_playing = true;

    if (pb_speed == PB_SPEED_1_1) {
        pb_gate_stop();
        media_start();
    } else {
        pb_gate_start();
    }
}

static void pb_pause(void) {
    pb_gate_stop();
    controller.is_playing = false;
    media_pause();
}

// One frame forward, from the media clock rather than from a timer. This
// blocks for about a frame, which is what the dial asked for.
static void pb_step_forward(void) {
    pb_gate_stop();
    pb_seek_cancel();
    controller.is_playing = false;

    controller.value = (int32_t)media_step_frame(media, controller.frame_ms);
}

static void pb_seek_now(void) {
    pb_seek_timer = NULL;
    pb_seek_pending = false;

    if (controller.is_playing) {
        media_seek(controller.value); // keeps playing, gate and all
    } else {
        controller.value = (int32_t)media_seek_show(media, (uint32_t)controller.value, controller.frame_ms);
    }
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

// One frame back has to be a seek: the pipeline only runs forwards. The
// demuxer seeks to a keyframe, so where this lands is the file's business, but
// the target keeps moving back a frame per click either way.
static void pb_step_back(void) {
    pb_gate_stop();
    controller.is_playing = false;
    pb_seek_by(-(int32_t)controller.frame_ms);
}

uint8_t mplayer_on_key(uint8_t key) {
    if (!controller.enable)
        return 0;

    LOGI("mplayer_on_key: %d", key);

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
            pb_step_forward();
        else
            pb_seek_by((int32_t)pb_steps[pb_step].ms);
        break;

    case DIAL_KEY_DOWN:
        if (pb_steps[pb_step].ms == 0)
            pb_step_back();
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
            pb_play(); // picks the gate up, or drops it for 1x
        break;
    }

    update_mplayer();

    return 0;
}

///////////////////////////////////////////////////////////////////////////////
//
static void notify_cb(media_info_t *info) {
    // LOGI("%d/%d",info->playing_time,info->duration);

    if (!media)
        return;
    if (info->playing_time == 0xFFFFFFFF)
        return;
    if (info->playing_time == 0xFFFFFFFE) {
        // End of file: play_moveStatus() has stopped the pipeline, so there is
        // nothing left to pace. The lvgl lock is what serialises this against
        // the UI thread's lv_timer_handler().
        pthread_mutex_lock(&lvgl_mutex);
        pb_gate_stop();
        pthread_mutex_unlock(&lvgl_mutex);
        return;
    }

    // While clicks are still being collected, the label shows where the dial
    // is heading; the clock would drag it back to where the picture still is.
    if (pb_seek_pending) {
        controller.range = (int32_t)info->duration;
    } else {
        mplayer_set_time(info->playing_time, info->duration);
    }

    pthread_mutex_lock(&lvgl_mutex);
    update_time_label(true);
    lv_timer_handler();
    pthread_mutex_unlock(&lvgl_mutex);
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

void media_init(char *fname) {
    media = media_instantiate(fname, notify_cb);
    if (!media) {
        perror("media_instantiate failed.");
        return;
    }
}

void media_start() {
    if (!media)
        return;
    cmd.opt = PLAYER_START;
    cmd.params = 0;

    media_control(media, &cmd);
}

void media_stop() {
    if (!media)
        return;
    cmd.opt = PLAYER_STOP;
    cmd.params = 0;
    media_control(media, &cmd);
}

void media_pause() {
    if (!media)
        return;
    cmd.opt = PLAYER_PAUSE;
    cmd.params = 0;
    media_control(media, &cmd);
}

void media_seek(uint32_t seekto) {
    if (!media)
        return;

    // pause
    media_pause();

    // seek
    cmd.opt = PLAYER_SEEK;
    cmd.params = seekto;
    media_control(media, &cmd);

    // start
    if (controller.is_playing) {
        if (pb_speed == PB_SPEED_1_1) {
            media_start();
        } else {
            pb_gate_start();
        }
    }

    // LOGI("media_seek %d",seekto);
}

///////////////////////////////////////////////////////////////////////////////
// interface func
void mplayer_file(char *fname) {
    LOGI("mplayer %s", fname);
    load_stars(fname);
    init_mplayer();
    media_init(fname);
    controller.frame_ms = media_frame_interval_ms(media);
    LOGI("frame is %ums", controller.frame_ms);
    pb_play();
    update_mplayer();
}

void mplayer_exit() {
    pb_gate_stop();
    pb_seek_cancel();
    pthread_mutex_unlock(&lvgl_mutex);
    if (media) {
        media_exit(media);
        media = NULL;
    }
    pthread_mutex_lock(&lvgl_mutex);
    free_mplayer();
}
