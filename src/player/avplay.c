#include "avplay.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

struct AvPlay {
    FrameStep_t *fs;
    ASend_t *as; // NULL when the file has no sound

    VO_DEV voDev;
    VO_LAYER voLayer;
    VO_CHN voChn;
    int uiLayer;
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

// The way vdec2vo_createVoChn() set it up for MPP's player, less the binding.
static bool avplay_openVo(AvPlay_t *ap) {
    VO_VIDEO_LAYER_ATTR_S layerAttr;
    ERRORTYPE ret = FAILURE;

    ap->voDev = 0;
    ap->uiLayer = HLAY(AVPLAY_uiCHN, 0);

    AW_MPI_VO_Enable(ap->voDev);
    AW_MPI_VO_AddOutsideVideoLayer(ap->uiLayer);

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
    layerAttr.stDispRect.Width = AVPLAY_WIDTH;
    layerAttr.stDispRect.Height = AVPLAY_HEIGHT;
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

// Puts a frame from the decoder on screen, and it is then the position.
// The frame goes to the output either way.
static bool avplay_show(AvPlay_t *ap, VIDEO_FRAME_INFO_S *frame) {
    const int ms = (int)(frame->VFrame.mpts / 1000);

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

    ap->fs = framestep_open_ex(path, AVPLAY_extraFRAMES);
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
        got = framestep_get_prev_frame(ap->fs, ms + 60000, &frame) && avplay_show(ap, &frame);
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
