#pragma once

// A recording's sound without MPP's player: packets read here with
// libavformat, decoded on an audio decoder channel that is never bound, and
// the decoded frames handed to an audio output channel with
// AW_MPI_AO_SendFrame. The audio half of the experiment in vosend.h, and the
// clock the picture is paced by when both run.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ASend ASend_t;

// Where the sound is taken to be.
typedef enum {
    // The first frame's pts from the moment the output took it, running on
    // the monotonic clock: blind to however long the output takes to sound.
    ASEND_CLOCK_START = 0,
    // What has gone to ALSA less what ALSA says it still holds (delay in
    // /proc/asound/.../status).
    ASEND_CLOCK_PROC,
} asend_clock_t;

// Opens the file, the decoder and the output, and starts feeding them from a
// thread of its own. start_ms: the first frame played is the first at or
// after it. aac_raw: strip ADTS headers and have the decoder add its own
// (chosen anyway for a file whose frames have none, such as an mp4).
// NULL when there is no AAC to play.
ASend_t *asend_open(const char *path, int start_ms, bool aac_raw);
// The same, opened and left paused.
ASend_t *asend_open_paused(const char *path, bool aac_raw);
void asend_close(ASend_t *as);

// Stops the sound where it is. Whatever the output and ALSA already hold is
// flushed by the next asend_play_from(), not here.
void asend_pause(ASend_t *as);
// Plays from the first frame at or after this position. The clock
// (asend_audible()) has no answer until the output has taken that frame.
bool asend_play_from(ASend_t *as, int ms);

// The pts audible at now_us (monotonic), by the method asked for. False
// until there is one: before the output has taken a frame, or for
// ASEND_CLOCK_PROC while ALSA does not report the stream running.
bool asend_audible(ASend_t *as, int method, int64_t now_us, int64_t *pts_us);

// The file ran out before the output took a frame from where play began:
// there is no sound to wait for (a recording shorter than the decoder's
// start, or play from its last moments).
bool asend_exhausted(ASend_t *as);

// One "VS audio" line: what has gone through, and both clocks side by side.
void asend_log(ASend_t *as, int64_t now_us);

#ifdef __cplusplus
}
#endif
