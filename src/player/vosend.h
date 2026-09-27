#pragma once

// Playing a recording without MPP's player: frames from the stepper's own
// decoder channel handed to a video output channel that is never bound to
// anything, with AW_MPI_VO_SendFrame. The first experiment towards a single
// player that pauses, steps and resumes on the decoded frame; it answers
// whether the output shows frames given to it this way, how fast, and whether
// the decoder keeps up with frames held by the output. Everything it measures
// goes to the log as "VS" lines.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOSEND_PACED = 0, // each frame when its pts is due, as playback would
    VOSEND_FREE,      // each frame as soon as it is decoded: the ceiling
    VOSEND_GATE,      // paced, pts squeezed 4x to probe the output's frame gate
} vosend_mode_t;

typedef struct {
    int run_ms;       // how long to play for
    int start_ms;     // where in the file to start
    int mode;         // vosend_mode_t
    int extra_frames; // decoder frame buffers beyond what the stream needs;
                      // 0 was enough with the output holding 3
    // AW_MPI_VO_SetChnDispBufNum; the player uses 2. 0 shows the frame no
    // sooner and hands it back 0.2ms after it is sent, while it is on screen.
    int disp_bufs;
} vosend_opts_t;

const char *vosend_mode_name(int mode);
int vosend_mode_parse(const char *name); // -1 when unknown

// Blocks for opts->run_ms and a little more. Nothing else may be using MPP:
// this sets up and tears down the whole system around the run.
bool vosend_run(const char *path, const vosend_opts_t *opts);

#ifdef __cplusplus
}
#endif
