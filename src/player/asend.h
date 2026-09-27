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
    // What ALSA says it has played (hw_ptr in /proc/asound/.../status),
    // counted from the first frame written.
    ASEND_CLOCK_PROC,
} asend_clock_t;

// Opens the file, the decoder and the output, and starts feeding them from a
// thread of its own. start_ms: the first frame played is the first at or
// after it. aac_raw: strip ADTS headers and have the decoder add its own.
ASend_t *asend_open(const char *path, int start_ms, bool aac_raw);
void asend_close(ASend_t *as);

// The pts audible at now_us (monotonic), by the method asked for. False
// until there is one: before the output has taken a frame, or for
// ASEND_CLOCK_PROC while ALSA does not report the stream running.
bool asend_audible(ASend_t *as, int method, int64_t now_us, int64_t *pts_us);

// One "VS audio" line: what has gone through, and both clocks side by side.
void asend_log(ASend_t *as, int64_t now_us);

#ifdef __cplusplus
}
#endif
