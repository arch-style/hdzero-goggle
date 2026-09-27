// Scripted playback tests, run from the SD card.
//
// Testing the frame stepper and the resume by hand cost one round of the card
// per scenario: write the build, boot, press the dial a dozen times, pull the
// card, read the log. A file on the card turns that into one round per build:
// the script below runs the dial's own code paths (mplayer_on_key(), like
// pb_key() does for the dial) and the player writes what it measured as "AT"
// lines. Nothing here is compiled out of release builds: with no script on the
// card the thread checks one path and exits.
//
// Script: one command per line, '#' comments, blank lines ignored.
//
//   open <file>          play a file; a bare name is under DCIM/100HDZRO/
//   wait <ms>            sleep
//   pause | play         click the dial only if it would change the state
//   toggle               click the dial regardless
//   step +<n> | step -<n>  n dial steps forward/back, stepgap ms apart
//   stepgap <ms>         gap between the steps of one "step" line (400)
//   size frame|1s|5s|10s|30s   what one dial click moves
//   speed 1|2|4|8        play speed 1/n
//   up <n> | down <n> | click | press | right | rightpress   raw keys
//   waitresume [ms]      wait until a resume has finished (default 10000)
//   set <name> <value>   a player tunable (see pb_tune[] in ui_player.c)
//   loop <n> ... end     repeat the block; nests
//   echo <text>          a marker line in the log
//   vosend <file> <ms> [paced|free|gate] [start_ms] [extra] [bufs]
//                        play without MPP's player: the stepper's decoder
//                        into an unbound video output (src/player/vosend.h),
//                        on the player's screen, then leave it; "VS" lines
//   exit                 leave the player
//   halt                 stop the script here
//
// The file is renamed to autotest.ran.txt as it starts, so a crash mid-script
// does not run it again at the next boot.

#include "autotest.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <log/log.h>
#include <lvgl/lvgl.h>

#include "core/app_state.h"
#include "core/common.hh"
#include "driver/beep.h"
#include "player/vosend.h"
#include "ui/ui_main_menu.h"
#include "ui/ui_player.h"

#define AUTOTEST_FILE "/mnt/extsd/autotest.txt"
#define AUTOTEST_RAN "/mnt/extsd/autotest.ran.txt"
#define AUTOTEST_MEDIA_DIR "/mnt/extsd/DCIM/100HDZRO/"
// Time for the menu to settle after boot before the first command.
#define AUTOTEST_SETTLE_MS 5000
#define AUTOTEST_MAX_LINES 2000
#define AUTOTEST_MAX_NEST 8

static char *at_lines[AUTOTEST_MAX_LINES];
static int at_count = 0;
static int at_stepgap_ms = 400;

static void at_log_cmd(int line, const char *text) {
    LOGI("AT cmd line=%d %s", line + 1, text);
}

static void at_sleep_ms(int ms) {
    while (ms > 0) {
        int chunk = ms > 200 ? 200 : ms;
        usleep((useconds_t)chunk * 1000);
        ms -= chunk;
    }
}

// One dial or button key through the same door the input thread uses.
static void at_key(uint8_t key) {
    pthread_mutex_lock(&lvgl_mutex);
    if (mplayer_is_open()) {
        if (mplayer_on_key(key)) {
            // Left the player: put the menu back the way pb_key() would.
            app_state_push(APP_STATE_MAINMENU);
            main_menu_show(true);
        }
    } else {
        LOGW("AT key %d with no player open", key);
    }
    pthread_mutex_unlock(&lvgl_mutex);
}

static bool at_path(const char *name, char *path, size_t size) {
    if (strchr(name, '/')) {
        snprintf(path, size, "%s", name);
    } else {
        snprintf(path, size, AUTOTEST_MEDIA_DIR "%s", name);
    }
    if (access(path, R_OK) != 0) {
        LOGW("AT open: no such file %s", path);
        return false;
    }
    return true;
}

// The player's screen, playing the file, or with nothing under it when bare.
static void at_open_screen(const char *path, bool bare) {
    pthread_mutex_lock(&lvgl_mutex);
    if (mplayer_is_open()) {
        // Close what is playing first.
        mplayer_on_key(DIAL_KEY_PRESS);
        app_state_push(APP_STATE_MAINMENU);
    }
    if (g_app_state == APP_STATE_VIDEO) {
        // Booted into the picture: the dial's long press would bring the menu.
        app_switch_to_menu();
    }
    pthread_mutex_unlock(&lvgl_mutex);
    at_sleep_ms(500);

    pthread_mutex_lock(&lvgl_mutex);
    // With Menu Over Video the display still belongs to the live source;
    // page_playback does the same on entry.
    app_menu_end_overlay();
    if (main_menu_is_shown()) {
        main_menu_show(false);
    }
    if (bare) {
        mplayer_autotest_open_bare();
    } else {
        mplayer_autotest_open(path);
    }
    pthread_mutex_unlock(&lvgl_mutex);
}

static void at_open(const char *name) {
    char path[300];

    if (at_path(name, path, sizeof(path))) {
        at_open_screen(path, false);
    }
}

static void at_vosend(const char *line) {
    char name[200] = "", mode[16] = "paced";
    char path[300];
    vosend_opts_t opts = {.run_ms = 5000, .mode = VOSEND_PACED, .disp_bufs = 2};

    sscanf(line, "%*s %199s %d %15s %d %d %d", name, &opts.run_ms, mode, &opts.start_ms,
           &opts.extra_frames, &opts.disp_bufs);
    opts.mode = vosend_mode_parse(mode);
    if (opts.mode < 0) {
        LOGW("AT vosend: unknown mode '%s'", mode);
        return;
    }
    if (!at_path(name, path, sizeof(path))) {
        return;
    }

    at_open_screen(path, true);
    at_sleep_ms(300);
    // Not under the LVGL lock: it runs for seconds, and the screen has to
    // keep drawing over the video layer as it would in playback.
    vosend_run(path, &opts);
    at_key(DIAL_KEY_PRESS);
    at_sleep_ms(300);
}

static void at_wait_resume(int timeout_ms) {
    const uint32_t t0 = lv_tick_get();

    while (lv_tick_elaps(t0) < (uint32_t)timeout_ms) {
        pthread_mutex_lock(&lvgl_mutex);
        bool resuming = mplayer_is_resuming();
        pthread_mutex_unlock(&lvgl_mutex);
        if (!resuming) {
            LOGI("AT waitresume done_ms=%u", lv_tick_elaps(t0));
            return;
        }
        usleep(20000);
    }
    LOGW("AT waitresume timeout_ms=%d", timeout_ms);
}

static int at_size_index(const char *s) {
    static const char *names[] = {"frame", "1s", "5s", "10s", "30s"};
    for (int i = 0; i < 5; i++) {
        if (strcmp(s, names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static int at_speed_index(const char *s) {
    switch (atoi(s)) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    case 8: return 3;
    default: return -1;
    }
}

static void at_under_lock(void (*fn)(int), int arg) {
    pthread_mutex_lock(&lvgl_mutex);
    fn(arg);
    pthread_mutex_unlock(&lvgl_mutex);
}

// Runs one command. Returns false to stop the script.
static bool at_run(int i, char *line) {
    char op[32] = "", a1[200] = "", a2[64] = "";
    int n = sscanf(line, "%31s %199s %63s", op, a1, a2);

    if (n < 1) {
        return true;
    }
    at_log_cmd(i, line);

    if (strcmp(op, "open") == 0) {
        at_open(a1);
    } else if (strcmp(op, "wait") == 0) {
        at_sleep_ms(atoi(a1));
    } else if (strcmp(op, "pause") == 0 || strcmp(op, "play") == 0) {
        pthread_mutex_lock(&lvgl_mutex);
        bool playing = mplayer_is_playing();
        pthread_mutex_unlock(&lvgl_mutex);
        if (playing == (op[1] == 'a')) { // "pause" while playing, "play" while paused
            at_key(DIAL_KEY_CLICK);
        }
    } else if (strcmp(op, "toggle") == 0 || strcmp(op, "click") == 0) {
        at_key(DIAL_KEY_CLICK);
    } else if (strcmp(op, "step") == 0) {
        int count = atoi(a1[0] == '+' ? a1 + 1 : a1);
        uint8_t key = (count < 0) ? DIAL_KEY_DOWN : DIAL_KEY_UP;
        if (count < 0) {
            count = -count;
        }
        for (int k = 0; k < count; k++) {
            at_key(key);
            if (k + 1 < count) {
                at_sleep_ms(at_stepgap_ms);
            }
        }
    } else if (strcmp(op, "stepgap") == 0) {
        at_stepgap_ms = atoi(a1);
    } else if (strcmp(op, "size") == 0) {
        int idx = at_size_index(a1);
        if (idx >= 0) {
            at_under_lock(mplayer_set_step_size, idx);
        } else {
            LOGW("AT size: unknown '%s'", a1);
        }
    } else if (strcmp(op, "speed") == 0) {
        int idx = at_speed_index(a1);
        if (idx >= 0) {
            at_under_lock(mplayer_set_speed, idx);
        } else {
            LOGW("AT speed: unknown '%s'", a1);
        }
    } else if (strcmp(op, "up") == 0 || strcmp(op, "down") == 0) {
        int count = a1[0] ? atoi(a1) : 1;
        for (int k = 0; k < count; k++) {
            at_key(op[0] == 'u' ? DIAL_KEY_UP : DIAL_KEY_DOWN);
            if (k + 1 < count) {
                at_sleep_ms(at_stepgap_ms);
            }
        }
    } else if (strcmp(op, "press") == 0 || strcmp(op, "exit") == 0) {
        at_key(DIAL_KEY_PRESS);
    } else if (strcmp(op, "right") == 0) {
        at_key(RIGHT_KEY_CLICK);
    } else if (strcmp(op, "rightpress") == 0) {
        at_key(RIGHT_KEY_PRESS);
    } else if (strcmp(op, "waitresume") == 0) {
        at_wait_resume(a1[0] ? atoi(a1) : 10000);
    } else if (strcmp(op, "set") == 0) {
        pthread_mutex_lock(&lvgl_mutex);
        mplayer_tune_set(a1, atoi(a2));
        pthread_mutex_unlock(&lvgl_mutex);
    } else if (strcmp(op, "vosend") == 0) {
        at_vosend(line);
    } else if (strcmp(op, "echo") == 0) {
        // already logged by at_log_cmd
    } else if (strcmp(op, "halt") == 0) {
        return false;
    } else if (strcmp(op, "loop") != 0 && strcmp(op, "end") != 0) {
        LOGW("AT unknown command '%s'", op);
    }
    return true;
}

static void *at_thread(void *arg) {
    (void)arg;
    at_sleep_ms(AUTOTEST_SETTLE_MS);

    FILE *fp = fopen(AUTOTEST_FILE, "r");
    if (!fp) {
        return NULL;
    }
    if (rename(AUTOTEST_FILE, AUTOTEST_RAN) != 0) {
        LOGW("AT could not rename the script; it will run again next boot");
    }

    char buf[512];
    while (at_count < AUTOTEST_MAX_LINES && fgets(buf, sizeof(buf), fp)) {
        buf[strcspn(buf, "\r\n")] = 0;
        char *p = buf;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#' || *p == 0) {
            p = ""; // keeps the line numbers in the log matching the file
        }
        at_lines[at_count++] = strdup(p);
    }
    fclose(fp);
    LOGI("AT start lines=%d", at_count);

    // loop/end: a stack of (line of the loop command, iterations left).
    int loop_line[AUTOTEST_MAX_NEST], loop_left[AUTOTEST_MAX_NEST];
    int depth = 0;
    const uint32_t t0 = lv_tick_get();
    int executed = 0;

    for (int i = 0; i < at_count; i++) {
        char *line = at_lines[i];
        if (!*line) {
            continue;
        }
        if (strncmp(line, "loop", 4) == 0 && (line[4] == ' ' || line[4] == 0)) {
            if (depth < AUTOTEST_MAX_NEST) {
                loop_line[depth] = i;
                loop_left[depth] = line[4] ? atoi(line + 5) : 1;
                depth++;
            }
            at_log_cmd(i, line);
            continue;
        }
        if (strcmp(line, "end") == 0) {
            if (depth > 0 && --loop_left[depth - 1] > 0) {
                i = loop_line[depth - 1]; // back to the loop line; the for's i++ skips it
            } else if (depth > 0) {
                depth--;
            }
            continue;
        }
        executed++;
        if (!at_run(i, line)) {
            break;
        }
    }

    pthread_mutex_lock(&lvgl_mutex);
    bool open = mplayer_is_open();
    pthread_mutex_unlock(&lvgl_mutex);
    if (open) {
        at_key(DIAL_KEY_PRESS);
    }
    LOGI("AT done commands=%d elapsed_ms=%u", executed, lv_tick_elaps(t0));

    // Done, and audibly so: three short and one long, unlike any key beep,
    // so the card can be pulled without watching the screen.
    for (int k = 0; k < 3; k++) {
        beep_dur(BEEP_SHORT);
        at_sleep_ms(200);
    }
    beep_dur(600);
    return NULL;
}

void autotest_start(void) {
    if (access(AUTOTEST_FILE, R_OK) != 0) {
        return;
    }
    pthread_t pid;
    if (pthread_create(&pid, NULL, at_thread, NULL) == 0) {
        pthread_detach(pid);
        LOGI("AT script found, running after %dms", AUTOTEST_SETTLE_MS);
    }
}
