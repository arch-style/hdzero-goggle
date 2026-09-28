#pragma once

// The player's video decoder: a recording read with libavformat and decoded
// on a VDEC channel of its own, one frame at a time, frame-exact in both
// directions. avplay.c plays with it, and pause, step and seek are the same
// operation on it.
//
// It began as the stepper for MPP's player, because nothing in the MPP
// pipeline can express one frame. AW_MPI_VO_StepChn is commented out of
// mpi_vo.h and is in none of the shipped libraries; AW_MPI_SYS_UnBind
// returns success without leaving tunnel mode; pausing the output for one
// frame's worth of time moved two; and AW_MPI_DEMUX_Seek only lands on
// keyframes, which the recorder writes one of a second.
//
// The channel is never bound to anything. Binding is what turns tunnel mode
// on, and in tunnel mode the vendor library refuses AW_MPI_VDEC_GetImage, so
// an unbound channel is the supported way to hand stream in and take
// pictures out.
//
// The demuxing is the ffmpeg already on the goggle. That build is
// --disable-everything with only demuxers, parsers and muxers, so it can read
// these recordings but decode nothing, which is exactly the half needed here.
// It has no hevc_mp4toannexb either, so length-prefixed samples (mp4) are
// rewritten into start codes here.
//
// A frame is exact whichever way it is reached: seek to the keyframe at or
// before the target and decode forward to the frame actually asked for.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FrameStep FrameStep_t;

// extra_frames: frame buffers beyond what the stream needs, for frames handed
// on to the video output and held there.
FrameStep_t *framestep_open(const char *path, int extra_frames);
void framestep_close(FrameStep_t *fs);

// What the container says the frame rate is, x1000, or 0 when it says nothing.
int framestep_frame_rate(FrameStep_t *fs);
// The file's length, or 0 when the container does not say.
int framestep_duration_ms(FrameStep_t *fs);

// The decoder's own frames, for handing to the video output as they are. A
// frame stays out of the decoder's pool until it is given back with
// framestep_put_frame(), which is safe from another thread, such as the
// output's release callback.
struct VIDEO_FRAME_INFO_S;
// The next frame. Costs one frame's decode: the decoder is left where the
// last frame came from.
bool framestep_get_frame(FrameStep_t *fs, struct VIDEO_FRAME_INFO_S *frame);
// The first frame at or after this position.
bool framestep_get_frame_at(FrameStep_t *fs, int ms, struct VIDEO_FRAME_INFO_S *frame);
// The last frame before this position, which is the one on screen: a step
// back. Decodes again from the preceding keyframe. The frame on screen itself
// is kept, to come out next.
bool framestep_get_prev_frame(FrameStep_t *fs, int before_ms, struct VIDEO_FRAME_INFO_S *frame);
// Hands a frame taken with framestep_get_frame() back, unshown, to be the
// next one out.
void framestep_unget_frame(FrameStep_t *fs, struct VIDEO_FRAME_INFO_S *frame);
void framestep_put_frame(FrameStep_t *fs, struct VIDEO_FRAME_INFO_S *frame);

#ifdef __cplusplus
}
#endif
