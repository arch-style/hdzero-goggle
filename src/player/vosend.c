#include "vosend.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_TAG "vosend"
#include <log/log.h>

#include <hwdisplay.h>
#include <mpi_sys.h>
#include <mpi_vdec.h>
#include <mpi_vo.h>

#include "asend.h"
#include "framestep.h"

// What was read out of libmpp_vo.so before writing this, so that the numbers
// below can be checked against it:
//
// - AW_MPI_VO_SendFrame() answers NOT_PERM (a00f8009) unless the channel is
//   started or paused, and otherwise hands the frame to the component's
//   EmptyThisBuffer, the same entry a bound decoder uses.
//
// - EmptyThisBuffer is where the frame-interval gate is, ahead of the split
//   between bound and unbound, so it applies here too: a frame whose pts is
//   less than PropertyGet_PlayFrameInterval() past the last one accepted is
//   refused. Unbound, the refusal comes back to the caller as a00f8010
//   (SYS_NOTREADY, borrowed), where a bound decoder never sees it. The card
//   patches that interval from 25000us to 8000us.
//
// - The render thread waits for a frame's pts only when AV sync is on and a
//   clock is bound. Neither is here, so a frame is shown as soon as the
//   thread takes it: pacing is the sender's job.
//
// - A frame the output is done with comes back through the channel callback
//   as MPP_EVENT_RELEASE_VIDEO_BUFFER, pointing at the output's own copy of
//   the VIDEO_FRAME_INFO_S that was sent.
//
// And what the goggle 2 then did with a 720p60 HEVC recording: every one of
// 601 frames over 10s accepted and shown, at 60.0fps, none late, the first
// on screen as it was sent. The frame from GetImage goes to SendFrame as it
// is, no copy, and looks right. Decoding averages 2.3ms a frame with spikes
// to 30ms, and runs at about 230fps when nothing waits for pts. Squeezing the
// pts to 4.2ms apart had exactly half refused with a00f8010, which is the
// patched 8ms gate at work. Starting at 60s put the exact frame up 155ms
// after the stepper was open.
#define VOSEND_errGATE 0xa00f8010

#define VOSEND_sendWAIT_MS 100
#define VOSEND_stopWAIT_MS 500

// How long the picture waits for the sound to start before going on without.
#define VOSEND_audioWAIT_MS 2000

#if defined HDZBOXPRO
#define VOSEND_WIDTH  1280
#define VOSEND_HEIGHT 720
#else
#define VOSEND_WIDTH  1920
#define VOSEND_HEIGHT 1080
#endif
#define VOSEND_uiCHN 2

// Send times by frame id, to measure how long the output holds a frame.
#define VOSEND_idSLOTS 64

typedef struct {
    int n;
    int64_t sum, min, max;
} Span_t;

typedef struct {
    FrameStep_t *fs;
    VO_DEV voDev;
    VO_LAYER voLayer;
    VO_CHN voChn;
    int uiLayer;

    pthread_mutex_t lock;
    int accepted; // SendFrame said yes
    int released; // the callback gave it back
    int64_t firstRenderUs;
    int64_t lastReleaseUs;
    Span_t releaseGap;
    int releaseHist[5]; // <12, 12-18, 18-25, 25-40, >=40 ms
    Span_t held;         // send to release
    struct {
        unsigned int id;
        int64_t us;
    } sentAt[VOSEND_idSLOTS];
    int sentNext;
} VoSend_t;

static int64_t vosend_nowUs(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void vosend_sleepUntil(int64_t us) {
    struct timespec ts = {.tv_sec = us / 1000000, .tv_nsec = (us % 1000000) * 1000};

    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

static void span_add(Span_t *s, int64_t v) {
    if (s->n == 0 || v < s->min) {
        s->min = v;
    }
    if (s->n == 0 || v > s->max) {
        s->max = v;
    }
    s->sum += v;
    s->n++;
}

static int64_t span_avg(const Span_t *s) {
    return s->n ? s->sum / s->n : 0;
}

const char *vosend_mode_name(int mode) {
    switch (mode) {
    case VOSEND_PACED: return "paced";
    case VOSEND_FREE: return "free";
    case VOSEND_GATE: return "gate";
    default: return "?";
    }
}

int vosend_mode_parse(const char *name) {
    for (int m = VOSEND_PACED; m <= VOSEND_GATE; m++) {
        if (strcmp(name, vosend_mode_name(m)) == 0) {
            return m;
        }
    }
    return -1;
}

static ERRORTYPE vosend_callback(void *cookie, MPP_CHN_S *chn, MPP_EVENT_TYPE event, void *data) {
    VoSend_t *vs = (VoSend_t *)cookie;
    const int64_t now = vosend_nowUs();

    if (chn->mModId != MOD_ID_VOU) {
        return SUCCESS;
    }

    switch (event) {
    case MPP_EVENT_RELEASE_VIDEO_BUFFER: {
        // The output's copy, which it reuses once this returns.
        VIDEO_FRAME_INFO_S frame = *(VIDEO_FRAME_INFO_S *)data;

        framestep_put_frame(vs->fs, &frame);

        pthread_mutex_lock(&vs->lock);
        vs->released++;
        if (vs->lastReleaseUs) {
            const int64_t gap = now - vs->lastReleaseUs;
            const int ms = (int)(gap / 1000);
            span_add(&vs->releaseGap, gap);
            vs->releaseHist[ms < 12 ? 0 : ms < 18 ? 1 : ms < 25 ? 2 : ms < 40 ? 3 : 4]++;
        }
        vs->lastReleaseUs = now;
        for (int i = 0; i < VOSEND_idSLOTS; i++) {
            if (vs->sentAt[i].us && vs->sentAt[i].id == frame.mId) {
                span_add(&vs->held, now - vs->sentAt[i].us);
                vs->sentAt[i].us = 0;
                break;
            }
        }
        pthread_mutex_unlock(&vs->lock);
        break;
    }

    case MPP_EVENT_RENDERING_START:
        pthread_mutex_lock(&vs->lock);
        vs->firstRenderUs = now;
        pthread_mutex_unlock(&vs->lock);
        break;

    default:
        LOGI("VS event %x", event);
        break;
    }

    return SUCCESS;
}

// The output the way MPP's player set it up (vdec2vo_createVoChn(), since
// removed), less the binding.
static bool vosend_openVo(VoSend_t *vs, int dispBufs) {
    VO_VIDEO_LAYER_ATTR_S layerAttr;
    ERRORTYPE ret;

    vs->voDev = 0;
    vs->uiLayer = HLAY(VOSEND_uiCHN, 0);

    AW_MPI_VO_Enable(vs->voDev);
    AW_MPI_VO_AddOutsideVideoLayer(vs->uiLayer);

    for (vs->voLayer = 0; vs->voLayer < VO_MAX_LAYER_NUM; vs->voLayer++) {
        if (AW_MPI_VO_EnableVideoLayer(vs->voLayer) == SUCCESS) {
            break;
        }
    }
    if (vs->voLayer >= VO_MAX_LAYER_NUM) {
        LOGE("VS no video layer");
        vs->voLayer = MM_INVALID_CHN;
        return false;
    }

    memset(&layerAttr, 0, sizeof(layerAttr));
    AW_MPI_VO_GetVideoLayerAttr(vs->voLayer, &layerAttr);
    layerAttr.stDispRect.X = 0;
    layerAttr.stDispRect.Y = 0;
    layerAttr.stDispRect.Width = VOSEND_WIDTH;
    layerAttr.stDispRect.Height = VOSEND_HEIGHT;
    layerAttr.enPixFormat = MM_PIXEL_FORMAT_YVU_PLANAR_420;
    AW_MPI_VO_SetVideoLayerAttr(vs->voLayer, &layerAttr);

    for (vs->voChn = 0; vs->voChn < VO_MAX_CHN_NUM; vs->voChn++) {
        ret = AW_MPI_VO_EnableChn(vs->voLayer, vs->voChn);
        if (ret == SUCCESS) {
            break;
        }
        if (ret != ERR_VO_CHN_NOT_DISABLE) {
            LOGE("VS enable vo channel %d: %x", vs->voChn, ret);
            vs->voChn = MM_INVALID_CHN;
            return false;
        }
    }
    if (vs->voChn >= VO_MAX_CHN_NUM) {
        vs->voChn = MM_INVALID_CHN;
        return false;
    }

    MPPCallbackInfo cb = {.cookie = vs, .callback = (MPPCallbackFuncType)&vosend_callback};
    AW_MPI_VO_RegisterCallback(vs->voLayer, vs->voChn, &cb);
    AW_MPI_VO_SetChnDispBufNum(vs->voLayer, vs->voChn, (unsigned int)dispBufs);

    ret = AW_MPI_VO_StartChn(vs->voLayer, vs->voChn);
    LOGI("VS vo layer %d chn %d, disp bufs %d, start %x", vs->voLayer, vs->voChn, dispBufs, ret);

    return ret == SUCCESS;
}

static void vosend_closeVo(VoSend_t *vs) {
    if (vs->voLayer == MM_INVALID_CHN) {
        return;
    }

    if (vs->voChn != MM_INVALID_CHN) {
        // Stopping is what gives back the frames the output still holds.
        const int64_t t0 = vosend_nowUs();
        const ERRORTYPE stop = AW_MPI_VO_StopChn(vs->voLayer, vs->voChn);
        int out;

        for (;;) {
            pthread_mutex_lock(&vs->lock);
            out = vs->accepted - vs->released;
            pthread_mutex_unlock(&vs->lock);
            if (out <= 0 || vosend_nowUs() - t0 > VOSEND_stopWAIT_MS * 1000) {
                break;
            }
            vosend_sleepUntil(vosend_nowUs() + 5000);
        }
        LOGI("VS stop %x, %d frames still out after %dms", stop, out,
             (int)((vosend_nowUs() - t0) / 1000));

        AW_MPI_VO_DisableChn(vs->voLayer, vs->voChn);

        pthread_mutex_lock(&vs->lock);
        out = vs->accepted - vs->released;
        pthread_mutex_unlock(&vs->lock);
        if (out > 0) {
            LOGW("VS %d frames never came back from the output", out);
        }
    }

    AW_MPI_VO_DisableVideoLayer(vs->voLayer);
    AW_MPI_VO_RemoveOutsideVideoLayer(vs->uiLayer);
    AW_MPI_VO_Disable(vs->voDev);
}

// When to show the frame with this pts: avoff after the sound reaches it.
// Waits for the sound to have a clock at all, up to the deadline; -1 when it
// did not get one.
static int64_t vosend_audioDue(ASend_t *as, const vosend_opts_t *opts, uint64_t pts,
                               int64_t deadlineUs) {
    for (;;) {
        const int64_t now = vosend_nowUs();
        int64_t heard;

        if (asend_audible(as, opts->clock, now, &heard)) {
            return now + ((int64_t)pts - heard) + (int64_t)opts->avoff_ms * 1000;
        }
        if (now > deadlineUs) {
            return -1;
        }
        vosend_sleepUntil(now + 2000);
    }
}

// Sound alone, for as long as asked, a line a second.
static bool vosend_audioOnly(const char *path, const vosend_opts_t *opts) {
    ASend_t *as = asend_open(path, opts->start_ms, opts->aac_raw);

    if (!as) {
        return false;
    }

    const int64_t t0 = vosend_nowUs();
    int64_t next = t0 + 1000000;

    for (;;) {
        const int64_t now = vosend_nowUs();

        if (now - t0 >= (int64_t)opts->run_ms * 1000) {
            break;
        }
        if (now >= next) {
            asend_log(as, now);
            next += 1000000;
        }
        vosend_sleepUntil(now + 20000);
        // Keeps ALSA's account fresh between the lines, as the picture would.
        int64_t heard;
        asend_audible(as, ASEND_CLOCK_PROC, vosend_nowUs(), &heard);
    }

    asend_log(as, vosend_nowUs());
    asend_close(as);
    return true;
}

bool vosend_run(const char *path, const vosend_opts_t *opts) {
    VoSend_t vs;
    MPP_SYS_CONF_S sysConf;
    const int mode = opts->mode;

    memset(&vs, 0, sizeof(vs));
    pthread_mutex_init(&vs.lock, NULL);
    vs.voLayer = MM_INVALID_CHN;
    vs.voChn = MM_INVALID_CHN;

    LOGI("VS begin file=%s mode=%s run_ms=%d start_ms=%d extra=%d bufs=%d video=%d audio=%d "
         "clock=%s avoff=%dms aac_raw=%d",
         path, vosend_mode_name(mode), opts->run_ms, opts->start_ms, opts->extra_frames,
         opts->disp_bufs, opts->video, opts->audio,
         opts->clock == ASEND_CLOCK_PROC ? "proc" : "start", opts->avoff_ms, opts->aac_raw);

    const int64_t tOpen = vosend_nowUs();

    memset(&sysConf, 0, sizeof(sysConf));
    sysConf.nAlignWidth = 32;
    AW_MPI_SYS_SetConf(&sysConf);
    AW_MPI_SYS_Init();

    if (opts->lineout_vol > 0) {
        char cmd[96];
        const int64_t t = vosend_nowUs();
        snprintf(cmd, sizeof(cmd), "amixer -q cset name='lineout volume' %d", opts->lineout_vol);
        const int ret = system(cmd);
        LOGI("VS lineout volume %d: %d, %dms", opts->lineout_vol, ret,
             (int)((vosend_nowUs() - t) / 1000));
    }

    if (!opts->video) {
        const bool ok = opts->audio && vosend_audioOnly(path, opts);
        AW_MPI_SYS_Exit();
        pthread_mutex_destroy(&vs.lock);
        return ok;
    }

    vs.fs = framestep_open(path, opts->extra_frames);
    if (!vs.fs) {
        LOGE("VS cannot open %s", path);
        AW_MPI_SYS_Exit();
        pthread_mutex_destroy(&vs.lock);
        return false;
    }

    const int rate = framestep_frame_rate(vs.fs); // x1000
    const int64_t frameUs = rate > 0 ? 1000000000LL / rate : 16667;

    if (!vosend_openVo(&vs, opts->disp_bufs)) {
        vosend_closeVo(&vs);
        framestep_close(vs.fs);
        AW_MPI_SYS_Exit();
        pthread_mutex_destroy(&vs.lock);
        return false;
    }

    const int64_t tReady = vosend_nowUs();

    int frames = 0, gated = 0, failed = 0, late = 0;
    ERRORTYPE lastErr = SUCCESS;
    Span_t dec = {0}, send = {0}, lateness = {0}, lag = {0}, sendGap = {0};
    int heldMax = 0;
    ASend_t *as = NULL;
    int64_t audioDeadline = 0;
    Span_t avStart = {0}, avProc = {0}; // frame pts less what was heard, when sent
    int64_t base = -1, lastSendUs = 0, tFirstSend = 0;
    uint64_t pts0 = 0, lastPts = 0;
    int64_t tBegin = 0;
    int firstMs = -1;

    // One line a second while it runs.
    int64_t secStart = 0;
    int secSent = 0, secGated = 0, secLate = 0;
    int64_t secDecMax = 0, secSendMax = 0;

    for (;;) {
        VIDEO_FRAME_INFO_S frame;
        int64_t t = vosend_nowUs();
        bool got;

        if (frames == 0 && opts->start_ms > 0) {
            got = framestep_get_frame_at(vs.fs, opts->start_ms, &frame);
        } else {
            got = framestep_get_frame(vs.fs, &frame);
        }
        const int64_t decUs = vosend_nowUs() - t;

        if (!got) {
            LOGI("VS no frame after %d, stopping", frames);
            break;
        }

        const uint64_t pts = frame.VFrame.mpts;

        if (frames == 0) {
            firstMs = (int)(pts / 1000);
            LOGI("VS first frame %dms, %ux%u fmt %d, strides %u/%u/%u, offsets t%d b%d l%d r%d, "
                 "phy %x/%x/%x, id %u, took %dms (open to ready %dms)",
                 firstMs, frame.VFrame.mWidth, frame.VFrame.mHeight, frame.VFrame.mPixelFormat,
                 frame.VFrame.mStride[0], frame.VFrame.mStride[1], frame.VFrame.mStride[2],
                 frame.VFrame.mOffsetTop, frame.VFrame.mOffsetBottom, frame.VFrame.mOffsetLeft,
                 frame.VFrame.mOffsetRight, frame.VFrame.mPhyAddr[0], frame.VFrame.mPhyAddr[1],
                 frame.VFrame.mPhyAddr[2], frame.mId, (int)(decUs / 1000),
                 (int)((tReady - tOpen) / 1000));
        } else {
            span_add(&dec, decUs);
        }

        if (base < 0) {
            base = vosend_nowUs();
            pts0 = pts;
            tBegin = base;
            secStart = base;
        }

        if (mode != VOSEND_FREE) {
            int64_t due = -1;

            if (as) {
                due = vosend_audioDue(as, opts, pts, audioDeadline);
                if (due < 0) {
                    LOGW("VS no sound clock after %dms, pacing by the wall clock",
                         VOSEND_audioWAIT_MS);
                    asend_log(as, vosend_nowUs());
                    asend_close(as);
                    as = NULL;
                    base = vosend_nowUs();
                    pts0 = pts;
                }
            }
            if (due < 0) {
                due = base + (int64_t)(pts - pts0);
            }
            const int64_t now = vosend_nowUs();

            if (now < due) {
                vosend_sleepUntil(due);
            } else {
                span_add(&lateness, now - due);
                if (now - due > frameUs) {
                    late++;
                    secLate++;
                }
            }
        }

        // Squeezed 4x, 60fps pts are 4.2ms apart: under the patched 8ms gate,
        // so about every other one should come back refused.
        const uint64_t sentPts = (mode == VOSEND_GATE) ? pts0 + (pts - pts0) / 4 : pts;
        frame.VFrame.mpts = sentPts;

        t = vosend_nowUs();
        const ERRORTYPE ret = AW_MPI_VO_SendFrame(vs.voLayer, vs.voChn, &frame, VOSEND_sendWAIT_MS);
        const int64_t sendUs = vosend_nowUs() - t;

        span_add(&send, sendUs);
        frames++;
        if (sendUs > secSendMax) {
            secSendMax = sendUs;
        }
        if (frames > 1 && decUs > secDecMax) {
            secDecMax = decUs;
        }

        if (as) {
            int64_t heard;
            if (asend_audible(as, ASEND_CLOCK_START, t, &heard)) {
                span_add(&avStart, (int64_t)pts - heard);
            }
            if (asend_audible(as, ASEND_CLOCK_PROC, t, &heard)) {
                span_add(&avProc, (int64_t)pts - heard);
            }
        }

        if (ret == SUCCESS) {
            pthread_mutex_lock(&vs.lock);
            vs.accepted++;
            vs.sentAt[vs.sentNext].id = frame.mId;
            vs.sentAt[vs.sentNext].us = t;
            vs.sentNext = (vs.sentNext + 1) % VOSEND_idSLOTS;
            const int out = vs.accepted - vs.released;
            pthread_mutex_unlock(&vs.lock);
            if (out > heldMax) {
                heldMax = out;
            }
            if (lastSendUs) {
                span_add(&sendGap, t - lastSendUs);
            } else {
                tFirstSend = t;
            }
            lastSendUs = t;
            secSent++;
        } else {
            if ((unsigned int)ret == VOSEND_errGATE) {
                gated++;
                secGated++;
            } else {
                failed++;
                lastErr = ret;
                if (failed <= 5) {
                    LOGW("VS send %x for %dms", ret, (int)(pts / 1000));
                }
            }
            framestep_put_frame(vs.fs, &frame);
        }

        // The first picture is up: now the sound, from that picture on.
        if (frames == 1 && opts->audio) {
            as = asend_open(path, firstMs, opts->aac_raw);
            audioDeadline = vosend_nowUs() + VOSEND_audioWAIT_MS * 1000;
            if (!as) {
                LOGW("VS no sound, pacing by the wall clock");
            }
        }

        {
            uint64_t shownUs = 0;
            if (AW_MPI_VO_GetChnPts(vs.voLayer, vs.voChn, &shownUs) == SUCCESS && shownUs) {
                span_add(&lag, (int64_t)sentPts - (int64_t)shownUs);
            }
            lastPts = shownUs;
        }

        const int64_t now = vosend_nowUs();

        if (now - secStart >= 1000000) {
            pthread_mutex_lock(&vs.lock);
            const int out = vs.accepted - vs.released;
            const int released = vs.released;
            pthread_mutex_unlock(&vs.lock);

            LOGI("VS sec=%d sent=%d gated=%d late=%d out=%d released=%d dec_max=%dus "
                 "send_max=%dus at=%dms shown=%dms",
                 (int)((now - tBegin) / 1000000), secSent, secGated, secLate, out, released,
                 (int)secDecMax, (int)secSendMax, (int)(pts / 1000), (int)(lastPts / 1000));
            if (as) {
                asend_log(as, now);
            }
            secStart = now;
            secSent = secGated = secLate = 0;
            secDecMax = secSendMax = 0;
        }

        if (now - tBegin >= (int64_t)opts->run_ms * 1000) {
            break;
        }
    }

    const int64_t tEnd = vosend_nowUs();

    // Everything the callback wrote, read once it can no longer change much.
    pthread_mutex_lock(&vs.lock);
    const int accepted = vs.accepted;
    const int released = vs.released;
    const int64_t firstRenderUs = vs.firstRenderUs;
    const Span_t relGap = vs.releaseGap;
    const Span_t held = vs.held;
    int hist[5];
    memcpy(hist, vs.releaseHist, sizeof(hist));
    pthread_mutex_unlock(&vs.lock);

    if (as) {
        asend_log(as, vosend_nowUs());
        asend_close(as);
    }
    vosend_closeVo(&vs);
    framestep_close(vs.fs);
    AW_MPI_SYS_Exit();
    pthread_mutex_destroy(&vs.lock);

    const int64_t runUs = tEnd - tBegin;
    const int64_t sendSpanUs = lastSendUs - tFirstSend;

    LOGI("VS summary file=%s mode=%s start=%dms first=%dms frames=%d accepted=%d gated=%d "
         "failed=%d last_err=%x run=%dms",
         path, vosend_mode_name(mode), opts->start_ms, firstMs, frames, accepted, gated, failed,
         lastErr, (int)(runUs / 1000));
    LOGI("VS summary fps_sent=%d.%d fps_released=%d.%d send_gap min/avg/max=%d/%d/%dus",
         sendSpanUs > 0 ? (int)((int64_t)(accepted - 1) * 10000000 / sendSpanUs) / 10 : 0,
         sendSpanUs > 0 ? (int)((int64_t)(accepted - 1) * 10000000 / sendSpanUs) % 10 : 0,
         relGap.n && relGap.sum ? (int)((int64_t)relGap.n * 10000000 / relGap.sum) / 10 : 0,
         relGap.n && relGap.sum ? (int)((int64_t)relGap.n * 10000000 / relGap.sum) % 10 : 0,
         (int)sendGap.min, (int)span_avg(&sendGap), (int)sendGap.max);
    LOGI("VS summary release_gap min/avg/max=%d/%d/%dus hist(<12,12-18,18-25,25-40,40+ms)="
         "%d/%d/%d/%d/%d held min/avg/max=%d/%d/%dus out_max=%d released=%d",
         (int)relGap.min, (int)span_avg(&relGap), (int)relGap.max, hist[0], hist[1], hist[2],
         hist[3], hist[4], (int)held.min, (int)span_avg(&held), (int)held.max, heldMax, released);
    LOGI("VS summary decode min/avg/max=%d/%d/%dus send min/avg/max=%d/%d/%dus late=%d "
         "lateness avg/max=%d/%dus lag avg/max=%d/%dus",
         (int)dec.min, (int)span_avg(&dec), (int)dec.max, (int)send.min, (int)span_avg(&send),
         (int)send.max, late, (int)span_avg(&lateness), (int)lateness.max, (int)span_avg(&lag),
         (int)lag.max);
    if (opts->audio) {
        LOGI("VS summary av (pts - heard at send) start min/avg/max=%d/%d/%dms n=%d, "
             "proc min/avg/max=%d/%d/%dms n=%d",
             (int)(avStart.min / 1000), (int)(span_avg(&avStart) / 1000),
             (int)(avStart.max / 1000), avStart.n, (int)(avProc.min / 1000),
             (int)(span_avg(&avProc) / 1000), (int)(avProc.max / 1000), avProc.n);
    }
    LOGI("VS summary first_render=%dms after the first send, open_to_first_send=%dms",
         firstRenderUs && tFirstSend ? (int)((firstRenderUs - tFirstSend) / 1000) : -1,
         tFirstSend ? (int)((tFirstSend - tOpen) / 1000) : -1);

    return accepted > 0;
}
