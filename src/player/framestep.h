#pragma once

// Stepping a paused recording one frame at a time, decoded here in software
// rather than by the SoC's player.
//
// Everything the MPP pipeline offers was measured first and none of it can
// express one frame. AW_MPI_VO_StepChn is commented out of mpi_vo.h and is in
// none of the shipped libraries; AW_MPI_SYS_UnBind returns success without
// leaving tunnel mode, so taking the decoder off the output and handing frames
// over with GetImage/SendFrame answers NOT_PERM and leaves the layer empty;
// pausing the output and letting it run for one frame's worth of time costs
// about a frame in the pause and resume calls alone, so every step moved two;
// and AW_MPI_DEMUX_Seek only lands on keyframes, which the recorder writes one
// of a second.
//
// What it does instead is decode the wanted frame on a VDEC channel of its
// own, created here and never bound to anything. Binding is what turns tunnel
// mode on, so an unbound channel is the supported way to hand stream in and
// take pictures out, and the player's own channel is left alone and paused.
//
// The demuxing is the ffmpeg already on the goggle. That build is
// --disable-everything with only demuxers, parsers and muxers, so it can read
// these recordings but decode nothing, which is exactly the half needed here.
// It has no hevc_mp4toannexb either, so length-prefixed samples (mp4) are
// refused; the recorder writes transport streams, which are already
// start-coded.
//
// A step is then exact in both directions: seek to the keyframe at or before
// the target and decode forward to the frame actually asked for. The player
// stays paused underneath with its last frame on the video layer, and the
// decoded picture is drawn over it on the UI layer, the way the OSD is.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FrameStep FrameStep_t;

// A decoded picture, four bytes a pixel in the order lv_color32_t has them
// (blue, green, red, alpha). Owned by the stepper and replaced by the next
// call that returns one.
typedef struct
{
    const uint8_t *pixels;
    int width;
    int height;
    int pts_ms;
} FrameStepPic_t;

FrameStep_t *framestep_open(const char *path);
void framestep_close(FrameStep_t *fs);

// Scale decoded frames to this size on the way out. Nearest neighbour, done in
// the same pass as the colour conversion, which is a great deal cheaper than
// handing LVGL a native-sized image and asking it to transform it. Off by
// default, and the buffer is only resized when this changes.
bool framestep_set_output_size(FrameStep_t *fs, int width, int height);

// What the container says the frame rate is, x1000, or 0 when it says nothing.
int framestep_frame_rate(FrameStep_t *fs);

// The keyframe at or before this position, as seen going past the decoder,
// or -1 when none has. What a resume has to start the player from: its seek
// lands past where it is asked and the hardware then skips to the next
// keyframe, so it has to be asked for a point safely before this one.
int framestep_key_before(FrameStep_t *fs, int ms);

// The first frame at or after this position.
bool framestep_to(FrameStep_t *fs, int ms, FrameStepPic_t *pic);

// The frame after, or before, the one last returned. Stepping forward normally
// costs one frame's decode, because the decoder is left where the last frame
// came from. Stepping back has to decode again from the preceding keyframe.
bool framestep_next(FrameStep_t *fs, FrameStepPic_t *pic);
bool framestep_prev(FrameStep_t *fs, FrameStepPic_t *pic);

#ifdef __cplusplus
}
#endif
