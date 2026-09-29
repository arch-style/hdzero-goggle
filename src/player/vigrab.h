#pragma once

// One frame of the DVR input, saved raw: a screenshot of what the goggle
// shows after the FPGA.
//
// The SoC's own output (the framebuffer, the video layer) is not what reaches
// the panel: the FPGA mixes it with the live picture first, and the
// recordings carry the goggle's OSD, so the FPGA sends that mix back to the
// SoC on the camera input the recorder reads. Grabbing a frame there shows
// what the FPGA made of the SoC's picture, pixel for pixel, where a photo of
// the screen only shows it roughly.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opens the camera input at width x height and fps (it stays open for the
// next grab: see vigrab_close()), drops `skip` frames, and logs the next
// one's count of pixels at 255 under `name`; with a path, also writes it
// there as NV21 (the Y plane, then interleaved V/U at half size), tightly
// packed. MPP must be up (a file open in the player). False, with the reason
// in the log, when there is no frame.
bool vigrab_save(const char *name, const char *path, int width, int height, int fps, int skip);
// Before MPP goes down.
void vigrab_close(void);

#ifdef __cplusplus
}
#endif
