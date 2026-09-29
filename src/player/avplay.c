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
// Late frames let go in a row at most, before one is shown however late.
#define AVPLAY_dropMAX 6
#define AVPLAY_stopWAIT_MS 500

#if defined HDZBOXPRO
#define AVPLAY_WIDTH  1280
#define AVPLAY_HEIGHT 720
#else
#define AVPLAY_WIDTH  1920
#define AVPLAY_HEIGHT 1080
#endif
#define AVPLAY_uiCHN 2

// Where the picture goes, from the next avplay_open() (avplay_set_output()).
static int avplay_screenW = AVPLAY_WIDTH;
static int avplay_screenH = AVPLAY_HEIGHT;
static bool avplay_live = false;
// For measuring: false leaves the frames as they are on the live side.
static bool avplay_clear = true;

void avplay_set_clear(bool on) {
    avplay_clear = on;
}

struct AvPlay {
    FrameStep_t *fs;
    ASend_t *as; // NULL when the file has no sound

    VO_DEV voDev;
    VO_LAYER voLayer;
    VO_CHN voChn;
    int uiLayer;
    // The frame avplay_clearKey() last went over, so the pump can do it while
    // it waits for the frame's time and avplay_show() does not do it again.
    const void *clearedY;
    uint64_t clearedPts;
    unsigned shown;     // frames, for avplay_colorspace()
    int videoW, videoH; // the first frame's
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

// On a screen smaller than LVGL's 1920x1080 (the goggle 2's 720p60) the
// display engine shows the UI layer (the bar) scaled into it instead of cut
// off; the FPGA scales it back up with the picture. MPP sets the
// layer back to 1920x1080 at every AW_MPI_SYS_Init(), so this follows it.
static bool avplay_uiWinChanged = false;

// fit: into the screen, after MPP's reset. Otherwise, and for a 1080p screen,
// back to the whole 1920x1080 window, for once the menu's display is back (on
// the 720p screen still up at close a 1080p window is not taken).
static void avplay_uiWindow(bool fit) {
    struct disp_layer_config cfg;
    unsigned long args[4] = {0, (unsigned long)&cfg, 1, 0};
    int fd;

    const bool whole = !fit || avplay_screenW == AVPLAY_WIDTH;
    if (whole && !avplay_uiWinChanged) {
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
        LOGI("UI layer was window %d,%d %ux%u, fb %ux%u crop %llux%llu", cfg.info.screen_win.x,
             cfg.info.screen_win.y, cfg.info.screen_win.width, cfg.info.screen_win.height,
             cfg.info.fb.size[0].width, cfg.info.fb.size[0].height,
             (unsigned long long)(cfg.info.fb.crop.width >> 32),
             (unsigned long long)(cfg.info.fb.crop.height >> 32));
        cfg.info.screen_win.x = 0;
        cfg.info.screen_win.y = 0;
        cfg.info.screen_win.width = whole ? AVPLAY_WIDTH : avplay_screenW;
        cfg.info.screen_win.height = whole ? AVPLAY_HEIGHT : avplay_screenH;
        cfg.info.fb.crop.x = 0;
        cfg.info.fb.crop.y = 0;
        cfg.info.fb.crop.width = (unsigned long long)AVPLAY_WIDTH << 32;
        cfg.info.fb.crop.height = (unsigned long long)AVPLAY_HEIGHT << 32;
        const int ret = ioctl(fd, DISP_LAYER_SET_CONFIG, args);
        avplay_uiWinChanged = !whole && ret == 0;
        LOGI("UI layer window now %ux%u: %d", cfg.info.screen_win.width, cfg.info.screen_win.height, ret);
    }
    close(fd);
}

void avplay_restore_ui(void) {
    avplay_uiWindow(false);
}

// On the live side the display engine's output is BT.709, and MPP's renderer
// declares the video layer BT.601, so the engine converted every frame: that
// moved a coloured pixel's luma by up to several codes, which avplay_clearKey()
// cannot see coming, and tinted the colours a little. Declared BT.709, a
// recording's values reach the output as they are, as the live picture's do
// (measured: 92% of a frame's luma identical against 33%). The renderer hands
// libhwdisplay the layer's source once and libhwdisplay pushes its copy with
// every frame, so the copy is what is changed (hwd_layer_set_src()).
static void avplay_colorspace(void) {
    const int want = DISP_BT709;
    struct disp_layer_config cfg;
    unsigned long args[4] = {0, (unsigned long)&cfg, 1, 0};
    int fd;

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
    AW_MPI_VO_RemoveOutsideVideoLayer(ap->uiLayer);
    AW_MPI_VO_Disable(ap->voDev);
}

// The live side shows the live layer wherever the SoC's output has luma
// exactly 125: that is the live OSD's transparent grey (0x7f7f7f,
// style_osd), and recordings have it here and there, around lights and
// letters mostly, which came through as specks (magenta with the receiver
// off). With the frame reaching the output pixel for pixel (a screen its own
// size, the layer in the output's colour space) moving the frame's luma off
// 125 keeps the key from matching. A band 123-127 is cleared, not 125
// alone: the output's rounding still moves some pixels by one. Each value
// goes to 122 or 128 by a 4x4 ordered dither in proportion, so the average
// stays and no contour shows. Measured on a 720p recording: 4 pixels through
// in 40 frames, against about 250 a frame, at about 2.5ms a frame (NEON: 16
// pixels at a time, and only blocks with a value in the band touched). The
// decoder predicts from these frames, so a change carries into the next
// ones, where it is cleared again.
#define AVPLAY_keyLUMA 125
#define AVPLAY_keyHALF 2

static const unsigned char avplay_bayer[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// 16 pixels at a time throughout: a band pixel v goes up to hi + 1 when
// v - (lo - 1) > up[x & 3], its dither threshold, else down to lo - 1. up[]
// is (2b + 1) * gap / 32 for the row's Bayer values b, rounded down, which
// gives the same choice as comparing 32 (v - lo + 1) with (2b + 1) gap.
static void avplay_clearRow(unsigned char *row, int width, int r) {
    const int lo = AVPLAY_keyLUMA - AVPLAY_keyHALF, hi = AVPLAY_keyLUMA + AVPLAY_keyHALF;
    const int gap = hi - lo + 2; // between the two values a band pixel goes to
    uint8_t up[16];
    for (int k = 0; k < 16; k++) {
        up[k] = (uint8_t)(((2 * avplay_bayer[r & 3][k & 3] + 1) * gap) / 32);
    }
    const uint8x16_t vUp = vld1q_u8(up);
    const uint8x16_t vBelow = vdupq_n_u8((uint8_t)(lo - 1));
    const uint8x16_t vSpan = vdupq_n_u8((uint8_t)(hi - lo));
    const uint8x16_t vLo = vdupq_n_u8((uint8_t)lo);
    const uint8x16_t vDown = vdupq_n_u8((uint8_t)(lo - 1));
    const uint8x16_t vHigh = vdupq_n_u8((uint8_t)(hi + 1));

    for (int x = 0; x + 16 <= width; x += 16) {
        const uint8x16_t v = vld1q_u8(row + x);
        const uint8x16_t in = vcleq_u8(vsubq_u8(v, vLo), vSpan);
        if (vget_lane_u64(vorr_u64(vreinterpret_u64_u8(vget_low_u8(in)),
                                   vreinterpret_u64_u8(vget_high_u8(in))),
                          0) == 0) {
            continue;
        }
        const uint8x16_t goUp = vcgtq_u8(vsubq_u8(v, vBelow), vUp);
        const uint8x16_t to = vbslq_u8(goUp, vHigh, vDown);
        vst1q_u8(row + x, vbslq_u8(in, to, v));
    }
}

static void avplay_clearRows(VIDEO_FRAME_S *f, int r0, int r1) {
    unsigned char *y = f->mpVirAddr[0];
    const int width = (int)f->mWidth;

    for (int r = r0; r < r1; r++) {
        avplay_clearRow(y + (size_t)r * width, width, r);
    }
}

// A frame wider than 720p's is shared with a second thread, half the rows
// each. The time is in reading the frame (uncached, some 300MB/s), so the
// two together take about as long as one, but the long stalls go: on a
// 1080p60 clip 4-8 frames in 8s came late against 20-27, where 720p's
// single thread's 2.5ms a frame leaves 1-2. (Flushing only the rows
// changed was slower still, 10ms: the whole plane's flush is 0.2ms.)
static struct {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool started, busy;
    VIDEO_FRAME_S *frame;
    int r0, r1;
} avplay_helper = {.lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};

static void *avplay_helperRun(void *arg) {
    (void)arg;
    pthread_mutex_lock(&avplay_helper.lock);
    for (;;) {
        while (!avplay_helper.busy) {
            pthread_cond_wait(&avplay_helper.cond, &avplay_helper.lock);
        }
        VIDEO_FRAME_S *f = avplay_helper.frame;
        const int r0 = avplay_helper.r0, r1 = avplay_helper.r1;
        pthread_mutex_unlock(&avplay_helper.lock);
        avplay_clearRows(f, r0, r1);
        pthread_mutex_lock(&avplay_helper.lock);
        avplay_helper.busy = false;
        pthread_cond_broadcast(&avplay_helper.cond);
    }
    return NULL;
}

static void avplay_clearKey(AvPlay_t *ap, VIDEO_FRAME_INFO_S *frame) {
    VIDEO_FRAME_S *f = &frame->VFrame;
    unsigned char *y = f->mpVirAddr[0];
    const int width = (int)f->mWidth, height = (int)f->mHeight;

    ap->clearedY = y;
    ap->clearedPts = f->mpts;
    if (!y) {
        return;
    }
    const int64_t t0 = avplay_nowUs();
    if (width > 1280) {
        if (!avplay_helper.started) {
            avplay_helper.started = pthread_create(&avplay_helper.thread, NULL, avplay_helperRun, NULL) == 0;
        }
        if (avplay_helper.started) {
            const int half = (height / 2) & ~3;
            pthread_mutex_lock(&avplay_helper.lock);
            avplay_helper.frame = f;
            avplay_helper.r0 = half;
            avplay_helper.r1 = height;
            avplay_helper.busy = true;
            pthread_cond_broadcast(&avplay_helper.cond);
            pthread_mutex_unlock(&avplay_helper.lock);
            avplay_clearRows(f, 0, half);
            pthread_mutex_lock(&avplay_helper.lock);
            while (avplay_helper.busy) {
                pthread_cond_wait(&avplay_helper.cond, &avplay_helper.lock);
            }
            pthread_mutex_unlock(&avplay_helper.lock);
        } else {
            avplay_clearRows(f, 0, height);
        }
    } else {
        avplay_clearRows(f, 0, height);
    }
    AW_MPI_SYS_MmzFlushCache(f->mPhyAddr[0], y, (unsigned)(width * height));
}

void avplay_set_output(int width, int height, bool live) {
    avplay_screenW = width;
    avplay_screenH = height;
    avplay_live = live;
}

// Puts a frame from the decoder on screen, and it is then the position.
// The frame goes to the output either way.
static bool avplay_show(AvPlay_t *ap, VIDEO_FRAME_INFO_S *frame) {
    const int ms = (int)(frame->VFrame.mpts / 1000);

    if (avplay_live && avplay_clear &&
        !(frame->VFrame.mpVirAddr[0] == ap->clearedY && frame->VFrame.mpts == ap->clearedPts)) {
        avplay_clearKey(ap, frame);
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
    if (avplay_live && (ap->shown++ % 30) == 0) {
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
    // Where the time went, for the late frames above all.
    int64_t getSum = 0, getMax = 0, clearSum = 0, clearMax = 0, showSum = 0, showMax = 0;
    int lateLogged = 0;
    int dropped = 0, droppedInRow = 0;

    for (;;) {
        VIDEO_FRAME_INFO_S frame;

        if (!avplay_wanted(ap)) {
            break;
        }
        const int64_t tGet = avplay_nowUs();
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

        const int64_t tGot = avplay_nowUs();

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

        // A frame already a frame late is let go, up to a few in a row, so
        // the picture catches the sound up at once instead of a frame's slack
        // at a time (it starts about 70ms behind), and none of the time goes
        // on clearing frames nobody sees.
        if (due >= 0 && avplay_nowUs() - due > ap->frameUs && droppedInRow < AVPLAY_dropMAX) {
            framestep_put_frame(ap->fs, &frame);
            dropped++;
            droppedInRow++;
            continue;
        }
        droppedInRow = 0;

        const int64_t tClear = avplay_nowUs();
        if (avplay_live && avplay_clear) {
            avplay_clearKey(ap, &frame);
        }
        const int64_t tCleared = avplay_nowUs();

        if (due < 0 || !avplay_waitUntil(ap, due)) {
            // Called off while waiting: this frame is the next one out.
            framestep_unget_frame(ap->fs, &frame);
            break;
        }

        const int64_t lateBy = avplay_nowUs() - due;
        if (lateBy > worst) {
            worst = lateBy;
        }
        const bool isLate = lateBy > ap->frameUs;
        if (isLate) {
            late++;
        }

        const int64_t tShow = avplay_nowUs();
        if (!avplay_show(ap, &frame)) {
            continue;
        }
        const int64_t getUs = tGot - tGet, clearUs = tCleared - tClear, showUs = avplay_nowUs() - tShow;
        getSum += getUs;
        clearSum += clearUs;
        showSum += showUs;
        getMax = getUs > getMax ? getUs : getMax;
        clearMax = clearUs > clearMax ? clearUs : clearMax;
        showMax = showUs > showMax ? showUs : showMax;
        if (isLate && lateLogged < 5) {
            lateLogged++;
            LOGI("late frame %d (%dms): %lldms late; decoder %lldus, clearing %lldus, output %lldus", sent,
                 (int)(pts / 1000), (long long)(lateBy / 1000), (long long)getUs, (long long)clearUs,
                 (long long)showUs);
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

    LOGI("played %d frames at 1/%d, %d late, worst %dms, %d let go, %s", sent, divider, late,
         (int)(worst / 1000), dropped, wallClock ? "by the wall clock" : "by the sound");
    if (sent > 0) {
        LOGI("per frame: decoder %lldus (worst %lldus), clearing %lldus (%lldus), output %lldus (%lldus)",
             (long long)(getSum / sent), (long long)getMax, (long long)(clearSum / sent),
             (long long)clearMax, (long long)(showSum / sent), (long long)showMax);
    }
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
    if (!framestep_get_frame(ap->fs, &frame)) {
        LOGE("no first frame in %s", path);
        avplay_close(ap);
        return NULL;
    }
    ap->videoW = (int)frame.VFrame.mWidth;
    ap->videoH = frame.VFrame.mOffsetBottom > frame.VFrame.mOffsetTop
                     ? frame.VFrame.mOffsetBottom - frame.VFrame.mOffsetTop
                     : (int)frame.VFrame.mHeight;
    if (!avplay_show(ap, &frame)) {
        LOGE("the first frame of %s did not go out", path);
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

void avplay_video_size(AvPlay_t *ap, int *width, int *height) {
    *width = ap->videoW;
    *height = ap->videoH;
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
