#include "avplay.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <arm_neon.h>

#define LOG_TAG "avplay"
#include <log/log.h>

#include <hwdisplay.h>
#include <mpi_sys.h>
#include <mpi_vdec.h>
#include <mpi_vo.h>

#include "asend.h"
#include "framestep.h"

// Frame buffers beyond what the stream needs: the output holds up to three
// (vosend.c), and a step back decodes a GOP with the frame on screen still
// out.
#define AVPLAY_extraFRAMES 3
// What the player used; 0 hands a frame back while it is still on screen.
#define AVPLAY_dispBUFS 2

// The output is not told the frames' own timestamps. It refuses any frame
// less than its frame interval past the last one it took (25ms in the shipped
// library, 8ms as the card patches it), which would refuse every step back
// and, unpatched, every other frame of 60fps. With no clock bound it shows a
// frame when it gets it whatever the timestamp says, so it is given one that
// only ever goes up, by more than the interval.
#define AVPLAY_voSTEP_US 40000

// How long the picture waits for the sound to start before going on by the
// wall clock. The sound restarts in tens of ms (asend_play_from()).
#define AVPLAY_audioWAIT_MS 1500

#define AVPLAY_sendWAIT_MS 100
#define AVPLAY_stopWAIT_MS 500

#if defined HDZBOXPRO
#define AVPLAY_WIDTH  1280
#define AVPLAY_HEIGHT 720
#else
#define AVPLAY_WIDTH  1920
#define AVPLAY_HEIGHT 1080
#endif
#define AVPLAY_uiCHN 2

// Being tried (avplay_set_screen(), avplay_set_scan()).
static int avplay_screenW = AVPLAY_WIDTH;
static int avplay_screenH = AVPLAY_HEIGHT;
static int avplay_scanMode = 0;
static struct {
    int frames;
    int64_t us, maxUs;
    long hits;
} avplay_scanStat;

struct AvPlay {
    FrameStep_t *fs;
    ASend_t *as; // NULL when the file has no sound

    VO_DEV voDev;
    VO_LAYER voLayer;
    VO_CHN voChn;
    int uiLayer;
    // The frame avplay_scan() last went over, so the pump can do it while it
    // waits for the frame's time and avplay_show() does not do it again.
    const void *scannedY;
    uint64_t scannedPts;
    unsigned shown; // frames, for avplay_colorspace()
    uint64_t voPts;

    int durationMs;
    int frameUs;

    // The pump plays while `want` is set and waits otherwise. Everything that
    // touches the decoder from outside first clears `want` and waits for
    // `idle`: the stepper is not thread safe, and the pump owns it while it
    // plays.
    pthread_t thread;
    bool threadRunning;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool quit;
    bool want;
    bool idle;
    int state;
    int divider;
    int positionMs;
    int voOut;

    // The resume being timed, from avplay_play() to the first frame after.
    bool resuming;
    int64_t playAskedUs;
    int resumeFrom;
};

static int64_t avplay_nowUs(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

///////////////////////////////////////////////////////////////////////////////
// The output.

static ERRORTYPE avplay_voCallback(void *cookie, MPP_CHN_S *chn, MPP_EVENT_TYPE event, void *data) {
    AvPlay_t *ap = (AvPlay_t *)cookie;

    if (chn->mModId != MOD_ID_VOU || event != MPP_EVENT_RELEASE_VIDEO_BUFFER) {
        return SUCCESS;
    }

    // The output's copy, which it reuses once this returns.
    VIDEO_FRAME_INFO_S frame = *(VIDEO_FRAME_INFO_S *)data;
    framestep_put_frame(ap->fs, &frame);

    pthread_mutex_lock(&ap->lock);
    ap->voOut--;
    pthread_mutex_unlock(&ap->lock);

    return SUCCESS;
}

// Being tried: on a screen smaller than LVGL's 1920x1080, the display engine
// shows the UI layer (the bar) scaled into it instead of cut off. MPP sets the
// layer back to 1920x1080 at every AW_MPI_SYS_Init(), so this follows it.
static struct disp_rect avplay_uiWinSaved;
static bool avplay_uiWinChanged = false;

static void avplay_uiWindow(bool fit) {
    struct disp_layer_config cfg;
    unsigned long args[4] = {0, (unsigned long)&cfg, 1, 0};
    int fd;

    if (fit ? avplay_screenW == AVPLAY_WIDTH : !avplay_uiWinChanged) {
        return;
    }
    fd = open("/dev/disp", O_RDWR);
    if (fd < 0) {
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.channel = AVPLAY_uiCHN;
    cfg.layer_id = 0;
    if (ioctl(fd, DISP_LAYER_GET_CONFIG, args) == 0) {
        if (fit) {
            avplay_uiWinSaved = cfg.info.screen_win;
            cfg.info.screen_win.x = 0;
            cfg.info.screen_win.y = 0;
            cfg.info.screen_win.width = avplay_screenW;
            cfg.info.screen_win.height = avplay_screenH;
        } else {
            cfg.info.screen_win = avplay_uiWinSaved;
        }
        const int ret = ioctl(fd, DISP_LAYER_SET_CONFIG, args);
        avplay_uiWinChanged = fit && ret == 0;
        LOGI("UI layer window %ux%u (fb crop %llux%llu): %d", cfg.info.screen_win.width,
             cfg.info.screen_win.height, (unsigned long long)(cfg.info.fb.crop.width >> 32),
             (unsigned long long)(cfg.info.fb.crop.height >> 32), ret);
    }
    close(fd);
}

// Being tried: the colour space the display engine reads the video layer in.
// MPP's renderer declares it BT.601, and the output on the live side is BT.709,
// so the engine converts, which moves a coloured pixel's luma by a code or
// more (and its colour a little). Declared as the output's, the recording's
// values would reach the output as they are, as the live picture's do. The
// renderer hands libhwdisplay the layer's source once and libhwdisplay pushes
// its copy with every frame, so the copy is what gets changed
// (hwd_layer_set_src()). 0 leaves it; 1 BT.709, 2 BT.601, 3 BT.709 full,
// 4 BT.601 full.
static int avplay_csMode = 0;

void avplay_set_colorspace(int mode) {
    avplay_csMode = mode;
    LOGI("colour space mode %d", mode);
}

static void avplay_colorspace(void) {
    static const int spaces[] = {0, DISP_BT709, DISP_BT601, DISP_BT709_F, DISP_BT601_F};
    struct disp_layer_config cfg;
    unsigned long args[4] = {0, (unsigned long)&cfg, 1, 0};
    int fd;

    if (avplay_csMode <= 0 || avplay_csMode >= (int)(sizeof(spaces) / sizeof(spaces[0]))) {
        return;
    }
    fd = open("/dev/disp", O_RDWR);
    if (fd < 0) {
        return;
    }
    // The video layer: the first enabled YUV one below the UI's channel.
    for (int chn = 0; chn < AVPLAY_uiCHN; chn++) {
        for (int lyl = 0; lyl < 4; lyl++) {
            memset(&cfg, 0, sizeof(cfg));
            cfg.channel = chn;
            cfg.layer_id = lyl;
            if (ioctl(fd, DISP_LAYER_GET_CONFIG, args) != 0 || !cfg.enable ||
                cfg.info.fb.format < DISP_FORMAT_YUV444_I_AYUV) {
                continue;
            }
            const int want = spaces[avplay_csMode];
            if ((int)cfg.info.fb.color_space != want) {
                struct src_info src;
                memset(&src, 0, sizeof(src));
                src.w = cfg.info.fb.size[0].width;
                src.h = cfg.info.fb.size[0].height;
                src.crop_x = (unsigned)(cfg.info.fb.crop.x >> 32);
                src.crop_y = (unsigned)(cfg.info.fb.crop.y >> 32);
                src.crop_w = (unsigned)(cfg.info.fb.crop.width >> 32);
                src.crop_h = (unsigned)(cfg.info.fb.crop.height >> 32);
                src.format = cfg.info.fb.format == DISP_FORMAT_YUV420_P ? HWC_FORMAT_YUV420PLANAR
                                                                         : HWC_FORMAT_YUV420VUC;
                src.color_space = want;
                const int ret = hwd_layer_set_src(HLAY(chn, lyl), &src);
                LOGI("video layer %d/%d: format 0x%x, %ux%u crop %u,%u %ux%u, colour space 0x%x -> 0x%x: %d",
                     chn, lyl, cfg.info.fb.format, src.w, src.h, src.crop_x, src.crop_y, src.crop_w,
                     src.crop_h, cfg.info.fb.color_space, want, ret);
            }
            close(fd);
            return;
        }
    }
    close(fd);
}

// The way MPP's player set it up (vdec2vo_createVoChn(), since removed), less
// the binding.
static bool avplay_openVo(AvPlay_t *ap) {
    VO_VIDEO_LAYER_ATTR_S layerAttr;
    ERRORTYPE ret = FAILURE;

    ap->voDev = 0;
    ap->uiLayer = HLAY(AVPLAY_uiCHN, 0);

    AW_MPI_VO_Enable(ap->voDev);
    AW_MPI_VO_AddOutsideVideoLayer(ap->uiLayer);
    avplay_uiWindow(true);

    for (ap->voLayer = 0; ap->voLayer < VO_MAX_LAYER_NUM; ap->voLayer++) {
        if (AW_MPI_VO_EnableVideoLayer(ap->voLayer) == SUCCESS) {
            break;
        }
    }
    if (ap->voLayer >= VO_MAX_LAYER_NUM) {
        LOGE("no video layer");
        ap->voLayer = MM_INVALID_CHN;
        return false;
    }

    memset(&layerAttr, 0, sizeof(layerAttr));
    AW_MPI_VO_GetVideoLayerAttr(ap->voLayer, &layerAttr);
    layerAttr.stDispRect.X = 0;
    layerAttr.stDispRect.Y = 0;
    layerAttr.stDispRect.Width = avplay_screenW;
    layerAttr.stDispRect.Height = avplay_screenH;
    layerAttr.enPixFormat = MM_PIXEL_FORMAT_YVU_PLANAR_420;
    AW_MPI_VO_SetVideoLayerAttr(ap->voLayer, &layerAttr);

    for (ap->voChn = 0; ap->voChn < VO_MAX_CHN_NUM; ap->voChn++) {
        ret = AW_MPI_VO_EnableChn(ap->voLayer, ap->voChn);
        if (ret == SUCCESS || ret != ERR_VO_CHN_NOT_DISABLE) {
            break;
        }
    }
    if (ret != SUCCESS) {
        LOGE("enable vo channel: %x", ret);
        ap->voChn = MM_INVALID_CHN;
        return false;
    }

    MPPCallbackInfo cb = {.cookie = ap, .callback = (MPPCallbackFuncType)&avplay_voCallback};
    AW_MPI_VO_RegisterCallback(ap->voLayer, ap->voChn, &cb);
    AW_MPI_VO_SetChnDispBufNum(ap->voLayer, ap->voChn, AVPLAY_dispBUFS);

    ret = AW_MPI_VO_StartChn(ap->voLayer, ap->voChn);
    if (ret != SUCCESS) {
        LOGE("start vo channel: %x", ret);
        return false;
    }
    return true;
}

static void avplay_closeVo(AvPlay_t *ap) {
    if (ap->voLayer == MM_INVALID_CHN) {
        return;
    }

    if (ap->voChn != MM_INVALID_CHN) {
        // Stopping gives back the frames the output holds, through the
        // callback, which the decoder needs before it can be destroyed.
        const int64_t t0 = avplay_nowUs();
        int out;

        AW_MPI_VO_StopChn(ap->voLayer, ap->voChn);
        for (;;) {
            pthread_mutex_lock(&ap->lock);
            out = ap->voOut;
            pthread_mutex_unlock(&ap->lock);
            if (out <= 0 || avplay_nowUs() - t0 > AVPLAY_stopWAIT_MS * 1000) {
                break;
            }
            usleep(5000);
        }
        if (out > 0) {
            LOGW("%d frames never came back from the output", out);
        }
        AW_MPI_VO_DisableChn(ap->voLayer, ap->voChn);
    }

    AW_MPI_VO_DisableVideoLayer(ap->voLayer);
    avplay_uiWindow(false);
    AW_MPI_VO_RemoveOutsideVideoLayer(ap->uiLayer);
    AW_MPI_VO_Disable(ap->voDev);
}

// Being tried: the live side of the display shows the live layer wherever
// the SoC's output is the OSD's transparent grey, luma exactly 125 with
// neutral chroma. At 720p60 the frame reaches the output pixel for pixel
// (luma unchanged but for full-range blacks clamped to 16), so moving the
// frame's luma off 125 keeps the key from ever matching. The conversion to
// the output can move a slightly coloured pixel's luma by one, so mode n
// clears a band n-2 wide either side of 125 (2: 125 only, 3: 124-126,
// 4: 123-127), sending each value to the band's two neighbours with a 4x4
// ordered dither in proportion, so the average stays and no contour shows.
// Mode 6 clears 121-129, 9-11 predict the output (avplay_predictRow()).
// Mode 1 only counts 125s. The decoder predicts from these frames, so a
// change carries into the next ones (and is cleared again there).
static const unsigned char avplay_bayer[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

static long avplay_clearRow(unsigned char *row, int width, int r, int lo, int hi, bool change) {
    const uint8x16_t vlo = vdupq_n_u8((uint8_t)lo);
    const uint8x16_t vspan = vdupq_n_u8((uint8_t)(hi - lo));
    const int gap = hi - lo + 2; // between the two values a band pixel goes to
    long hits = 0;

    for (int x = 0; x + 16 <= width; x += 16) {
        const uint8x16_t in = vcleq_u8(vsubq_u8(vld1q_u8(row + x), vlo), vspan);
        const uint64x1_t any = vorr_u64(vreinterpret_u64_u8(vget_low_u8(in)),
                                        vreinterpret_u64_u8(vget_high_u8(in)));
        if (vget_lane_u64(any, 0) == 0) {
            continue;
        }
        for (int k = x; k < x + 16; k++) {
            const int v = row[k];
            if (v < lo || v > hi) {
                continue;
            }
            hits++;
            if (change) {
                // Up with probability (v - (lo - 1)) / gap.
                const int b = avplay_bayer[r & 3][k & 3];
                row[k] = (uint8_t)((32 * (v - lo + 1) > (2 * b + 1) * gap) ? hi + 1 : lo - 1);
            }
        }
    }
    return hits;
}

// Predicted: the output's conversion moves a coloured pixel's luma by about
// -(0.242 (V - 128) + 0.137 (U - 128)), measured on this display (the
// decoder's frames are BT.601, the output seemingly BT.709). A pixel near
// mid grey is moved only when its predicted output is within `half` (in
// 1/256) of 125, to 125 -/+ (half + 1/2) by the dither, so an error of a code
// in the prediction still misses 125. vPlane/uPlane are the frame's
// quarter-size chroma planes.
static long avplay_predictRow(unsigned char *row, const unsigned char *vRow, const unsigned char *uRow,
                              int width, int r, int half256) {
    const uint8x16_t vlo = vdupq_n_u8(125 - 12);
    const uint8x16_t vspan = vdupq_n_u8(24);
    long hits = 0;

    const int16x8_t k125 = vdupq_n_s16(125), k128 = vdupq_n_s16(128);
    const int16x8_t kMax = vdupq_n_s16(13), kMin = vdupq_n_s16(-13), kHalf = vdupq_n_s16(half256);

    for (int x = 0; x + 16 <= width; x += 16) {
        const uint8x16_t yv = vld1q_u8(row + x);
        const uint8x16_t near = vcleq_u8(vsubq_u8(yv, vlo), vspan);
        if (vget_lane_u64(vorr_u64(vreinterpret_u64_u8(vget_low_u8(near)),
                                   vreinterpret_u64_u8(vget_high_u8(near))),
                          0) == 0) {
            continue;
        }
        // The same prediction as below, eight pixels at a time, in 1/256 of
        // a code about 125: (y - 125) * 256 - (62 dv + 35 du).
        const uint8x8x2_t vz = vzip_u8(vld1_u8(vRow + (x >> 1)), vld1_u8(vRow + (x >> 1)));
        const uint8x8x2_t uz = vzip_u8(vld1_u8(uRow + (x >> 1)), vld1_u8(uRow + (x >> 1)));
        uint16x8_t hit[2];
        for (int h = 0; h < 2; h++) {
            const uint8x8_t y8 = h ? vget_high_u8(yv) : vget_low_u8(yv);
            int16x8_t dy = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(y8)), k125);
            dy = vmaxq_s16(vminq_s16(dy, kMax), kMin);
            const int16x8_t dv = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vz.val[h])), k128);
            const int16x8_t du = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(uz.val[h])), k128);
            const int16x8_t shift = vnegq_s16(vmlaq_n_s16(vmulq_n_s16(dv, 62), du, 35));
            const int16x8_t p = vmlaq_n_s16(shift, dy, 256);
            hit[h] = vandq_u16(vcgeq_s16(p, vnegq_s16(kHalf)), vcltq_s16(p, kHalf));
        }
        const uint16x8_t any = vorrq_u16(hit[0], hit[1]);
        if (vget_lane_u64(vorr_u64(vreinterpret_u64_u16(vget_low_u16(any)),
                                   vreinterpret_u64_u16(vget_high_u16(any))),
                          0) == 0) {
            continue;
        }
        for (int k = x; k < x + 16; k++) {
            const int v = row[k];
            if (v < 125 - 12 || v > 125 + 12) {
                continue;
            }
            const int dv = vRow[k >> 1] - 128, du = uRow[k >> 1] - 128;
            const int shift256 = -(62 * dv + 35 * du);
            const int p256 = v * 256 + shift256; // predicted output, 1/256
            if (p256 < 125 * 256 - half256 || p256 >= 125 * 256 + half256) {
                continue;
            }
            hits++;
            // To one side or the other, up with the predicted value's share.
            const int b = avplay_bayer[r & 3][k & 3];
            const int span256 = 2 * half256 + 256;
            const int from256 = p256 - (125 * 256 - half256 - 128);
            const int up = 16 * from256 > (2 * b + 1) * span256 / 2;
            const int target256 = up ? 125 * 256 + half256 + 128 : 125 * 256 - half256 - 128;
            int out = (target256 - shift256 + 128) >> 8;
            row[k] = (uint8_t)(out < 0 ? 0 : out > 255 ? 255 : out);
        }
    }
    return hits;
}

static void avplay_scan(VIDEO_FRAME_INFO_S *frame) {
    VIDEO_FRAME_S *f = &frame->VFrame;
    unsigned char *y = f->mpVirAddr[0];
    const int width = (int)f->mWidth, height = (int)f->mHeight;
    const int mode = avplay_scanMode;
    const int64_t t0 = avplay_nowUs();
    long hits = 0;

    if (!y) {
        return;
    }
    if (mode >= 9) {
        // YV12: V, then U, each a quarter of the luma.
        const unsigned char *p1 = f->mpVirAddr[1];
        const unsigned char *p2 = f->mpVirAddr[2] ? f->mpVirAddr[2] : p1 + (size_t)(width / 2) * (height / 2);
        const unsigned char *vPlane = mode == 10 ? p2 : p1;
        const unsigned char *uPlane = mode == 10 ? p1 : p2;
        const int half256 = mode == 11 ? 640 : 384;
        static int logged = 0;
        if (logged != mode) {
            logged = mode;
            LOGI("scan %d: planes %p %p %p, %dx%d", mode, f->mpVirAddr[0], f->mpVirAddr[1],
                 f->mpVirAddr[2], width, height);
        }
        for (int r = 0; r < height; r++) {
            const size_t c = (size_t)(r >> 1) * (width / 2);
            hits += avplay_predictRow(y + (size_t)r * width, vPlane + c, uPlane + c, width, r, half256);
        }
    } else {
        const int half = mode == 6 ? 4 : mode >= 2 ? mode - 2 : 0;
        for (int r = 0; r < height; r++) {
            hits += avplay_clearRow(y + (size_t)r * width, width, r, 125 - half, 125 + half, mode >= 2);
        }
    }
    if (mode >= 2) {
        AW_MPI_SYS_MmzFlushCache(f->mPhyAddr[0], y, (unsigned)(width * height));
    }

    const int64_t us = avplay_nowUs() - t0;
    avplay_scanStat.frames++;
    avplay_scanStat.us += us;
    avplay_scanStat.hits += hits;
    if (us > avplay_scanStat.maxUs) {
        avplay_scanStat.maxUs = us;
    }
    if (avplay_scanStat.frames == 60) {
        LOGI("scan %d: %dx%d, 60 frames, %lldus a frame, worst %lldus, %ld hits a frame", mode, width,
             height, (long long)(avplay_scanStat.us / 60), (long long)avplay_scanStat.maxUs,
             avplay_scanStat.hits / 60);
        memset(&avplay_scanStat, 0, sizeof(avplay_scanStat));
    }
}

void avplay_set_scan(int mode) {
    avplay_scanMode = mode;
    memset(&avplay_scanStat, 0, sizeof(avplay_scanStat));
    LOGI("scan mode %d", mode);
}

void avplay_set_screen(int width, int height) {
    avplay_screenW = width;
    avplay_screenH = height;
}

// Puts a frame from the decoder on screen, and it is then the position.
// The frame goes to the output either way.
static bool avplay_show(AvPlay_t *ap, VIDEO_FRAME_INFO_S *frame) {
    const int ms = (int)(frame->VFrame.mpts / 1000);

    if (avplay_scanMode &&
        !(frame->VFrame.mpVirAddr[0] == ap->scannedY && frame->VFrame.mpts == ap->scannedPts)) {
        avplay_scan(frame);
    }

    ap->voPts += AVPLAY_voSTEP_US;
    frame->VFrame.mpts = ap->voPts;

    const ERRORTYPE ret = AW_MPI_VO_SendFrame(ap->voLayer, ap->voChn, frame, AVPLAY_sendWAIT_MS);
    if (ret != SUCCESS) {
        LOGW("output refused the frame at %dms: %x", ms, ret);
        framestep_put_frame(ap->fs, frame);
        return false;
    }

    pthread_mutex_lock(&ap->lock);
    ap->voOut++;
    ap->positionMs = ms;
    pthread_mutex_unlock(&ap->lock);
    // Checked again now and then in case the renderer sets it back.
    if (avplay_csMode && (ap->shown++ % 30) == 0) {
        avplay_colorspace();
    }
    return true;
}

///////////////////////////////////////////////////////////////////////////////
// The pump.

// Sleeps until `until`, or less if playing is called off. Returns whether it
// still is.
static bool avplay_waitUntil(AvPlay_t *ap, int64_t until) {
    struct timespec ts = {.tv_sec = until / 1000000, .tv_nsec = (until % 1000000) * 1000};
    bool wanted;

    pthread_mutex_lock(&ap->lock);
    while (ap->want && avplay_nowUs() < until) {
        if (pthread_cond_timedwait(&ap->cond, &ap->lock, &ts) == ETIMEDOUT) {
            break;
        }
    }
    wanted = ap->want;
    pthread_mutex_unlock(&ap->lock);
    return wanted;
}

static bool avplay_wanted(AvPlay_t *ap) {
    bool wanted;

    pthread_mutex_lock(&ap->lock);
    wanted = ap->want;
    pthread_mutex_unlock(&ap->lock);
    return wanted;
}

// One run of playing, until it is called off or the file ends.
static void avplay_run(AvPlay_t *ap, int divider) {
    const bool sound = ap->as && divider == 1;
    const int64_t audioDeadline = avplay_nowUs() + (int64_t)AVPLAY_audioWAIT_MS * 1000;
    bool wallClock = !sound;
    int64_t base = -1, pts0 = 0, soundAt = 0;
    int sent = 0, late = 0;
    int64_t worst = 0;
    int64_t nextLog = avplay_nowUs() + 1000000;

    for (;;) {
        VIDEO_FRAME_INFO_S frame;

        if (!avplay_wanted(ap)) {
            break;
        }
        if (!framestep_get_frame(ap->fs, &frame)) {
            LOGI("end of the file at %dms", ap->positionMs);
            pthread_mutex_lock(&ap->lock);
            ap->want = false;
            ap->state = AVPLAY_ENDED;
            ap->resuming = false;
            pthread_cond_broadcast(&ap->cond);
            pthread_mutex_unlock(&ap->lock);
            if (ap->as) {
                asend_pause(ap->as);
            }
            break;
        }

        if (avplay_scanMode) {
            avplay_scan(&frame);
            ap->scannedY = frame.VFrame.mpVirAddr[0];
            ap->scannedPts = frame.VFrame.mpts;
        }

        const int64_t pts = (int64_t)frame.VFrame.mpts;
        int64_t due = -1;

        // The frame is due when the sound reaches it. Until the sound has
        // started there is nothing to go by, so the picture holds.
        while (!wallClock) {
            const int64_t now = avplay_nowUs();
            int64_t heard;

            if (asend_audible(ap->as, ASEND_CLOCK_START, now, &heard)) {
                if (!soundAt) {
                    soundAt = now;
                }
                due = now + (pts - heard);
                break;
            }
            if (now > audioDeadline) {
                LOGW("no sound %dms after play, going by the wall clock", AVPLAY_audioWAIT_MS);
                wallClock = true;
                break;
            }
            if (asend_exhausted(ap->as)) {
                LOGI("no sound left from here, going by the wall clock");
                wallClock = true;
                break;
            }
            if (!avplay_waitUntil(ap, now + 2000)) {
                break;
            }
        }
        if (wallClock) {
            if (base < 0) {
                base = avplay_nowUs();
                pts0 = pts;
            }
            due = base + (pts - pts0) * divider;
        }

        if (due < 0 || !avplay_waitUntil(ap, due)) {
            // Called off while waiting: this frame is the next one out.
            framestep_unget_frame(ap->fs, &frame);
            break;
        }

        const int64_t lateBy = avplay_nowUs() - due;
        if (lateBy > worst) {
            worst = lateBy;
        }
        if (lateBy > ap->frameUs) {
            late++;
        }

        if (!avplay_show(ap, &frame)) {
            continue;
        }
        sent++;

        // A line a second on the sound while it plays: what it has taken,
        // and the two clocks side by side.
        if (sound && avplay_nowUs() >= nextLog) {
            asend_log(ap->as, avplay_nowUs());
            nextLog += 1000000;
        }

        pthread_mutex_lock(&ap->lock);
        const bool timed = ap->resuming;
        ap->resuming = false;
        pthread_mutex_unlock(&ap->lock);

        if (timed) {
            // One frame on is seamless; drop_delta is how far past that.
            const int first = (int)(pts / 1000);
            const int frameMs = ap->frameUs / 1000;
            LOGI("AT resume target=%d first=%d seeks=1 hold_ms=%d sound_ms=%d drop_delta=%d result=ok",
                 ap->resumeFrom, first, (int)((avplay_nowUs() - ap->playAskedUs) / 1000),
                 soundAt ? (int)((soundAt - ap->playAskedUs) / 1000) : -1,
                 first - ap->resumeFrom - frameMs);
        }
    }

    LOGI("played %d frames at 1/%d, %d late, worst %dms, %s", sent, divider, late,
         (int)(worst / 1000), wallClock ? "by the wall clock" : "by the sound");
}

static void *avplay_pump(void *arg) {
    AvPlay_t *ap = (AvPlay_t *)arg;

    for (;;) {
        pthread_mutex_lock(&ap->lock);
        while (!ap->quit && !ap->want) {
            ap->idle = true;
            pthread_cond_broadcast(&ap->cond);
            pthread_cond_wait(&ap->cond, &ap->lock);
        }
        if (ap->quit) {
            ap->idle = true;
            pthread_cond_broadcast(&ap->cond);
            pthread_mutex_unlock(&ap->lock);
            break;
        }
        ap->idle = false;
        const int divider = ap->divider;
        pthread_mutex_unlock(&ap->lock);

        avplay_run(ap, divider);
    }

    return NULL;
}

// Calls playing off and waits until the pump has let go of the decoder.
static void avplay_halt(AvPlay_t *ap) {
    pthread_mutex_lock(&ap->lock);
    ap->want = false;
    ap->resuming = false;
    pthread_cond_broadcast(&ap->cond);
    while (!ap->idle) {
        pthread_cond_wait(&ap->cond, &ap->lock);
    }
    if (ap->state == AVPLAY_PLAYING) {
        ap->state = AVPLAY_PAUSED;
    }
    pthread_mutex_unlock(&ap->lock);
}

///////////////////////////////////////////////////////////////////////////////

AvPlay_t *avplay_open(const char *path) {
    const int64_t t0 = avplay_nowUs();
    AvPlay_t *ap = calloc(1, sizeof(*ap));
    MPP_SYS_CONF_S sysConf;
    pthread_condattr_t attr;

    if (!ap) {
        return NULL;
    }
    ap->voLayer = MM_INVALID_CHN;
    ap->voChn = MM_INVALID_CHN;
    ap->divider = 1;
    ap->idle = true;
    pthread_mutex_init(&ap->lock, NULL);
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&ap->cond, &attr);
    pthread_condattr_destroy(&attr);

    memset(&sysConf, 0, sizeof(sysConf));
    sysConf.nAlignWidth = 32;
    AW_MPI_SYS_SetConf(&sysConf);
    AW_MPI_SYS_Init();

    ap->fs = framestep_open(path, AVPLAY_extraFRAMES);
    if (!ap->fs || !avplay_openVo(ap)) {
        avplay_close(ap);
        return NULL;
    }

    const int rate = framestep_frame_rate(ap->fs); // x1000
    ap->frameUs = rate > 0 ? (int)(1000000000LL / rate) : 16667;
    ap->durationMs = framestep_duration_ms(ap->fs);

    const int64_t tVideo = avplay_nowUs();
    ap->as = asend_open_paused(path, false);
    if (!ap->as) {
        LOGI("no sound in %s; the picture goes by the wall clock", path);
    }
    const int64_t tAudio = avplay_nowUs();

    if (pthread_create(&ap->thread, NULL, avplay_pump, ap) != 0) {
        avplay_close(ap);
        return NULL;
    }
    ap->threadRunning = true;

    VIDEO_FRAME_INFO_S frame;
    if (!framestep_get_frame(ap->fs, &frame) || !avplay_show(ap, &frame)) {
        LOGE("no first frame in %s", path);
        avplay_close(ap);
        return NULL;
    }

    LOGI("opened %s: %dms, %d.%03dfps, sound %s; video %dms, sound %dms, first frame %dms",
         path, ap->durationMs, rate / 1000, rate % 1000, ap->as ? "yes" : "no",
         (int)((tVideo - t0) / 1000), (int)((tAudio - tVideo) / 1000),
         (int)((avplay_nowUs() - tAudio) / 1000));
    return ap;
}

void avplay_close(AvPlay_t *ap) {
    if (!ap) {
        return;
    }

    if (ap->threadRunning) {
        pthread_mutex_lock(&ap->lock);
        ap->quit = true;
        ap->want = false;
        pthread_cond_broadcast(&ap->cond);
        pthread_mutex_unlock(&ap->lock);
        pthread_join(ap->thread, NULL);
    }

    asend_close(ap->as);
    // The output first: it gives its frames back to the decoder as it stops.
    avplay_closeVo(ap);
    framestep_close(ap->fs);
    AW_MPI_SYS_Exit();

    pthread_cond_destroy(&ap->cond);
    pthread_mutex_destroy(&ap->lock);
    free(ap);
}

int avplay_duration_ms(AvPlay_t *ap) {
    return ap ? ap->durationMs : 0;
}

int avplay_frame_ms(AvPlay_t *ap) {
    return ap ? ap->frameUs / 1000 : 16;
}

int avplay_position_ms(AvPlay_t *ap) {
    int ms;

    if (!ap) {
        return 0;
    }
    pthread_mutex_lock(&ap->lock);
    ms = ap->positionMs;
    pthread_mutex_unlock(&ap->lock);
    return ms;
}

int avplay_state(AvPlay_t *ap) {
    int state;

    if (!ap) {
        return AVPLAY_PAUSED;
    }
    pthread_mutex_lock(&ap->lock);
    state = ap->state;
    pthread_mutex_unlock(&ap->lock);
    return state;
}

bool avplay_has_audio(AvPlay_t *ap) {
    return ap && ap->as;
}

bool avplay_resuming(AvPlay_t *ap) {
    bool resuming;

    if (!ap) {
        return false;
    }
    pthread_mutex_lock(&ap->lock);
    resuming = ap->resuming;
    pthread_mutex_unlock(&ap->lock);
    return resuming;
}

void avplay_pause(AvPlay_t *ap) {
    if (!ap) {
        return;
    }
    avplay_halt(ap);
    if (ap->as) {
        asend_pause(ap->as);
    }
    const int at = avplay_position_ms(ap);
    LOGI("AT pause placed=%d paused_at=%d output=%d clock=%d delta=0", at, at, at, at);
}

void avplay_play(AvPlay_t *ap, int divider) {
    if (!ap) {
        return;
    }
    if (divider < 1) {
        divider = 1;
    }

    avplay_halt(ap);

    if (avplay_state(ap) == AVPLAY_ENDED) {
        VIDEO_FRAME_INFO_S frame;
        if (!framestep_get_frame_at(ap->fs, 0, &frame) || !avplay_show(ap, &frame)) {
            return;
        }
    }

    const int from = avplay_position_ms(ap);

    // The sound from the frame on screen; the next frame is due when it has
    // played up to it.
    if (ap->as) {
        if (divider == 1) {
            asend_play_from(ap->as, from);
        } else {
            asend_pause(ap->as);
        }
    }

    pthread_mutex_lock(&ap->lock);
    ap->divider = divider;
    ap->resuming = true;
    ap->playAskedUs = avplay_nowUs();
    ap->resumeFrom = from;
    ap->state = AVPLAY_PLAYING;
    ap->want = true;
    pthread_cond_broadcast(&ap->cond);
    pthread_mutex_unlock(&ap->lock);
}

bool avplay_step(AvPlay_t *ap, bool forward) {
    VIDEO_FRAME_INFO_S frame;

    if (!ap) {
        return false;
    }

    avplay_halt(ap);
    if (ap->as) {
        asend_pause(ap->as);
    }

    const int from = avplay_position_ms(ap);
    const bool got = forward ? framestep_get_frame(ap->fs, &frame)
                             : framestep_get_prev_frame(ap->fs, from, &frame);
    if (!got) {
        return false;
    }
    if (!avplay_show(ap, &frame)) {
        return false;
    }

    pthread_mutex_lock(&ap->lock);
    ap->state = AVPLAY_PAUSED; // an ended file can be stepped back into
    pthread_mutex_unlock(&ap->lock);
    return true;
}

bool avplay_seek(AvPlay_t *ap, int ms) {
    VIDEO_FRAME_INFO_S frame;
    bool playing;

    if (!ap) {
        return false;
    }

    pthread_mutex_lock(&ap->lock);
    playing = ap->want;
    pthread_mutex_unlock(&ap->lock);

    avplay_halt(ap);
    if (ap->as) {
        asend_pause(ap->as);
    }

    if (ms < 0) {
        ms = 0;
    }
    bool got = framestep_get_frame_at(ap->fs, ms, &frame) && avplay_show(ap, &frame);
    if (!got) {
        // Past the last frame (the dial stops at the file's length, which is
        // the end of the last frame): show the last one, as the end of the
        // file would. Left with nothing shown, the picture stayed where it
        // was while the decoder sat at the end, and a step forward from it
        // found nothing.
        //
        // Looked for back from the end rather than from the position asked
        // for: a step back starts decoding a keyframe or so before where it
        // is asked, and from far past the end that is past it too (measured:
        // asked from 60s past a 9s file, it found nothing).
        const int from = (ap->durationMs > 0 && ap->durationMs < ms) ? ap->durationMs : ms;
        got = framestep_get_prev_frame(ap->fs, from, &frame) && avplay_show(ap, &frame);
        if (got) {
            LOGI("nothing at %dms, showing the last frame, %dms", ms, avplay_position_ms(ap));
        }
    }
    if (got) {
        pthread_mutex_lock(&ap->lock);
        ap->state = AVPLAY_PAUSED;
        pthread_mutex_unlock(&ap->lock);
    } else {
        LOGW("nothing at %dms", ms);
    }

    if (playing) {
        avplay_play(ap, ap->divider);
    }
    return got;
}
