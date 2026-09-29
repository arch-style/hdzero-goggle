#include "vigrab.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <log/log.h>
#include <mm_comm_vi.h>
#include <mpi_sys.h>
#include <mpi_vi.h>

// In libmpp_isp.so; the recorder gets them through mpi_isp.h, whose
// declarations this toolchain's copy lacks.
extern AW_S32 AW_MPI_ISP_Run(int IspDev);
extern AW_S32 AW_MPI_ISP_Stop(int IspDev);

// The recorder's input (src/record/vi2venc.c: ViParams_t.devNum, ISP 0).
#define VIGRAB_DEV 0
#define VIGRAB_ISP 0
#define VIGRAB_TIMEOUT_MS 1000

// The input stays open between grabs: closing it takes three seconds.
static struct {
    bool vipp, enabled;
    VI_CHN chn;
    int width, height, fps;
} vg = {.chn = MM_INVALID_CHN};

static long vigrab_msSince(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (long)((t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000);
}

void vigrab_close(void) {
    struct timespec t0;

    if (!vg.vipp) {
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (vg.chn != MM_INVALID_CHN) {
        AW_MPI_VI_DisableVirChn(VIGRAB_DEV, vg.chn);
        AW_MPI_VI_DestoryVirChn(VIGRAB_DEV, vg.chn);
    }
    if (vg.enabled) {
        AW_MPI_VI_DisableVipp(VIGRAB_DEV);
    }
    AW_MPI_VI_DestoryVipp(VIGRAB_DEV);
    if (vg.enabled) {
        AW_MPI_ISP_Stop(VIGRAB_ISP);
    }
    memset(&vg, 0, sizeof(vg));
    vg.chn = MM_INVALID_CHN;
    LOGI("vigrab: closed in %ldms", vigrab_msSince(&t0));
}

static bool vigrab_open(int width, int height, int fps) {
    VI_ATTR_S attr;
    AW_S32 ret;

    if (vg.vipp && vg.width == width && vg.height == height && vg.fps == fps) {
        return true;
    }
    vigrab_close();

    ret = AW_MPI_VI_CreateVipp(VIGRAB_DEV);
    if (ret != SUCCESS) {
        LOGE("vigrab: CreateVipp %d: %x", VIGRAB_DEV, ret);
        return false;
    }
    vg.vipp = true;

    memset(&attr, 0, sizeof(attr));
    attr.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    attr.memtype = V4L2_MEMORY_MMAP;
    attr.format.pixelformat = V4L2_PIX_FMT_NV21M;
    attr.format.field = V4L2_FIELD_NONE;
    attr.format.width = width;
    attr.format.height = height;
    attr.nbufs = 3;
    attr.nplanes = 2;
    attr.fps = fps;
    if ((ret = AW_MPI_VI_SetVippAttr(VIGRAB_DEV, &attr)) != SUCCESS) {
        LOGE("vigrab: SetVippAttr %dx%d@%d: %x", width, height, fps, ret);
        goto failed;
    }
    if ((ret = AW_MPI_VI_EnableVipp(VIGRAB_DEV)) != SUCCESS) {
        LOGE("vigrab: EnableVipp: %x", ret);
        goto failed;
    }
    vg.enabled = true;
    AW_MPI_ISP_Run(VIGRAB_ISP);

    for (VI_CHN c = 0; c < VIU_MAX_CHN_NUM; c++) {
        if (AW_MPI_VI_CreateVirChn(VIGRAB_DEV, c, NULL) == SUCCESS) {
            vg.chn = c;
            break;
        }
    }
    if (vg.chn == MM_INVALID_CHN) {
        LOGE("vigrab: no virtual channel");
        goto failed;
    }
    if ((ret = AW_MPI_VI_EnableVirChn(VIGRAB_DEV, vg.chn)) != SUCCESS) {
        LOGE("vigrab: EnableVirChn %d: %x", vg.chn, ret);
        goto failed;
    }
    vg.width = width;
    vg.height = height;
    vg.fps = fps;
    return true;

failed:
    vigrab_close();
    return false;
}

// Pixels at 0 or 255: the FPGA's own layer showing through the SoC's picture
// where the SoC drew its OSD's transparent grey. The SoC's output is 16-235,
// and the live layer reads all ones at 1080p60 and black (0) at 720p60 with
// no receiver.
static unsigned vigrab_count(const unsigned char *y, size_t n, unsigned char value) {
    unsigned count = 0;
    for (size_t i = 0; i < n; i++) {
        count += y[i] == value;
    }
    return count;
}

static bool vigrab_write(const char *path, const VIDEO_FRAME_S *f) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        LOGE("vigrab: cannot write %s", path);
        return false;
    }
    // mStride holds each plane's size here (1920x1088 for a 1080-line
    // frame), not a line's: the lines are packed at the width.
    const size_t ysize = (size_t)f->mWidth * f->mHeight;
    bool ok = fwrite(f->mpVirAddr[0], 1, ysize, fp) == ysize &&
              fwrite(f->mpVirAddr[1], 1, ysize / 2, fp) == ysize / 2;
    if (fclose(fp) != 0) {
        ok = false;
    }
    if (!ok) {
        LOGE("vigrab: writing %s failed", path);
    }
    return ok;
}

bool vigrab_save(const char *name, const char *path, int width, int height, int fps, int skip) {
    VIDEO_FRAME_INFO_S frame;
    bool ok = true;
    AW_S32 ret;

    if (!vigrab_open(width, height, fps)) {
        return false;
    }
    // The first frames after the input starts, or after the picture
    // changed, can be from before.
    for (int i = 0; i <= skip; i++) {
        memset(&frame, 0, sizeof(frame));
        ret = AW_MPI_VI_GetFrame(VIGRAB_DEV, vg.chn, &frame, VIGRAB_TIMEOUT_MS);
        if (ret != SUCCESS) {
            LOGE("vigrab: no frame %d in %dms: %x", i, VIGRAB_TIMEOUT_MS, ret);
            return false;
        }
        if (i < skip) {
            AW_MPI_VI_ReleaseFrame(VIGRAB_DEV, vg.chn, &frame);
        }
    }
    const VIDEO_FRAME_S *f = &frame.VFrame;
    const bool whole = f->mpVirAddr[0] && f->mpVirAddr[1];
    const size_t n = (size_t)f->mWidth * f->mHeight;
    const unsigned at0 = whole ? vigrab_count(f->mpVirAddr[0], n, 0) : 0;
    const unsigned at255 = whole ? vigrab_count(f->mpVirAddr[0], n, 255) : 0;
    if (path && whole) {
        ok = vigrab_write(path, f);
    }
    LOGI("vigrab %s: %ux%u, %u through (%u at 0, %u at 255)%s", name, f->mWidth, f->mHeight,
         at0 + at255, at0, at255, path ? (ok ? ", saved" : ", not saved") : "");
    AW_MPI_VI_ReleaseFrame(VIGRAB_DEV, vg.chn, &frame);
    return whole && ok;
}
