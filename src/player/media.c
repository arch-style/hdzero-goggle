#include "media.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// #define LOG_NDEBUG 0
#define LOG_TAG "play"
#include <log/log.h>

#include "adec2ao.h"
#include "awdmx.h"
#include "gogglemsg.h"
#include "vdec2vo.h"
#include "version.h"

#ifdef EVB_DEBUG
#define PLAY_HDZERO 0
#else
#define PLAY_HDZERO 1
#endif

#if (PLAY_HDZERO)
#define VO_intfTYPE VO_INTF_BT1120
// Inert, whatever it is set to. The goggle's display runs on VDPO, and MPP has
// no VO_INTF_* that maps to it, so AW_MPI_VO_SetPubAttr always fails the same
// way ("Inout disp_type:0 error!" -> a00f8008, VO/NOT_SUPPORT). Measured with
// both NTSC and 720P60. The output timing comes from `dispw -s vdpo 720p60`.
#define VO_intfSYNC VO_OUTPUT_NTSC
#define VO_uiCHN    2
#if defined HDZGOGGLE
#define VO_WIDTH  1920
#define VO_HEIGHT 1080
#elif defined HDZBOXPRO
#define VO_WIDTH  1280
#define VO_HEIGHT 720
#elif defined HDZGOGGLE2
// Matches the UI display Display_UI_init() sets up, so the recordings are
// scaled up 1.5x from 720p by the display engine. 1280x720 was measured and
// the layer was correct and ran at a true 60fps, but the framebuffer half of
// the display cannot follow it -- see the note in Display_UI_init().
#define VO_WIDTH  1920
#define VO_HEIGHT 1080
#endif
#else
#define VO_intfTYPE VO_INTF_LCD
#define VO_intfSYNC VO_OUTPUT_NTSC
#define VO_WIDTH    720
#define VO_HEIGHT   1280
#define VO_uiCHN    2
#endif

#define PLAY_statIDLE      0
#define PLAY_statOPENED    BIT(0)
#define PLAY_statSTARTED   BIT(1)
#define PLAY_statPAUSED    BIT(2)
#define PLAY_statCOMPLETED BIT(3)

#define PLAY_statSTEPPING BIT(4) // chain running, clock stopped
#define PLAY_statMANUAL   BIT(5) // vdec unbound from vo, app hands over frames

#define PLAY_statSEEKING BIT(16)

typedef struct
{
    Vdec2VoContext_t *vv;
    Adec2AoContext_t *aa;
    AwdmxContext_t *dmx;
    uint32_t state;
    pthread_mutex_t mutex;

    int playingTime; // ms
} PlayContext_t;

static void play_manual_exit(PlayContext_t *playCtx);

static int play_start(PlayContext_t *playCtx) {
    int ret = vdec2vo_start(playCtx->vv);

    if (ret == SUCCESS) {
        ret = adec2ao_start(playCtx->aa);
        ret = awdmx_start(playCtx->dmx);
    }

    playCtx->state &= ~(PLAY_statPAUSED | PLAY_statCOMPLETED | PLAY_statSTEPPING);
    playCtx->state |= PLAY_statSTARTED;

    return ret;
}

static int play_pause(PlayContext_t *playCtx) {
    int ret = awdmx_pause(playCtx->dmx);

    if (ret == SUCCESS) {
        ret = adec2ao_pause(playCtx->aa);
        ret = vdec2vo_pause(playCtx->vv);
    }

    playCtx->state &= ~PLAY_statSTEPPING;
    playCtx->state |= PLAY_statPAUSED;

    return ret;
}

static int play_stop(PlayContext_t *playCtx) {
    int ret = awdmx_stop(playCtx->dmx);

    if (ret == SUCCESS) {
        ret = adec2ao_stop(playCtx->aa);
        ret = vdec2vo_stop(playCtx->vv);
    }

    return ret;
}

static int play_seekto(PlayContext_t *playCtx, int seekTime) {
    int ret;

    ret = awdmx_seekTo(playCtx->dmx, seekTime);

    if (ret == SUCCESS) {
        ret = adec2ao_seekTo(playCtx->aa);
        ret = vdec2vo_seekTo(playCtx->vv);
    }

    return ret;
}

static void play_moveStatus(PlayContext_t *playCtx) {
    if (!(playCtx->state & PLAY_statSTARTED)) {
        return;
    }

    if (!(playCtx->state & PLAY_statCOMPLETED)) {
        if (awdmx_isEOF(playCtx->dmx)) {
            vdec2vo_checkEof(playCtx->vv);
            adec2ao_checkEof(playCtx->aa);
        }

        if (vdec2vo_isEOF(playCtx->vv)) {
            play_stop(playCtx);
            playCtx->state |= PLAY_statCOMPLETED;
        }
    }
}

static void play_onDemuxEof(void *context) {
    PlayContext_t *playCtx = (PlayContext_t *)context;
    vdec2vo_setVdecEof(playCtx->vv);
    adec2ao_setAdecEof(playCtx->aa);
}

void *thread_media(void *params) {
    media_info_t info;
    media_t *media = (media_t *)params;
    PlayContext_t *playCtx = (PlayContext_t *)media->context;
    notify_cb_t notify = media->notify;
    for (;;) {
        if (!media)
            break;

        if (media->is_media_thread_exit)
            break;

        pthread_mutex_lock(&playCtx->mutex);
        play_moveStatus(playCtx);
        if (!vdec2vo_isEOF(playCtx->vv)) {
            vdec2vo_currentMediaTime(playCtx->vv, &playCtx->playingTime);
        } else {
            playCtx->playingTime = -2;
        }
        pthread_mutex_unlock(&playCtx->mutex);

        info.playing_time = playCtx->playingTime;
        info.duration = playCtx->dmx->msDuration;
        if (!media->is_media_thread_exit)
            notify(&info);
        usleep(100000);
    }
    return NULL;
}

void media_control(media_t *media, player_cmd_t *cmd) {
    PlayContext_t *playCtx = (PlayContext_t *)media->context;

    pthread_mutex_lock(&playCtx->mutex);

    // The manual frame path holds the decoder and the output apart. Anything
    // the rest of the player asks for wants them bound again.
    bool wasManual = (playCtx->state & PLAY_statMANUAL) != 0;
    play_manual_exit(playCtx);

    switch (cmd->opt) {
    case PLAYER_START:
        // Stopping for the unbind left the demuxer at the start of the file.
        if (wasManual && playCtx->playingTime > 0) {
            play_seekto(playCtx, playCtx->playingTime);
        }
        play_start(playCtx);
        break;
    case PLAYER_STOP:
        play_stop(playCtx);
        break;
    case PLAYER_PAUSE:
        play_pause(playCtx);
        break;
    case PLAYER_SEEK:
        play_seekto(playCtx, cmd->params);
        break;

    default:
        break;
    }

    pthread_mutex_unlock(&playCtx->mutex);
}

// How long one frame lasts, in ms. The SoC's player has neither a play-rate
// call nor a frame-step call, so the UI paces frames itself and has to know
// this. Files the demuxer gives no rate for fall back to 30fps.
#define MEDIA_defFRAME_MS 33

// Stepping a paused picture, one decoded frame at a time.
//
// Three timing-based attempts failed on hardware before this one, each in a
// way worth keeping written down, because the API reads as though any of them
// would work:
//
//   Pausing the whole chain between steps stopped the picture, but only the
//   first step after real playback showed anything: restarting the decoder
//   takes longer than the frame the step waited for, so time moved and the
//   picture did not.
//
//   Pausing the clock alone kept the decoder hot and did not stop the picture
//   at all: the file played itself away. The clock is not the gate on
//   presentation that mpi_clock.h reads as.
//
//   Pausing the output alone stopped and released the picture correctly, but
//   a step could only be expressed as a length of time, and the release and
//   hold cost about a frame by themselves, so every step moved two.
//
// None of them could express "one frame" because, with VDEC bound to VO, no
// frame ever passes through this program. So the step path takes that binding
// out and does the handover itself: one frame from the decoder, one frame to
// the output. Normal playback still runs on the bound pipeline and is not
// touched; the binding is restored on play, pause, seek and exit.
#define PLAY_stepPOLL_US 500

// Video only: audio stays paused while stepping, so the demuxer feeds the
// decoder and nothing reaches the speakers.
//
// The unbinding has to happen with the components stopped. Taking the binding
// out while they ran was accepted (SUCCESS from both UnBind calls) but changed
// nothing: the decoder kept feeding the output, so a step played on as usual,
// AW_MPI_VDEC_GetImage answered NOT_PERM because the channel was still in
// tunnel mode, and binding it back afterwards answered NOT_SUPPORT. Tunnel
// mode is settled when a component starts, so the sequence is stop, unbind,
// start again.
//
// Stopping loses the position, so it is taken before and seeked back to after.
static void play_manual_enter(PlayContext_t *playCtx) {
    if (playCtx->state & PLAY_statMANUAL) {
        return;
    }

    int pos = playCtx->playingTime;

    play_stop(playCtx);
    vdec2vo_manualEnter(playCtx->vv);

    awdmx_start(playCtx->dmx);  // the demuxer has to keep feeding the decoder
    vdec2vo_start(playCtx->vv); // and the decoder has to keep decoding

    if (pos > 0) {
        play_seekto(playCtx, pos);
        playCtx->playingTime = pos;
    }

    playCtx->state |= PLAY_statMANUAL;
    playCtx->state &= ~(PLAY_statPAUSED | PLAY_statCOMPLETED);
    playCtx->state |= PLAY_statSTARTED;

    LOGI("manual mode on at %dms", pos);
}

static void play_manual_exit(PlayContext_t *playCtx) {
    if (!(playCtx->state & PLAY_statMANUAL)) {
        return;
    }

    int pos = playCtx->playingTime;

    play_stop(playCtx);
    vdec2vo_manualExit(playCtx->vv);

    playCtx->state &= ~PLAY_statMANUAL;
    playCtx->state |= PLAY_statPAUSED;
    playCtx->playingTime = pos;

    LOGI("manual mode off at %dms", pos);
}

// Advance one frame and hold it. Returns the media time now on the screen.
uint32_t media_step_frame(media_t *media, uint32_t frame_ms) {
    (void)frame_ms; // a frame is a frame; no rate is involved any more

    if (!media) {
        return 0;
    }

    PlayContext_t *playCtx = (PlayContext_t *)media->context;
    int pts = -1;
    ERRORTYPE ret;

    pthread_mutex_lock(&playCtx->mutex);

    play_manual_enter(playCtx);
    ret = vdec2vo_showNextFrame(playCtx->vv, &pts);

    if (ret == SUCCESS && pts >= 0) {
        playCtx->playingTime = pts;
    }

    pthread_mutex_unlock(&playCtx->mutex);

    LOGD("step: %s, now %dms", ret == SUCCESS ? "shown" : "no frame", playCtx->playingTime);

    return (uint32_t)playCtx->playingTime;
}

// Seek, and leave a frame on the screen. Stays on the manual path: the
// demuxer runs there too, so the seek is the ordinary one, and the frame that
// comes out of the decoder afterwards is put up by hand.
uint32_t media_seek_show(media_t *media, uint32_t seekto, uint32_t frame_ms) {
    if (!media) {
        return 0;
    }

    PlayContext_t *playCtx = (PlayContext_t *)media->context;
    int pts = -1;
    ERRORTYPE ret;

    (void)frame_ms;

    pthread_mutex_lock(&playCtx->mutex);

    play_manual_enter(playCtx);
    play_seekto(playCtx, (int)seekto);
    playCtx->playingTime = (int)seekto;

    ret = vdec2vo_showNextFrame(playCtx->vv, &pts);

    if (ret == SUCCESS && pts >= 0) {
        playCtx->playingTime = pts;
    }

    pthread_mutex_unlock(&playCtx->mutex);

    LOGD("seek: asked %ums, showing %dms", seekto, playCtx->playingTime);

    return (uint32_t)playCtx->playingTime;
}

uint32_t media_frame_interval_ms(media_t *media) {
    if (!media) {
        return MEDIA_defFRAME_MS;
    }

    PlayContext_t *playCtx = (PlayContext_t *)media->context;
    int rate = playCtx->dmx ? playCtx->dmx->frameRate : 0; // x1000

    if (rate < 1000) {
        return MEDIA_defFRAME_MS;
    }

    uint32_t ms = 1000000u / (uint32_t)rate;

    return ms ? ms : 1;
}

media_t *media_instantiate(char *filename, notify_cb_t notify) {
    int ret = 0;
    pthread_t pid;

    PlayContext_t *playCtx = calloc(1, sizeof(PlayContext_t));
    if (!playCtx)
        return NULL;

    media_t *media = calloc(1, sizeof(media_t));
    if (!media)
        return NULL;
    media->context = playCtx;
    media->notify = notify;

    pthread_mutex_init(&playCtx->mutex, NULL);

    playCtx->vv = vdec2vo_initSys();
    if (playCtx->vv == NULL) {
        LOGE("create vdec2vo failed");
        goto failed;
    }

    playCtx->aa = adec2ao_initSys();
    if (playCtx->aa == NULL) {
        LOGE("create adec2ao failed");
        goto failed;
    }

    playCtx->dmx = awdmx_open(filename, play_onDemuxEof, playCtx);
    if (playCtx->dmx == NULL) {
        LOGE("open demux failed");
        goto failed;
    } else {
        Vdec2VoParams_t vvParams;
        memset(&vvParams, 0, sizeof(vvParams));

        vvParams.initRotation = 0;
        vvParams.pixelFormat = MM_PIXEL_FORMAT_YVU_PLANAR_420;

        vvParams.vdec.codecType = playCtx->dmx->codecType;
        vvParams.vdec.width = playCtx->dmx->width;
        vvParams.vdec.height = playCtx->dmx->height;

        vvParams.vo.width = VO_WIDTH;
        vvParams.vo.height = VO_HEIGHT;
        vvParams.vo.intfType = VO_intfTYPE;
        vvParams.vo.intfSync = VO_intfSYNC;
        vvParams.vo.uiChn = VO_uiCHN;

        vvParams.clkChn = playCtx->dmx->clkChn;
        vvParams.dmxChn = playCtx->dmx->dmxChn;

        ret = vdec2vo_prepare(playCtx->vv, &vvParams);

        if (playCtx->dmx->audioNum > 0) {
            AdecParams_t aaParams;
            memset(&aaParams, 0, sizeof(aaParams));
            aaParams.clkChn = playCtx->dmx->clkChn;
            aaParams.dmxChn = playCtx->dmx->dmxChn;
            aaParams.codecType = playCtx->dmx->aCodecType;
            aaParams.bitsPerSample = playCtx->dmx->bitsPerSample;
            aaParams.channels = playCtx->dmx->channels;
            aaParams.sampleRate = playCtx->dmx->sampleRate;
            ret = adec2ao_prepare(playCtx->aa, &aaParams);
        }
    }
    if (ret != 0) {
        LOGE("prepare vdec2vo failed");
        goto failed;
    }

    LOGE("ready to play");
    media->is_media_thread_exit = false;
    ret = pthread_create(&pid, NULL, thread_media, (void *)media);
    if (ret) {
        LOGE("create thread media failed, exit");
        return NULL;
    }
    media->pid = pid;
    return media;
failed:
    adec2ao_deinitSys(playCtx->aa);
    vdec2vo_deinitSys(playCtx->vv);
    awdmx_close(playCtx->dmx);
    pthread_mutex_destroy(&playCtx->mutex);
    free(playCtx);
    LOGD("exit done");

    return NULL;
}

void media_exit(media_t *media) {
    assert(media);
    media->is_media_thread_exit = true;
    pthread_join(media->pid, NULL);

    PlayContext_t *playCtx = (PlayContext_t *)media->context;
    play_manual_exit(playCtx);
    play_stop(playCtx);
    adec2ao_deinitSys(playCtx->aa);
    vdec2vo_deinitSys(playCtx->vv);
    awdmx_close(playCtx->dmx);
    pthread_mutex_destroy(&playCtx->mutex);
    free(media->context);
    free(media);
}
