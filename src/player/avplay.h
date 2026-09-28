#pragma once

// Playing a recording through decoders of our own rather than MPP's player.
//
// The picture comes from the stepper's video decoder (framestep.h) and goes
// to a video output channel that is never bound, one frame at a time with
// AW_MPI_VO_SendFrame; the sound comes from asend.h. The picture is paced by
// the sound's clock, so the two stay together by construction.
//
// MPP's player could only be moved by seeking, and its seek cannot land on a
// frame: resuming after a pause or a step meant seeking short, playing on
// under a decoded picture and taking the picture down when the output caught
// up, which held the screen two seconds on average. Here the decoder that
// stepped is the one that plays, so play carries on from the frame on screen,
// and pause, step and seek are all the same frame-exact operation.
//
// Measured before this was written, and the reasons behind each choice, are
// in vosend.c and asend.c.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AvPlay AvPlay_t;

typedef enum {
    AVPLAY_PAUSED = 0,
    AVPLAY_PLAYING,
    AVPLAY_ENDED, // played to the last frame, which stays on screen
} avplay_state_t;

// Opens the file and puts its first frame on screen, paused. NULL when the
// file cannot be played. Sets up and tears down MPP itself: nothing else may
// be using it in between.
AvPlay_t *avplay_open(const char *path);
void avplay_close(AvPlay_t *ap);

int avplay_duration_ms(AvPlay_t *ap);
int avplay_frame_ms(AvPlay_t *ap);    // one frame, rounded down
int avplay_position_ms(AvPlay_t *ap); // the frame on screen
int avplay_state(AvPlay_t *ap);       // avplay_state_t
bool avplay_has_audio(AvPlay_t *ap);
// Play has been asked for and the picture has not moved yet.
bool avplay_resuming(AvPlay_t *ap);

// Plays from the frame on screen, or from the start once ended. divider 1 is
// normal speed with the sound; 2, 4 and 8 are slower and silent.
void avplay_play(AvPlay_t *ap, int divider);
void avplay_pause(AvPlay_t *ap);
// One frame forward or back; pauses first. False at either end.
bool avplay_step(AvPlay_t *ap, bool forward);
// Shows the first frame at or after this position, and keeps playing if it
// was.
bool avplay_seek(AvPlay_t *ap, int ms);

#ifdef __cplusplus
}
#endif
