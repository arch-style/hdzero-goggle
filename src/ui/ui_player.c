#include "ui_player.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <minIni.h>

#include <log/log.h>
#include <lvgl/lvgl.h>

#include "../conf/ui.h"
#include "common.hh"
#include "core/app_state.h"
#include "player/framestep.h"
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

// Frame-exact stepping, decoded here rather than by MPP: see
// src/player/framestep.h for why none of the pipeline can do it. Opened on the
// first step rather than with the file, because opening costs a demux of its
// own and most viewing never steps.
static char pb_path[256];
static FrameStep_t *pb_stepper = NULL;
static bool pb_stepper_failed = false;
static lv_obj_t *pb_frame_img = NULL;
static lv_img_dsc_t pb_frame_dsc;

// The picture on screen at the moment of pausing. Read before the pause:
// once the output is paused it answers with the last keyframe instead, which
// put the first step up to a second behind where the eye was.
static int pb_paused_at = -1;

// Open with the bar and nothing playing under it, for an autotest that drives
// the output itself (src/player/vosend.c). Only a long press is taken then:
// the rest of the keys would reach for a player that is not there.
static bool pb_bare = false;

static void pb_pause(void);

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

// True while the picture on screen is one this decoded, rather than one MPP
// put there. The two do not agree on position: MPP is paused wherever it was
// left, so its clock must not be allowed to drag the label back.
static bool pb_frame_at = false;

// Resuming playback from a decoded frame: where it is heading, what was
// asked of the player, and what the output reported before that.
//
// The player cannot start on an arbitrary frame, and its seek does not land
// where it is asked to either. Measured over seven resumes of a 1s-GOP TS
// file (hdz_9048.ts), with ffprobe giving the byte positions:
//
//   asked    sound started   first picture
//   39100    40083           40999
//   41100    42045           42999
//   47100    47997           48999
//   49100    50088           50999
//
// The demuxer lands 540..990ms *after* the point asked for, never before;
// the spread says it estimates a byte offset from the average bitrate and
// scans forward. The landing then falls a few hundred KB inside the
// keyframe's data (the 40.000s I-frame starts at byte 112,847,564, the first
// sound packet delivered was at 113,266,616), so that keyframe is lost and
// the decoder discards everything up to the next one. The picture therefore
// starts at the keyframe *after* the one following the landing.
//
// So the resume asks for a point more than a GOP before the keyframe that
// precedes the frame on screen: the landing falls in the GOP before it, the
// picture starts on that keyframe or the one before, and plays under the
// decoded picture until it reaches the frame that was on screen. The output
// keeps reporting the paused frame until it has actually put up a new one,
// so the value it had before the seek is remembered and ignored. If the
// first new frame is past the target anyway (a file whose bitrate makes the
// landing miss by more), or nothing new appears for a while (once in seven,
// the sound did not restart and the render waited three seconds for it),
// the seek is repeated a second further back, a couple of times, before
// giving up and letting the jump happen.
static int pb_catchup_ms = -1;     // the frame on screen; -1 when not resuming
static int pb_catchup_ask = -1;    // what the player was last asked for
static int pb_catchup_stale = -1;  // what the output reported before that seek
static int pb_catchup_tries = 0;   // seeks issued so far for this resume
static uint32_t pb_catchup_since = 0; // lv_tick_get() at the last seek
static uint32_t pb_catchup_began = 0; // lv_tick_get() when play was pressed
static bool pb_catchup_fresh = false; // the output has shown something new since the seek

// Where the player's seek lands relative to what it is asked for, on the
// file now open, in ms; learned from the first resume and used to aim the
// next ones closer. The offset is a property of the file (average bitrate
// against the bitrate around the point asked for): +540..+990ms on one
// recording, -200..-35ms on another, steady to within half a GOP on each.
static int pb_land_bias = 0;       // the largest (latest) landing seen on this file
static bool pb_land_known = false;
static uint32_t pb_catchup_first_at = 0; // lv_tick_get() at the first new picture

// How far before the keyframe to ask for while the file's landing offset is
// not known. Landings measured from 1475ms short of the ask to 990ms past it
// over five runs; this keeps even the latest 300ms before the keyframe.
// (1800 cost the first resume on every file a whole extra GOP.)
#define PB_RESUME_LEAD_MS 1300
// Where to aim the landing once the offset is known: the middle of the GOP
// before the keyframe, so half a GOP of error either way still starts the
// picture on that keyframe. The hold is then about this plus the distance
// from the keyframe to the frame on screen.
#define PB_RESUME_AIM_MS 500
// The GOP the aim assumes; the recorder writes one keyframe a second.
#define PB_RESUME_GOP_MS 1000
// How much further back each repeated seek asks for, and how many.
#define PB_RESUME_RETRY_MS 1000
#define PB_RESUME_RETRIES 2
// How long to wait for the first new frame before repeating the seek. It
// shows 360..420ms after the seek when the render gets going. When the
// video's first frame reaches the render before the sound's first frame
// starts the clock, the render misses the start and sits until its own
// three-second timeout (or never, seen once); a repeated seek gets it going.
#define PB_RESUME_SILENT_MS 900
// The sound's first frame, which starts the clock, has followed the seek by
// 212..225ms every time. Used to work the landing back from the clock.
#define PB_RESUME_SOUND_MS 215
// Before the output has shown anything new it keeps reporting the paused
// frame, or one or two frames past it (PauseChn() lets a couple through).
// Anything within this band of that value is taken as still the old picture.
#define PB_RESUME_STALE_BAND_MS 200
// Take the picture down this far before the output reaches the target:
// hiding it is a full-screen redraw that takes a few frames, during which
// the output keeps moving.
#define PB_RESUME_DROP_LEAD_MS 60
// How long one call of the watch may sit polling the output before handing
// the thread back. Longer than a resume ever takes; it is only a backstop.
#define PB_RESUME_WATCH_MS 4000
// How far before the keyframe the latest landing seen on this file should
// still fall. Landings on one file spread over 1.3s (hdz_0012: 100..1475ms
// short of the ask; hdz_9048: 620 short to 585 past), so the ask is set by
// the worst one seen so far, not the average: a landing past the keyframe
// costs a whole second seek, one short of it only the distance.
//
// 250, 400 and 550ms measured the same on average (2.2s hold over twelve
// resumes each); 400 is the middle.
#define PB_RESUME_MARGIN_MS 400
// The clock starts when the first picture is decoded, if the sound has
// started by then. Sometimes the sound never starts: 12 of 50 seeks over two
// runs, with no "AO set config" line at all, and not tied to where the seek
// landed (margins of 250, 400 and 550ms all had them). The render then sits
// on the picture for three seconds before forcing the clock; if the clock is
// not running this long after the first picture, the seek is repeated
// instead. Where the sound did start after the picture, it was 2..21ms
// after, so 150ms tells the two apart and saves 350ms per stall over 500.
#define PB_RESUME_STALL_MS 150

// The same values at run time, so one can be tried without a rebuild: from
// [step] in /mnt/extsd/step.ini when a file opens, or from an autotest "set"
// line. TUNE(X) is what the code reads; the defines above are the defaults.
static struct {
    const char *name;
    int value;
} pb_tune[] = {
    {"resume_lead", PB_RESUME_LEAD_MS},
    {"resume_aim", PB_RESUME_AIM_MS},
    {"resume_gop", PB_RESUME_GOP_MS},
    {"resume_retry", PB_RESUME_RETRY_MS},
    {"resume_retries", PB_RESUME_RETRIES},
    {"resume_silent", PB_RESUME_SILENT_MS},
    {"resume_sound", PB_RESUME_SOUND_MS},
    {"resume_band", PB_RESUME_STALE_BAND_MS},
    {"resume_drop_lead", PB_RESUME_DROP_LEAD_MS},
    {"resume_watch", PB_RESUME_WATCH_MS},
    {"resume_margin", PB_RESUME_MARGIN_MS},
    {"resume_stall", PB_RESUME_STALL_MS},
};
enum { TUNE_LEAD, TUNE_AIM, TUNE_GOP, TUNE_RETRY, TUNE_RETRIES, TUNE_SILENT, TUNE_SOUND, TUNE_BAND, TUNE_DROP_LEAD, TUNE_WATCH, TUNE_MARGIN, TUNE_STALL, TUNE_COUNT };
#define TUNE(x) (pb_tune[TUNE_##x].value)


bool mplayer_tune_set(const char *name, int value) {
    for (int i = 0; i < TUNE_COUNT; i++) {
        if (strcmp(pb_tune[i].name, name) == 0) {
            LOGI("AT tune %s=%d (was %d)", name, value, pb_tune[i].value);
            pb_tune[i].value = value;
            return true;
        }
    }
    LOGW("tune: no such value '%s'", name);
    return false;
}

#define PB_TUNE_INI "/mnt/extsd/step.ini"

static void pb_tune_load(void) {
    if (access(PB_TUNE_INI, R_OK) != 0) {
        return;
    }
    for (int i = 0; i < TUNE_COUNT; i++) {
        int v = (int)ini_getl("step", pb_tune[i].name, pb_tune[i].value, PB_TUNE_INI);
        if (v != pb_tune[i].value) {
            LOGI("tune: %s=%d from " PB_TUNE_INI, pb_tune[i].name, v);
            pb_tune[i].value = v;
        }
    }
}

// What the last resume measured, for the AT line that reports it.
static int pb_catchup_key = -1;     // the keyframe the resume aimed at
static int pb_catchup_landing = -1; // where the seek landed, worked back from the clock
static int pb_catchup_first = -1;   // the first new picture after the seek
static int pb_catchup_drop_at = -1; // where the output was when the picture came down

// The resume is over, one way or another.
//
// The sound is left running during the hold, so the second before the frame
// is heard again. It was muted through AW_MPI_AO_SetDevMute() once: that
// goes through the ALSA mixer, cost 180ms a call, and on the fifth call
// never returned (last log line "alsaMixerSetMute ... state: 1"), with the
// lvgl lock held and the beeper left on. Not worth a second try.
static void pb_catchup_end(void) {
    pb_catchup_ms = -1;
    pb_catchup_ask = -1;
    pb_catchup_stale = -1;
    pb_catchup_tries = 0;
}

// One seek of a resume: the player plays from wherever it lands to the
// target under the picture.
static void pb_catchup_seek(int ask) {
    if (ask < 0) {
        ask = 0;
    }
    pb_catchup_ask = ask;
    pb_catchup_stale = media_shown_ms(media);
    pb_catchup_since = lv_tick_get();
    pb_catchup_fresh = false;
    pb_catchup_tries++;
    LOGI("step: resume seek %d: asking the player for %dms (output was at %d)",
         pb_catchup_tries, ask, pb_catchup_stale);
    media_seek((uint32_t)ask);
}

static void pb_frame_hide(void) {
    if (pb_frame_img) {
        lv_obj_add_flag(pb_frame_img, LV_OBJ_FLAG_HIDDEN);
    }
    pb_frame_at = false;
    pb_catchup_end();
}

static bool pb_stepper_ready(void) {
    if (pb_stepper) {
        return true;
    }
    if (pb_stepper_failed || !pb_path[0]) {
        return false;
    }

    pb_stepper = framestep_open(pb_path);
    if (!pb_stepper) {
        // Asked for once and then left alone: a file this cannot decode will
        // not start decoding on the next click.
        LOGI("step: no decoder for %s", pb_path);
        pb_stepper_failed = true;
        return false;
    }

    // Decoded straight to the size it is drawn at. Handing LVGL a native
    // frame and a zoom makes it resample the whole screen on every step.
    framestep_set_output_size(pb_stepper,
                              lv_disp_get_hor_res(NULL) - DISP_OVERSCAN,
                              lv_disp_get_ver_res(NULL) - DISP_OVERSCAN);

    return true;
}

static void pb_frame_show(const FrameStepPic_t *pic) {
    pb_frame_dsc.header.always_zero = 0;
    pb_frame_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    pb_frame_dsc.header.w = pic->width;
    pb_frame_dsc.header.h = pic->height;
    pb_frame_dsc.data = pic->pixels;
    pb_frame_dsc.data_size = (uint32_t)pic->width * pic->height * sizeof(lv_color_t);

    if (!pb_frame_img) {
        pb_frame_img = lv_img_create(lv_scr_act());
        lv_obj_clear_flag(pb_frame_img, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(pb_frame_img, 0, 0);
    }

    // The buffer is reused, so the address says nothing about the contents and
    // the cache would hand back the frame before this one.
    lv_img_cache_invalidate_src(&pb_frame_dsc);
    lv_img_set_src(pb_frame_img, &pb_frame_dsc);
    lv_obj_clear_flag(pb_frame_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(pb_frame_img, pic->width, pic->height);
    lv_obj_invalidate(pb_frame_img);
    // Over the video the player left on screen, under the control bar.
    lv_obj_move_foreground(controller.bar);

    pb_frame_at = true;

    LOGI("step: showing %dms, %dx%d at %d,%d size %dx%d, index %d of %d",
         pic->pts_ms, pic->width, pic->height,
         (int)lv_obj_get_x(pb_frame_img), (int)lv_obj_get_y(pb_frame_img),
         (int)lv_obj_get_width(pb_frame_img), (int)lv_obj_get_height(pb_frame_img),
         (int)lv_obj_get_index(pb_frame_img),
         (int)lv_obj_get_child_cnt(lv_scr_act()));
}

// Put the decoder where the picture on screen is, without moving anything.
// Costs a decode from the preceding keyframe, and only the first step after
// playing pays it.
// Put the decoder on the frame the pause landed on. Returns the picture too,
// for the pause itself to show: after that the frame on screen is this one,
// so every step from it is exact by construction. Reading the frame back
// from the output can never be, because the output is a pipeline: GetChnPts
// said one thing before PauseChn() and one or two frames more after it,
// every time, and the panel is a frame or so behind that again.
static bool pb_frame_locate(FrameStepPic_t *pic) {
    if (pb_frame_at) {
        return true;
    }
    if (!pb_stepper_ready()) {
        return false;
    }

    // Start from the picture that was on screen when playback paused. The
    // clock is the wrong answer (it runs a frame or two ahead of the picture)
    // and so is asking the output now (paused, it reports the last keyframe).
    const int shownNow = media_shown_ms(media);
    int from = pb_paused_at;

    if (from < 0) {
        from = shownNow >= 0 ? shownNow : controller.value;
    }

    if (!framestep_to(pb_stepper, from, pic)) {
        return false;
    }

    LOGI("step: placed at %dms (paused at %d, output says %d, clock %d)",
         pic->pts_ms, pb_paused_at, shownNow, controller.value);
    LOGI("AT pause placed=%d paused_at=%d output=%d clock=%d delta=%d",
         pic->pts_ms, pb_paused_at, shownNow, controller.value,
         pb_paused_at >= 0 ? pic->pts_ms - pb_paused_at : 0);
    pb_paused_at = -1;
    controller.value = pic->pts_ms;
    return true;
}

// Drawn when there is no decoder for the file, so that a step still puts
// something on the UI layer. Whether this shows says whether the drawing half
// works, which is otherwise impossible to tell apart from a decode that never
// produced a frame.
static bool pb_frame_test_pattern(FrameStepPic_t *pic) {
    static uint8_t *pattern = NULL;
    static int patW = 0, patH = 0;

    const int w = lv_disp_get_hor_res(NULL) - DISP_OVERSCAN;
    const int h = lv_disp_get_ver_res(NULL) - DISP_OVERSCAN;

    if (!pattern || patW != w || patH != h) {
        uint8_t *next = realloc(pattern, (size_t)w * h * 4);
        if (!next) {
            return false;
        }
        pattern = next;
        patW = w;
        patH = h;

        for (int y = 0; y < h; y++) {
            uint8_t *out = pattern + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) {
                *out++ = (uint8_t)(255 - (x * 255 / w)); // blue
                *out++ = (uint8_t)(y * 255 / h);         // green
                *out++ = (uint8_t)(x * 255 / w);         // red
                *out++ = 0xff;
            }
        }
    }

    pic->pixels = pattern;
    pic->width = w;
    pic->height = h;
    pic->pts_ms = controller.value;

    return true;
}

static void pb_frame_step(bool forward) {
    FrameStepPic_t pic;

    pb_gate_stop();
    pb_seek_cancel();

    if (controller.is_playing) {
        pb_pause();
    }

    if (!pb_frame_locate(&pic)) {
        LOGI("step: cannot place the decoder at %dms, drawing the test pattern",
             controller.value);
        if (pb_frame_test_pattern(&pic)) {
            pb_frame_show(&pic);
            // Not a decoded frame: the next press should try to place the
            // decoder again rather than step from here.
            pb_frame_at = false;
        }
        return;
    }

    const int from = controller.value;
    const uint32_t t0 = lv_tick_get();

    if (!(forward ? framestep_next(pb_stepper, &pic)
                  : framestep_prev(pb_stepper, &pic))) {
        LOGI("step: no frame %s of %dms", forward ? "after" : "before", controller.value);
        LOGI("AT step dir=%s from=%d to=%d frames=0 ms=%u result=end", forward ? "+" : "-", from, from, lv_tick_elaps(t0));
        return; // either end of the file; leave the picture alone
    }

    controller.value = pic.pts_ms;
    pb_frame_show(&pic);

    // The demuxer reports 0fps for the recorder's TS files, so frame_ms starts
    // at the 33ms default even for 60fps. The decoder's own step is the truth:
    // the smallest one seen is one frame.
    const int d = abs(pic.pts_ms - from);
    if (d >= 5 && d < (int)controller.frame_ms) {
        LOGI("step: frame interval is %dms, not %ums", d, controller.frame_ms);
        controller.frame_ms = (uint32_t)d;
    }

    // frames is in hundredths, so a step of exactly one frame reads 100 and a
    // 16.6ms frame does not get rounded away.
    const int frame_ms = controller.frame_ms ? (int)controller.frame_ms : 33;
    LOGI("AT step dir=%s from=%d to=%d frames=%d ms=%u result=%s", forward ? "+" : "-", from, pic.pts_ms,
         (pic.pts_ms - from) * 100 / frame_ms, lv_tick_elaps(t0),
         (forward ? pic.pts_ms > from : pic.pts_ms < from) ? "ok" : "wrongway");
}

static void pb_play(void) {
    pb_seek_cancel();

    const bool fromFrame = pb_frame_at;

    pb_frame_hide();
    pb_paused_at = -1;
    controller.is_playing = true;

    if (fromFrame) {
        // MPP is still paused where stepping started, so it has to be moved to
        // the frame that was on screen. See pb_catchup_ms: it is asked for a
        // point before the keyframe, the picture stays up until the output
        // has played its way to here, and notify_cb() takes it down.
        const int target = controller.value;
        const int key = pb_stepper ? framestep_key_before(pb_stepper, target) : -1;
        // Without a known keyframe, assume one within the last second.
        const int aimKey = (key >= 0) ? key : target - TUNE(GOP);
        const int ask = pb_land_known ? aimKey - TUNE(MARGIN) - pb_land_bias : aimKey - TUNE(LEAD);

        pb_frame_at = true; // pb_frame_hide() cleared it; the picture is still up
        if (pb_frame_img) {
            lv_obj_clear_flag(pb_frame_img, LV_OBJ_FLAG_HIDDEN);
        }
        pb_catchup_began = lv_tick_get();
        pb_catchup_key = key;
        pb_catchup_landing = -1;
        pb_catchup_first = -1;
        pb_catchup_drop_at = -1;
        LOGI("step: resuming at %dms, keyframe %d, landing bias %s%d", target, key,
             pb_land_known ? "" : "unknown, default ", pb_land_known ? pb_land_bias : -TUNE(LEAD));
        pb_catchup_seek(ask);
        // Armed last: the watch runs on the media thread without the lock,
        // and must not see a target before the seek's bookkeeping is in.
        pb_catchup_ms = target;
        return;
    }

    if (pb_speed == PB_SPEED_1_1) {
        pb_gate_stop();
        media_start();
    } else {
        pb_gate_start();
    }
}

static void pb_pause(void) {
    FrameStepPic_t pic;

    pb_gate_stop();
    controller.is_playing = false;
    media_pause();
    pb_catchup_end(); // paused mid-resume: the picture stays, the sound comes back

    // Where the output settled, read after the pause. PauseChn() is not
    // instant: the render thread puts up one or two more frames after it is
    // called, and the panel ends on those. Measured as +1 to +2 frames over
    // the value read just before, every time; using the earlier value put the
    // decoded picture a frame or two behind what the eye had just seen.
    usleep(40000);
    pb_paused_at = media_shown_ms(media);

    // Replace the output's picture with the decoder's for the same moment,
    // so what is on screen and where the steps start from are one thing.
    if (pb_frame_locate(&pic)) {
        pb_frame_show(&pic);
    }
}

static void pb_step_forward(void) {
    pb_frame_step(true);
}

static void pb_seek_now(void) {
    pb_seek_timer = NULL;
    pb_seek_pending = false;

    if (controller.is_playing) {
        pb_frame_hide();
        media_seek(controller.value); // keeps playing, gate and all
        return;
    }

    // Paused: show the frame actually asked for rather than the keyframe the
    // demuxer would land on.
    FrameStepPic_t pic;

    if (pb_stepper_ready() && framestep_to(pb_stepper, controller.value, &pic)) {
        controller.value = pic.pts_ms;
        pb_frame_show(&pic);
        return;
    }

    // No decoder for this file. Move the paused pipeline instead, which gets
    // as close as it can: a keyframe.
    pb_frame_hide();
    media_seek(controller.value);
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

static void pb_step_back(void) {
    pb_frame_step(false);
}

uint8_t mplayer_on_key(uint8_t key) {
    if (!controller.enable)
        return 0;

    LOGI("mplayer_on_key: %d", key);

    if (pb_bare && key != DIAL_KEY_PRESS)
        return 0;

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
// What the watch found the output doing during a resume.
typedef enum {
    CATCHUP_WAITING, // nothing to do yet
    CATCHUP_REACHED, // the output is at the target: picture down
    CATCHUP_MISSED,  // the output is past it, or never showed anything new
} catchup_verdict_t;

// Where the seek landed, from the clock. MPP starts the clock when the first
// picture is decoded (notifyStartToRun, same millisecond as KeyFrameDecoded),
// at the time of the first sound it has, which is where the demuxer landed:
// the log's "AO set config start_time" agreed with it to a few ms. So the
// landing is the clock less the time since the first picture. (An earlier
// version counted from the seek and assumed the sound started 215ms after
// it; that was off by up to a second and taught the aim the wrong lesson.)
// Returns false while the clock is not running yet.
static bool pb_catchup_landed(void) {
    const int clock = media_clock_ms(media);

    if (clock < 0) {
        return false;
    }

    const int landing = clock - (int)lv_tick_elaps(pb_catchup_first_at);
    const int bias = landing - pb_catchup_ask;

    LOGI("step: first new picture %dms, clock %d, landed about %dms (asked %d, bias %+d, worst so far %s%+d)",
         pb_catchup_first, clock, landing, pb_catchup_ask, bias, pb_land_known ? "" : "none ", pb_land_bias);
    pb_catchup_landing = landing;
    if (!pb_land_known || bias > pb_land_bias) {
        pb_land_bias = bias;
        pb_land_known = true;
    }
    return true;
}

// Watches the output during a resume. Runs on the media thread *without*
// lvgl_mutex, polling every few ms: the notify callback's own 100ms period,
// plus the 100..270ms a redraw under the picture takes, let the output run
// half a second between two looks and sail past the target (seen at 42583ms
// for a target of 42399ms). Without the lock, the dial keeps working; a
// pause or exit ends the resume (pb_catchup_ms goes to -1) and this returns.
static catchup_verdict_t pb_catchup_watch(void) {
    const int target = pb_catchup_ms;
    const int drop = target - TUNE(DROP_LEAD);
    const uint32_t started = lv_tick_get();
    static uint32_t lastLook = 0;
    static int lastTarget = -1;

    // The gap since the last look is normally the 100ms between callbacks
    // plus a redraw. Anything longer means the thread was held up somewhere.
    // Only within one resume: across two it is just the time between them.
    if (lastTarget == target && lastLook && lv_tick_elaps(lastLook) > 400) {
        LOGW("step: watch away for %ums", lv_tick_elaps(lastLook));
    }
    lastTarget = target;

    while (pb_catchup_ms == target && controller.is_playing && lv_tick_elaps(started) < TUNE(WATCH)) {
        const uint32_t before = lv_tick_get();
        const int shown = media_shown_ms(media);

        if (lv_tick_elaps(before) > 100) {
            LOGW("step: reading the output took %ums", lv_tick_elaps(before));
        }

        if (!pb_catchup_fresh) {
            // Until the output has put up something new it reports the
            // paused frame, or a frame or two past it (PauseChn() lets a
            // couple through). A new picture from the seek is a keyframe at
            // or before the target, so it is below that value. (A band
            // either side was fooled when the target sat less than the band
            // after its keyframe: the new picture was taken for the old one.)
            if (shown < 0 || (shown >= pb_catchup_stale && shown <= pb_catchup_stale + TUNE(BAND))) {
                if (lv_tick_elaps(pb_catchup_since) >= TUNE(SILENT)) {
                    LOGW("step: no new picture %ums after the seek (output says %d)", lv_tick_elaps(pb_catchup_since), shown);
                    lastLook = lv_tick_get();
                    return CATCHUP_MISSED;
                }
                usleep(4000);
                continue;
            }
            pb_catchup_fresh = true;
            pb_catchup_first = shown;
            pb_catchup_first_at = lv_tick_get();
            pb_catchup_landing = -1;
        }

        if (pb_catchup_landing < 0 && !pb_catchup_landed()) {
            if (lv_tick_elaps(pb_catchup_first_at) >= (uint32_t)TUNE(STALL)) {
                LOGW("step: clock not running %ums after the first picture (%dms); the sound did not start",
                     lv_tick_elaps(pb_catchup_first_at), shown);
                lastLook = lv_tick_get();
                return CATCHUP_MISSED;
            }
            usleep(4000);
            continue;
        }

        if (shown > target + TUNE(DROP_LEAD)) {
            LOGW("step: output at %dms is past %dms", shown, target);
            pb_catchup_drop_at = shown;
            lastLook = lv_tick_get();
            return CATCHUP_MISSED;
        }
        if (shown >= drop) {
            LOGI("step: output at %dms has caught up to %dms (%d seek%s, %ums after play), picture down",
                 shown, target, pb_catchup_tries, pb_catchup_tries == 1 ? "" : "s", lv_tick_elaps(pb_catchup_began));
            pb_catchup_drop_at = shown;
            lastLook = lv_tick_get();
            return CATCHUP_REACHED;
        }
        usleep(4000);
    }

    lastLook = lv_tick_get();
    return CATCHUP_WAITING;
}

// One line per finished resume, in a fixed key=value form for
// utilities/sdcard-test/autotest-report.py. drop_delta is where the output
// was when the picture came down minus where the picture was: the jump, in
// ms, that the eye would have seen.
static void pb_catchup_report(const char *result) {
    LOGI("AT resume target=%d key=%d ask=%d landed=%d first=%d seeks=%d hold_ms=%u drop_at=%d drop_delta=%d result=%s",
         pb_catchup_ms, pb_catchup_key, pb_catchup_ask, pb_catchup_landing, pb_catchup_first, pb_catchup_tries,
         lv_tick_elaps(pb_catchup_began), pb_catchup_drop_at,
         pb_catchup_drop_at >= 0 ? pb_catchup_drop_at - pb_catchup_ms : 0, result);
}

// Acts on the watch's verdict, under lvgl_mutex.
static void pb_catchup_act(catchup_verdict_t verdict) {
    if (pb_catchup_ms < 0 || !controller.is_playing) {
        return; // ended by a key while the watch was looking
    }

    switch (verdict) {
    case CATCHUP_WAITING:
        break;
    case CATCHUP_REACHED:
        pb_catchup_report("ok");
        pb_frame_hide();
        break;
    case CATCHUP_MISSED:
        // Ask further back; the picture would jump if it came down now. A
        // landing past the target has already raised the worst landing seen
        // (pb_catchup_landed()), so the next resume aims earlier by itself.
        if (pb_catchup_tries <= TUNE(RETRIES) && pb_catchup_ask > 0) {
            pb_catchup_seek(pb_catchup_ask - TUNE(RETRY));
        } else {
            LOGW("step: giving up on %dms, picture down", pb_catchup_ms);
            pb_catchup_report("giveup");
            pb_frame_hide();
        }
        break;
    }
}

static void notify_cb(media_info_t *info) {
    // LOGI("%d/%d",info->playing_time,info->duration);

    if (!media)
        return;

    // A resume in progress comes first: while the render is waiting for the
    // clock the player reports no time at all, and the checks below would
    // return before the watch got a look (the three-second hold on a resume
    // whose sound did not restart went unnoticed that way).
    if (pb_catchup_ms >= 0 && controller.is_playing) {
        const catchup_verdict_t verdict = pb_catchup_watch();
        const int target = pb_catchup_ms;

        pthread_mutex_lock(&lvgl_mutex);
        pb_catchup_act(verdict);
        const bool dropped = pb_catchup_ms < 0;
        const uint32_t t0 = lv_tick_get();
        lv_timer_handler();
        pthread_mutex_unlock(&lvgl_mutex);

        // The picture comes off the panel when this redraw has been flushed,
        // not when it was hidden. Where the output is by then is what the eye
        // sees next to the last decoded frame: 0 is seamless, negative a step
        // back, positive a skip forward.
        if (dropped) {
            const int after = media_shown_ms(media);
            LOGI("AT drop target=%d redraw_ms=%u output_after=%d delta=%d", target, lv_tick_elaps(t0), after,
                 after >= 0 ? after - target : 0);
        }
        return;
    }

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
    if (pb_seek_pending || pb_frame_at) {
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

    snprintf(pb_path, sizeof(pb_path), "%s", fname);
    pb_land_known = false;
    pb_tune_load();
    pb_stepper_failed = false;
    pb_frame_at = false;

    controller.frame_ms = media_frame_interval_ms(media);
    LOGI("frame is %ums", controller.frame_ms);
    pb_play();
    update_mplayer();
}

void mplayer_exit() {
    pb_gate_stop();
    pb_seek_cancel();
    pb_catchup_end();
    pthread_mutex_unlock(&lvgl_mutex);
    if (media) {
        media_exit(media);
        media = NULL;
    }
    pthread_mutex_lock(&lvgl_mutex);

    // The image points into the decoder's buffer, so it goes first.
    if (pb_frame_img) {
        lv_img_cache_invalidate_src(&pb_frame_dsc);
        lv_obj_del(pb_frame_img);
        pb_frame_img = NULL;
    }
    pb_frame_at = false;
    framestep_close(pb_stepper);
    pb_stepper = NULL;
    pb_path[0] = '\0';
    pb_bare = false;

    free_mplayer();
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
    return controller.enable && pb_catchup_ms >= 0;
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

void mplayer_set_speed(int index) {
    if (index >= 0 && index < PB_SPEED_COUNT) {
        pb_speed = (uint8_t)index;
        if (controller.enable) {
            if (controller.is_playing) {
                pb_play();
            }
            update_mplayer();
        }
    }
}
