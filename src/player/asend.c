#include "asend.h"

#include <errno.h>
#include <glob.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "asend"
#include <log/log.h>

#include <mpi_adec.h>
#include <mpi_ao.h>
#include <mpi_sys.h>

#include <libavformat/avformat.h>

// What was read out of libmedia_mpp.so before writing this:
//
// - AW_MPI_AO_SendFrame() queues the AUDIO_FRAME_S itself, not a copy of the
//   samples, on a list with no limit, and returns at once; its timeout is
//   never used. The output's thread takes frames in order and copies them
//   into a ring of its own (audioHw_AO_FillPcmRingBuf, which waits while the
//   ring is full), and only then hands the frame back, through the channel
//   callback as MPP_EVENT_RELEASE_AUDIO_BUFFER with the output's copy of it.
//   A second thread moves the ring into ALSA, in 1024-frame periods.
//
// - The output waits for nothing when no clock is bound. With one bound it
//   would set that clock from each frame's pts as it goes into the ring,
//   which is how MPP's player keeps time: ahead of what can be heard by the
//   ring and ALSA's buffer.
//
// - AW_MPI_ADEC_GetFrame() refuses a channel in tunnel mode and otherwise
//   hands out decoded frames until they are released.
//
// - The AAC decoder finds ADTS sync words itself (AACFindSyncWord), and
//   ADEC_CHN_ATTR_S.attachAACHeader makes libadecoder build a header for bare
//   frames (AdCedarBuildAACPacketHdr).
//
// So when the sound is actually heard is past anything the output reports,
// by however much sits in its ring and in ALSA. ALSA itself says how far it
// has got, in /proc/asound/.../status, which is what ASEND_CLOCK_PROC reads.
//
// Measured on the goggle 2: ALSA is opened with 1024-frame periods and an
// 8192-frame buffer, starts on the first write, and is kept nearly full, so
// the output hands a frame back about 160ms before it is heard. The two
// clocks agreed within 1ms over 15s: counting from the first frame handed
// back is as good as ALSA's own count. A picture paced by either went out
// within 3ms of it, and a beep recorded with a flash was heard with it.
// Opening all this costs 230-800ms, most of it libavformat.

#define ASEND_sendWAIT_MS 20
#define ASEND_idleUS 5000
#define ASEND_stopWAIT_MS 500
#define ASEND_procEVERY_US 20000

// How far short of a play_from() position the reader seeks. A transport
// stream seek lands roughly, sometimes after the point asked for; landing
// short and reading forward to it costs some reading, landing past it would
// start the sound late.
#define ASEND_seekSHORT_MS 500

// What counts as a frame from the position asked for, rather than one decoded
// before the jump.
#define ASEND_freshBEFORE_MS 100
#define ASEND_freshAFTER_MS 1000

// Frames handed to the output and not yet back: to time the ring, and to give
// back to the decoder whatever the output never returns.
#define ASEND_SLOTS 64

typedef struct {
    int n;
    int64_t sum, min, max;
} ASpan_t;

struct ASend {
    AVFormatContext *fmt;
    int stream;
    AVPacket *pkt;
    bool pktPending;
    bool eof;
    int startMs;
    bool aacRaw;
    int rate, channels;

    // Bare AAC frames (mp4) given ADTS headers here, from the AAC
    // configuration in the container, so that the decoder takes them the way
    // it takes a transport stream's. Having it add its own headers
    // (attachAACHeader) played until the first resume and then went silent:
    // after AW_MPI_ADEC_Seek() it gave 16 more frames and no more.
    bool adtsWrap;
    int adtsProfile, adtsFreq, adtsChan;
    uint8_t *wrapBuf;
    size_t wrapCap;

    ADEC_CHN adecChn;
    AUDIO_DEV aoDev;
    AO_CHN aoChn;

    pthread_t thread;
    bool threadRunning;
    volatile bool stop;

    // Playing or idle. The thread only touches the decoder and the output
    // while playing, and asend_pause() waits for it to be idle before
    // anything else does.
    pthread_cond_t cond;
    bool playing;
    bool idle;
    bool started; // the decoder and the output have been started once
    int plays;

    // From an asend_play_from() until the first frame from there: frames
    // decoded before it are let go unplayed, and the clock waits for one from
    // there to start.
    int targetMs;
    bool fresh;
    bool clockArmed;
    int staleDropped;

    // The end of the last frame handed to the output, to catch one that does
    // not follow on from it: sound heard twice, or skipped.
    int64_t sentEndUs;
    int jumps;
    int64_t lastFrameUs; // when the decoder last gave a frame, this play
    bool stallLogged;
    int64_t eofUs;       // when the reader ran out, this play

    // Written by the feeding thread and the output's callback.
    pthread_mutex_t lock;
    int packetsIn, framesOut, framesSent, framesBack, sendErrs;
    ERRORTYPE lastSendErr;
    int64_t firstBackUs;   // monotonic, when the output took the first frame since play
    int64_t firstPtsUs;    // that frame's pts
    int64_t backEndPtsUs;  // the end of the last frame handed back
    int64_t samplesBack;   // samples handed back, all told
    int64_t lastBackUs;
    ASpan_t backGap;
    ASpan_t inRing;        // send to release
    struct {
        AUDIO_FRAME_S frame;
        int64_t us;
        bool out;
    } sentAt[ASEND_SLOTS];
    int sentNext;
    int described;

    // ALSA's account, read by whoever asks for the clock.
    char procPath[128];
    bool procFound;
    int64_t procReadUs;
    int64_t procHwPtr, procApplPtr, procDelay;
    char procState[16];
    int64_t procRunningUs; // monotonic, when it was first seen running
    int procReads;
};

static int64_t asend_nowUs(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void aspan_add(ASpan_t *s, int64_t v) {
    if (s->n == 0 || v < s->min) {
        s->min = v;
    }
    if (s->n == 0 || v > s->max) {
        s->max = v;
    }
    s->sum += v;
    s->n++;
}

static int64_t aspan_avg(const ASpan_t *s) {
    return s->n ? s->sum / s->n : 0;
}

// Samples in a decoded frame. mLen is taken as bytes for all channels, which
// the first frames' timestamps are logged to check.
static int asend_frameSamples(const AUDIO_FRAME_S *f) {
    const int bytes = (int)f->mBitwidth + 1; // AUDIO_BIT_WIDTH_16 is 1
    const int channels = f->mSoundmode == AUDIO_SOUND_MODE_STEREO ? 2 : 1;

    if (bytes <= 0 || f->mLen == 0) {
        return 1024;
    }
    return (int)f->mLen / (bytes * channels);
}

///////////////////////////////////////////////////////////////////////////////
// ALSA's own account, from procfs.

static bool asend_procParse(const char *buf, const char *key, int64_t *out) {
    const char *p = strstr(buf, key);
    long long v;

    if (!p || !(p = strchr(p, ':')) || sscanf(p + 1, "%lld", &v) != 1) {
        return false;
    }
    *out = v;
    return true;
}

// Finds the playback substream that is open. The output opens ALSA when it
// starts, so this is looked for lazily.
static bool asend_procFind(ASend_t *as) {
    glob_t g;
    char buf[512];

    if (as->procFound) {
        return true;
    }
    if (glob("/proc/asound/card*/pcm*p/sub*/status", 0, NULL, &g) != 0) {
        return false;
    }
    for (size_t i = 0; i < g.gl_pathc; i++) {
        FILE *fp = fopen(g.gl_pathv[i], "r");
        size_t n = 0;

        if (fp) {
            n = fread(buf, 1, sizeof(buf) - 1, fp);
            fclose(fp);
        }
        buf[n] = 0;
        if (n > 0 && strncmp(buf, "closed", 6) != 0) {
            snprintf(as->procPath, sizeof(as->procPath), "%s", g.gl_pathv[i]);
            as->procFound = true;

            // hw_params too, once: the buffer and period ALSA was given.
            char hw[160];
            snprintf(hw, sizeof(hw), "%s", g.gl_pathv[i]);
            char *slash = strrchr(hw, '/');
            if (slash) {
                snprintf(slash, sizeof(hw) - (size_t)(slash - hw), "/hw_params");
                fp = fopen(hw, "r");
                if (fp) {
                    n = fread(buf, 1, sizeof(buf) - 1, fp);
                    fclose(fp);
                    buf[n] = 0;
                    for (char *c = buf; *c; c++) {
                        if (*c == '\n') {
                            *c = ' ';
                        }
                    }
                    LOGI("VS audio alsa %s hw_params: %s", hw, buf);
                }
            }
            break;
        }
    }
    globfree(&g);
    return as->procFound;
}

static bool asend_procRead(ASend_t *as, int64_t now) {
    char buf[512];
    FILE *fp;
    size_t n;

    if (!asend_procFind(as)) {
        return false;
    }
    fp = fopen(as->procPath, "r");
    if (!fp) {
        return false;
    }
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = 0;

    if (sscanf(buf, "state: %15s", as->procState) != 1) {
        snprintf(as->procState, sizeof(as->procState), "%.15s", buf);
    }
    asend_procParse(buf, "hw_ptr", &as->procHwPtr);
    asend_procParse(buf, "appl_ptr", &as->procApplPtr);
    asend_procParse(buf, "delay", &as->procDelay);
    as->procReadUs = now;
    as->procReads++;

    if (!as->procRunningUs && strcmp(as->procState, "RUNNING") == 0) {
        as->procRunningUs = now;
        for (char *c = buf; *c; c++) {
            if (*c == '\n') {
                *c = ' ';
            }
        }
        LOGI("VS audio alsa running: %s", buf);
    }
    return true;
}

static bool asend_isFresh(const ASend_t *as, int64_t tsUs) {
    const int ms = (int)(tsUs / 1000);
    return ms >= as->targetMs - ASEND_freshBEFORE_MS && ms <= as->targetMs + ASEND_freshAFTER_MS;
}

///////////////////////////////////////////////////////////////////////////////
// The output hands frames back here.

static ERRORTYPE asend_callback(void *cookie, MPP_CHN_S *chn, MPP_EVENT_TYPE event, void *data) {
    ASend_t *as = (ASend_t *)cookie;
    const int64_t now = asend_nowUs();

    if (chn->mModId != MOD_ID_AO) {
        return SUCCESS;
    }
    if (event != MPP_EVENT_RELEASE_AUDIO_BUFFER) {
        LOGI("VS audio event %x", event);
        return SUCCESS;
    }

    AUDIO_FRAME_S frame = *(AUDIO_FRAME_S *)data;
    const int samples = asend_frameSamples(&frame);

    AW_MPI_ADEC_ReleaseFrame(as->adecChn, &frame);

    pthread_mutex_lock(&as->lock);
    if (as->clockArmed && asend_isFresh(as, (int64_t)frame.mTimeStamp)) {
        as->clockArmed = false;
        as->firstBackUs = now;
        as->firstPtsUs = (int64_t)frame.mTimeStamp;
    }
    as->framesBack++;
    as->samplesBack += samples;
    as->backEndPtsUs = (int64_t)frame.mTimeStamp + (int64_t)samples * 1000000 / as->rate;
    if (as->lastBackUs) {
        aspan_add(&as->backGap, now - as->lastBackUs);
    }
    as->lastBackUs = now;
    for (int i = 0; i < ASEND_SLOTS; i++) {
        if (as->sentAt[i].out && as->sentAt[i].frame.mId == frame.mId) {
            aspan_add(&as->inRing, now - as->sentAt[i].us);
            as->sentAt[i].out = false;
            break;
        }
    }
    pthread_mutex_unlock(&as->lock);

    return SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// Feeding.

// Next audio packet at or after the start, or false at the end of the file.
static bool asend_readPacket(ASend_t *as) {
    for (;;) {
        if (av_read_frame(as->fmt, as->pkt) < 0) {
            return false;
        }
        if (as->pkt->stream_index != as->stream) {
            av_packet_unref(as->pkt);
            continue;
        }
        const int64_t pts = as->pkt->pts != AV_NOPTS_VALUE ? as->pkt->pts : as->pkt->dts;
        const int64_t ms = pts == AV_NOPTS_VALUE
                               ? 0
                               : av_rescale_q(pts, as->fmt->streams[as->stream]->time_base,
                                              (AVRational){1, 1000});
        if (ms < as->startMs) {
            av_packet_unref(as->pkt);
            continue;
        }
        return true;
    }
}

// ADTS header: 7 bytes, 9 with a CRC (protection_absent == 0).
static int asend_adtsHeader(const uint8_t *p, int len) {
    if (len < 7 || p[0] != 0xff || (p[1] & 0xf6) != 0xf0) {
        return 0;
    }
    return (p[1] & 1) ? 7 : 9;
}

static void asend_adtsBuild(const ASend_t *as, uint8_t *h, size_t payload) {
    const size_t len = payload + 7;

    h[0] = 0xff;
    h[1] = 0xf1; // MPEG-4, no CRC
    h[2] = (uint8_t)((as->adtsProfile << 6) | (as->adtsFreq << 2) | (as->adtsChan >> 2));
    h[3] = (uint8_t)(((as->adtsChan & 3) << 6) | ((len >> 11) & 3));
    h[4] = (uint8_t)((len >> 3) & 0xff);
    h[5] = (uint8_t)(((len & 7) << 5) | 0x1f);
    h[6] = 0xfc;
}

static void asend_feedOne(ASend_t *as) {
    if (!as->pktPending) {
        if (as->eof) {
            usleep(ASEND_idleUS);
            return;
        }
        if (!asend_readPacket(as)) {
            as->eof = true;
            as->eofUs = asend_nowUs();
            AW_MPI_ADEC_SetStreamEof(as->adecChn, TRUE);
            LOGI("VS audio end of file after %d packets", as->packetsIn);
            return;
        }
        as->pktPending = true;
    }

    AUDIO_STREAM_S stream;
    const int64_t pts = as->pkt->pts != AV_NOPTS_VALUE ? as->pkt->pts : as->pkt->dts;
    const int skip = as->aacRaw ? asend_adtsHeader(as->pkt->data, as->pkt->size) : 0;

    uint8_t *data = as->pkt->data + skip;
    size_t len = (size_t)(as->pkt->size - skip);

    if (as->adtsWrap) {
        if (len + 7 > as->wrapCap) {
            uint8_t *grown = realloc(as->wrapBuf, len + 7);
            if (!grown) {
                av_packet_unref(as->pkt);
                as->pktPending = false;
                return;
            }
            as->wrapBuf = grown;
            as->wrapCap = len + 7;
        }
        asend_adtsBuild(as, as->wrapBuf, len);
        memcpy(as->wrapBuf + 7, data, len);
        data = as->wrapBuf;
        len += 7;
    }

    memset(&stream, 0, sizeof(stream));
    stream.pStream = data;
    stream.mLen = (unsigned int)len;
    stream.mTimeStamp = pts == AV_NOPTS_VALUE
                            ? 0
                            : (unsigned long long)av_rescale_q(
                                  pts, as->fmt->streams[as->stream]->time_base, (AVRational){1, 1000000});
    stream.mId = as->packetsIn;

    if (AW_MPI_ADEC_SendStream(as->adecChn, &stream, ASEND_sendWAIT_MS) == SUCCESS) {
        av_packet_unref(as->pkt);
        as->pktPending = false;
        pthread_mutex_lock(&as->lock);
        as->packetsIn++;
        pthread_mutex_unlock(&as->lock);
    } else {
        usleep(ASEND_idleUS);
    }
}

static void *asend_thread(void *arg) {
    ASend_t *as = (ASend_t *)arg;
    AUDIO_FRAME_S frame;
    int64_t prevTs = -1;

    for (;;) {
        pthread_mutex_lock(&as->lock);
        while (!as->playing && !as->stop) {
            as->idle = true;
            pthread_cond_broadcast(&as->cond);
            pthread_cond_wait(&as->cond, &as->lock);
        }
        as->idle = as->stop;
        pthread_cond_broadcast(&as->cond);
        pthread_mutex_unlock(&as->lock);
        if (as->stop) {
            break;
        }

        memset(&frame, 0, sizeof(frame));
        if (AW_MPI_ADEC_GetFrame(as->adecChn, &frame, 0) != SUCCESS) {
            const int64_t now = asend_nowUs();
            if (!as->eof && !as->stallLogged && as->lastFrameUs && now - as->lastFrameUs > 500000) {
                as->stallLogged = true;
                LOGW("VS audio: the decoder has given nothing for %dms (%d packets in)",
                     (int)((now - as->lastFrameUs) / 1000), as->packetsIn);
            }
            asend_feedOne(as);
            continue;
        }
        as->lastFrameUs = asend_nowUs();

        // AW_MPI_ADEC_Seek() leaves what was already decoded to come out,
        // the way the video decoder does (see framestep.c).
        if (as->fresh) {
            if (!asend_isFresh(as, (int64_t)frame.mTimeStamp)) {
                AW_MPI_ADEC_ReleaseFrame(as->adecChn, &frame);
                as->staleDropped++;
                continue;
            }
            as->fresh = false;
        }

        if (as->described < 3) {
            LOGI("VS audio frame %d: ts %lluus (+%lld), len %u, rate %d, width %d, mode %d, id %u, "
                 "addr %p",
                 as->described, frame.mTimeStamp,
                 prevTs >= 0 ? (long long)((int64_t)frame.mTimeStamp - prevTs) : 0LL, frame.mLen,
                 frame.mSamplerate, frame.mBitwidth, frame.mSoundmode, frame.mId, frame.mpAddr);
            prevTs = (int64_t)frame.mTimeStamp;
            as->described++;
        }

        {
            const int64_t ts = (int64_t)frame.mTimeStamp;
            const int64_t dur = (int64_t)asend_frameSamples(&frame) * 1000000 / as->rate;

            if (as->sentEndUs && (ts < as->sentEndUs - 5000 || ts > as->sentEndUs + 30000)) {
                as->jumps++;
                if (as->jumps <= 20) {
                    LOGW("VS audio frame at %lldus does not follow %lldus (%+lldms)", (long long)ts,
                         (long long)as->sentEndUs, (long long)((ts - as->sentEndUs) / 1000));
                }
            }
            as->sentEndUs = ts + dur;
        }

        const int64_t t = asend_nowUs();
        const ERRORTYPE ret = AW_MPI_AO_SendFrame(as->aoDev, as->aoChn, &frame, 0);

        pthread_mutex_lock(&as->lock);
        as->framesOut++;
        if (ret == SUCCESS) {
            as->framesSent++;
            for (int n = 0; n < ASEND_SLOTS; n++) {
                const int i = (as->sentNext + n) % ASEND_SLOTS;
                if (!as->sentAt[i].out) {
                    as->sentAt[i].frame = frame;
                    as->sentAt[i].us = t;
                    as->sentAt[i].out = true;
                    as->sentNext = (i + 1) % ASEND_SLOTS;
                    break;
                }
            }
        } else {
            as->sendErrs++;
            as->lastSendErr = ret;
        }
        pthread_mutex_unlock(&as->lock);

        if (ret != SUCCESS) {
            AW_MPI_ADEC_ReleaseFrame(as->adecChn, &frame);
        }
    }

    pthread_mutex_lock(&as->lock);
    as->idle = true;
    pthread_cond_broadcast(&as->cond);
    pthread_mutex_unlock(&as->lock);
    return NULL;
}

///////////////////////////////////////////////////////////////////////////////

static bool asend_openReader(ASend_t *as, const char *path) {
    if (avformat_open_input(&as->fmt, path, NULL, NULL) < 0 ||
        avformat_find_stream_info(as->fmt, NULL) < 0) {
        LOGE("VS audio cannot read %s", path);
        return false;
    }

    // Not av_find_best_stream(): it passes over audio with no sample rate or
    // channel count, and with no decoder in this ffmpeg to find them, every
    // AAC stream has neither. Measured: "no audio stream" on every recording.
    for (unsigned int i = 0; i < as->fmt->nb_streams; i++) {
        const AVCodecParameters *par = as->fmt->streams[i]->codecpar;
        if (as->stream < 0 && par->codec_type == AVMEDIA_TYPE_AUDIO &&
            par->codec_id == AV_CODEC_ID_AAC) {
            as->stream = (int)i;
        } else {
            as->fmt->streams[i]->discard = AVDISCARD_ALL;
        }
    }
    if (as->stream < 0) {
        LOGE("VS audio: no AAC stream in %s", path);
        return false;
    }

    as->pkt = av_packet_alloc();
    if (!as->pkt) {
        return false;
    }

    // The rate and channels from the first packet's ADTS header, which the
    // decoder and the output have to be set up with before it goes in.
    static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                22050, 16000, 12000, 11025, 8000, 7350};
    const AVCodecParameters *par = as->fmt->streams[as->stream]->codecpar;
    const char *from = "the container";

    bool adts = false;

    as->rate = par->sample_rate;
    as->channels = par->channels;
    if (asend_readPacket(as)) {
        const uint8_t *h = as->pkt->data;
        as->pktPending = true;
        if (asend_adtsHeader(h, as->pkt->size)) {
            adts = true;
            const int rateIndex = (h[2] >> 2) & 0xf;
            const int channels = ((h[2] & 1) << 2) | (h[3] >> 6);
            if (rateIndex < (int)(sizeof(rates) / sizeof(rates[0])) && channels > 0) {
                as->rate = rates[rateIndex];
                as->channels = channels;
                from = "adts";
            }
        }
    }
    // mp4 keeps bare frames and the configuration in the container
    // (AudioSpecificConfig: object type, rate index, channels), which is all
    // an ADTS header needs.
    if (!adts && !as->aacRaw && par->extradata_size >= 2) {
        const uint8_t *asc = par->extradata;
        const int object = asc[0] >> 3;
        const int freq = ((asc[0] & 7) << 1) | (asc[1] >> 7);
        const int chan = (asc[1] >> 3) & 0xf;

        if (object >= 1 && object <= 4 && freq < (int)(sizeof(rates) / sizeof(rates[0])) && chan > 0) {
            as->adtsWrap = true;
            as->adtsProfile = object - 1;
            as->adtsFreq = freq;
            as->adtsChan = chan;
            as->rate = rates[freq];
            as->channels = chan;
            from = "the AAC configuration";
            LOGI("VS audio: bare AAC frames, given ADTS headers here (object %d)", object);
        }
    }
    // Nothing to build a header from: have the decoder add its own.
    if (!adts && !as->adtsWrap && !as->aacRaw) {
        as->aacRaw = true;
        LOGI("VS audio: bare AAC frames, the decoder adds the headers");
    }
    if (as->rate <= 0 || as->channels <= 0) {
        // What the recorder writes.
        as->rate = 48000;
        as->channels = 2;
        from = "a guess";
    }
    LOGI("VS audio stream %d, %dHz %dch from %s", as->stream, as->rate, as->channels, from);

    return true;
}

// The decoder will not be destroyed until it has every frame back: it waits
// for ever ("Wait ADec idleOutFrameList [8]nodes to home[64]", the first
// run's hang), and the frames it decoded ahead that were never taken are not
// back until taken and released. The output's must be back already.
static void asend_closeAdec(ASend_t *as) {
    AUDIO_FRAME_S frame;
    int drained = 0;

    if (as->adecChn == MM_INVALID_CHN) {
        return;
    }

    AW_MPI_ADEC_StopRecvStream(as->adecChn);
    while (drained < 256) {
        memset(&frame, 0, sizeof(frame));
        if (AW_MPI_ADEC_GetFrame(as->adecChn, &frame, 0) != SUCCESS) {
            break;
        }
        AW_MPI_ADEC_ReleaseFrame(as->adecChn, &frame);
        drained++;
    }
    AW_MPI_ADEC_DestroyChn(as->adecChn);
    as->adecChn = MM_INVALID_CHN;
    LOGI("VS audio decoder destroyed, %d decoded frames drained first", drained);
}

static bool asend_openAdec(ASend_t *as) {
    ADEC_CHN_ATTR_S attr;
    ERRORTYPE ret = FAILURE;

    memset(&attr, 0, sizeof(attr));
    attr.mType = PT_AAC;
    attr.sampleRate = as->rate;
    attr.channels = as->channels;
    attr.bitsPerSample = 16;
    attr.attachAACHeader = as->aacRaw ? 1 : 0;

    for (ADEC_CHN chn = 0; chn < ADEC_MAX_CHN_NUM; chn++) {
        ret = AW_MPI_ADEC_CreateChn(chn, &attr);
        if (ret == SUCCESS) {
            as->adecChn = chn;
            break;
        }
        if (ret != ERR_ADEC_EXIST) {
            break;
        }
    }
    if (as->adecChn == MM_INVALID_CHN) {
        LOGE("VS audio: no adec channel (%x)", ret);
        return false;
    }

    LOGI("VS audio adec %d, %dHz %dch, %s", as->adecChn, as->rate, as->channels,
         as->aacRaw ? "bare frames" : "adts");
    return true;
}

// The way MPP's player set it up (adec2ao_createAOChn(), since removed), less
// the binding.
static bool asend_openAo(ASend_t *as) {
    AIO_ATTR_S attr;
    ERRORTYPE ret;

    memset(&attr, 0, sizeof(attr));
    attr.u32ChnCnt = (unsigned int)as->channels;
    attr.enBitwidth = AUDIO_BIT_WIDTH_16;
    attr.enSamplerate = (AUDIO_SAMPLE_RATE_E)as->rate;

    as->aoDev = 0;
    AW_MPI_AO_SetPubAttr(as->aoDev, &attr);
    ret = AW_MPI_AO_Enable(as->aoDev);
    if (ret != SUCCESS) {
        LOGW("VS audio ao enable %x", ret);
    }

    for (AO_CHN chn = 0; chn < AIO_MAX_CHN_NUM; chn++) {
        ret = AW_MPI_AO_EnableChn(as->aoDev, chn);
        if (ret == SUCCESS) {
            as->aoChn = chn;
            break;
        }
        if (ret != ERR_AO_EXIST) {
            break;
        }
    }
    if (as->aoChn == MM_INVALID_CHN) {
        LOGE("VS audio: no ao channel (%x)", ret);
        return false;
    }

    MPPCallbackInfo cb = {.cookie = as, .callback = (MPPCallbackFuncType)&asend_callback};
    AW_MPI_AO_RegisterCallback(as->aoDev, as->aoChn, &cb);

    LOGI("VS audio ao dev %d chn %d", as->aoDev, as->aoChn);
    return true;
}

ASend_t *asend_open(const char *path, int start_ms, bool aac_raw) {
    ASend_t *as = asend_open_paused(path, aac_raw);

    if (as && !asend_play_from(as, start_ms)) {
        asend_close(as);
        return NULL;
    }
    return as;
}

ASend_t *asend_open_paused(const char *path, bool aac_raw) {
    const int64_t t0 = asend_nowUs();
    ASend_t *as = calloc(1, sizeof(*as));

    if (!as) {
        return NULL;
    }
    pthread_mutex_init(&as->lock, NULL);
    pthread_cond_init(&as->cond, NULL);
    as->stream = -1;
    as->adecChn = MM_INVALID_CHN;
    as->aoDev = MM_INVALID_DEV;
    as->aoChn = MM_INVALID_CHN;
    as->aacRaw = aac_raw;

    if (!asend_openReader(as, path) || !asend_openAdec(as) || !asend_openAo(as)) {
        asend_close(as);
        return NULL;
    }

    if (pthread_create(&as->thread, NULL, asend_thread, as) != 0) {
        asend_close(as);
        return NULL;
    }
    as->threadRunning = true;

    LOGI("VS audio open took %dms", (int)((asend_nowUs() - t0) / 1000));
    return as;
}

void asend_close(ASend_t *as) {
    if (!as) {
        return;
    }

    if (as->threadRunning) {
        pthread_mutex_lock(&as->lock);
        as->stop = true;
        pthread_cond_broadcast(&as->cond);
        pthread_mutex_unlock(&as->lock);
        pthread_join(as->thread, NULL);
    }

    if (as->aoChn != MM_INVALID_CHN) {
        const int64_t t0 = asend_nowUs();
        const ERRORTYPE stop = AW_MPI_AO_StopChn(as->aoDev, as->aoChn);
        int out;

        for (;;) {
            pthread_mutex_lock(&as->lock);
            out = as->framesSent - as->framesBack;
            pthread_mutex_unlock(&as->lock);
            if (out <= 0 || asend_nowUs() - t0 > ASEND_stopWAIT_MS * 1000) {
                break;
            }
            usleep(ASEND_idleUS);
        }
        LOGI("VS audio stop %x, %d frames still out after %dms", stop, out,
             (int)((asend_nowUs() - t0) / 1000));
        AW_MPI_AO_DisableChn(as->aoDev, as->aoChn);

        // Disabling the channel hands back what the output still held,
        // through the callback ("release all frames to ADec" in its log).
        // Handing them back from here as well was a double release ("adec
        // frameId ... is not find in UsedOutFrameList").
    }
    if (as->aoDev != MM_INVALID_DEV) {
        AW_MPI_AO_Disable(as->aoDev);
    }
    asend_closeAdec(as);

    pthread_mutex_lock(&as->lock);
    LOGI("VS audio summary packets=%d decoded=%d sent=%d back=%d send_errs=%d (last %x) "
         "back_gap min/avg/max=%d/%d/%dus in_ring min/avg/max=%d/%d/%dus",
         as->packetsIn, as->framesOut, as->framesSent, as->framesBack, as->sendErrs,
         as->lastSendErr, (int)as->backGap.min, (int)aspan_avg(&as->backGap),
         (int)as->backGap.max, (int)as->inRing.min, (int)aspan_avg(&as->inRing),
         (int)as->inRing.max);
    pthread_mutex_unlock(&as->lock);

    if (as->pkt) {
        av_packet_free(&as->pkt);
    }
    free(as->wrapBuf);
    if (as->fmt) {
        avformat_close_input(&as->fmt);
    }
    pthread_cond_destroy(&as->cond);
    pthread_mutex_destroy(&as->lock);
    free(as);
}

// ALSA's state in one line, around the pauses and restarts, to see what the
// output's pause and seek do to what is already queued there.
static void asend_procLog(ASend_t *as, const char *when) {
    if (asend_procRead(as, asend_nowUs())) {
        LOGI("VS audio alsa %s: %s hw=%lld appl=%lld delay=%lld", when, as->procState,
             (long long)as->procHwPtr, (long long)as->procApplPtr, (long long)as->procDelay);
    }
}

void asend_pause(ASend_t *as) {
    if (!as) {
        return;
    }

    pthread_mutex_lock(&as->lock);
    const bool was = as->playing;
    as->playing = false;
    pthread_cond_broadcast(&as->cond);
    while (!as->idle) {
        pthread_cond_wait(&as->cond, &as->lock);
    }
    pthread_mutex_unlock(&as->lock);

    if (!was) {
        return;
    }

    const int64_t t0 = asend_nowUs();
    const ERRORTYPE adec = AW_MPI_ADEC_Pause(as->adecChn);
    const ERRORTYPE ao = AW_MPI_AO_PauseChn(as->aoDev, as->aoChn);
    LOGI("VS audio pause: adec %x, ao %x, %dms", adec, ao, (int)((asend_nowUs() - t0) / 1000));
    asend_procLog(as, "paused");
}

bool asend_play_from(ASend_t *as, int ms) {
    if (!as) {
        return false;
    }

    asend_pause(as);

    const int64_t t0 = asend_nowUs();
    const int aim = ms > ASEND_seekSHORT_MS ? ms - ASEND_seekSHORT_MS : 0;
    ERRORTYPE seekA = SUCCESS, seekO = SUCCESS;
    int drained = 0, stillOut = 0;

    // The reader, on the first packet at or after the position.
    if (as->pktPending) {
        av_packet_unref(as->pkt);
        as->pktPending = false;
    }
    as->eof = false;
    as->eofUs = 0;
    as->startMs = ms;
    av_seek_frame(as->fmt, as->stream,
                  av_rescale_q(aim, (AVRational){1, 1000}, as->fmt->streams[as->stream]->time_base),
                  AVSEEK_FLAG_BACKWARD);

    // A fresh decoder for every play. AW_MPI_ADEC_Seek() left it in a state
    // that played on for a while and then went wrong: after the second or
    // third resume of an mp4 it took packets and gave no frames (sound gone),
    // and the user heard the sound doubled with a delay after another. Every
    // first play from a new decoder was right, in every run, so every play is
    // a first play.
    //
    // The output is flushed first (the order MPP's own player uses: pause,
    // seek, start), and its seek hands back what it held, through the
    // callback, which is what destroying the decoder waits for. Should any be
    // left out, destroying would hang, so the decoder is then kept and
    // flushed instead.
    bool renewed = false;
    if (as->started) {
        AW_MPI_AO_SetStreamEof(as->aoDev, as->aoChn, FALSE, FALSE);
        seekO = AW_MPI_AO_Seek(as->aoDev, as->aoChn);

        const int64_t t1 = asend_nowUs();
        int out;
        for (;;) {
            pthread_mutex_lock(&as->lock);
            out = as->framesSent - as->framesBack;
            pthread_mutex_unlock(&as->lock);
            if (out <= 0 || asend_nowUs() - t1 > 200000) {
                break;
            }
            usleep(2000);
        }

        if (out <= 0) {
            asend_closeAdec(as);
            renewed = asend_openAdec(as);
            if (!renewed) {
                LOGE("VS audio: no decoder to play with");
                return false;
            }
        } else {
            LOGW("VS audio: %d frames still with the output, flushing the decoder instead", out);
            AW_MPI_ADEC_SetStreamEof(as->adecChn, FALSE);
            seekA = AW_MPI_ADEC_Seek(as->adecChn);

            AUDIO_FRAME_S frame;
            while (drained < 256) {
                memset(&frame, 0, sizeof(frame));
                if (AW_MPI_ADEC_GetFrame(as->adecChn, &frame, 0) != SUCCESS) {
                    break;
                }
                AW_MPI_ADEC_ReleaseFrame(as->adecChn, &frame);
                drained++;
            }
        }
    }

    pthread_mutex_lock(&as->lock);
    // After the output's seek it holds nothing, whatever the slots say. Left
    // alone, slots its seek emptied without a callback piled up, 6 more on
    // every resume.
    for (int i = 0; i < ASEND_SLOTS; i++) {
        stillOut += as->sentAt[i].out ? 1 : 0;
        as->sentAt[i].out = false;
    }
    as->firstBackUs = 0;
    as->firstPtsUs = 0;
    as->sentEndUs = 0;
    as->lastFrameUs = asend_nowUs();
    as->stallLogged = false;
    as->clockArmed = true;
    as->targetMs = ms;
    as->fresh = true;
    pthread_mutex_unlock(&as->lock);

    const ERRORTYPE startO = AW_MPI_AO_StartChn(as->aoDev, as->aoChn);
    const ERRORTYPE startA = AW_MPI_ADEC_StartRecvStream(as->adecChn);
    as->started = true;
    as->plays++;

    LOGI("VS audio play %d from %dms: %s, seek ao %x adec %x, drained %d, %d slots out, "
         "start ao %x adec %x, %dms",
         as->plays, ms, renewed ? "new decoder" : "same decoder", seekO, seekA, drained, stillOut,
         startO, startA, (int)((asend_nowUs() - t0) / 1000));
    asend_procLog(as, "restarted");

    pthread_mutex_lock(&as->lock);
    as->playing = true;
    pthread_cond_broadcast(&as->cond);
    pthread_mutex_unlock(&as->lock);

    return true;
}

bool asend_audible(ASend_t *as, int method, int64_t now_us, int64_t *pts_us) {
    int64_t firstBack, firstPts;

    if (!as) {
        return false;
    }

    pthread_mutex_lock(&as->lock);
    firstBack = as->firstBackUs;
    firstPts = as->firstPtsUs;
    pthread_mutex_unlock(&as->lock);

    if (!firstBack) {
        return false;
    }

    if (method == ASEND_CLOCK_START) {
        *pts_us = firstPts + (now_us - firstBack);
        return true;
    }

    if (!as->procReadUs || now_us - as->procReadUs >= ASEND_procEVERY_US) {
        asend_procRead(as, now_us);
    }
    if (!as->procRunningUs || strcmp(as->procState, "RUNNING") != 0) {
        return false;
    }

    // What has been handed back, less what ALSA still has queued. The output's
    // own ring between the two was measured at next to nothing (what was
    // handed back ran ahead of the clock by what ALSA said it held, to 10ms),
    // and unlike hw_ptr this needs no origin, so it holds across restarts.
    pthread_mutex_lock(&as->lock);
    const int64_t backEnd = as->backEndPtsUs;
    pthread_mutex_unlock(&as->lock);
    *pts_us = backEnd - as->procDelay * 1000000 / as->rate + (now_us - as->procReadUs);
    return true;
}

bool asend_exhausted(ASend_t *as) {
    bool exhausted;

    if (!as) {
        return true;
    }
    pthread_mutex_lock(&as->lock);
    exhausted = as->clockArmed && as->eofUs && asend_nowUs() - as->eofUs > 200000;
    pthread_mutex_unlock(&as->lock);
    return exhausted;
}

void asend_log(ASend_t *as, int64_t now_us) {
    int64_t start = 0, proc = 0;
    bool haveStart, haveProc;

    if (!as) {
        return;
    }

    haveStart = asend_audible(as, ASEND_CLOCK_START, now_us, &start);
    haveProc = asend_audible(as, ASEND_CLOCK_PROC, now_us, &proc);

    pthread_mutex_lock(&as->lock);
    // How far the output has taken frames ahead of the monotonic clock since
    // the first: what sits in its ring and ALSA's, less the start delay.
    const int64_t lead = as->framesBack
                             ? as->backEndPtsUs - as->firstPtsUs - (now_us - as->firstBackUs)
                             : 0;
    LOGI("VS audio in=%d decoded=%d sent=%d back=%d out=%d lead=%dms start=%dms proc=%dms "
         "start-proc=%dms alsa %s hw=%lld appl=%lld delay=%lld (%d reads)",
         as->packetsIn, as->framesOut, as->framesSent, as->framesBack,
         as->framesSent - as->framesBack, (int)(lead / 1000),
         haveStart ? (int)(start / 1000) : -1, haveProc ? (int)(proc / 1000) : -1,
         haveStart && haveProc ? (int)((start - proc) / 1000) : 0,
         as->procFound ? as->procState : "-", (long long)as->procHwPtr,
         (long long)as->procApplPtr, (long long)as->procDelay, as->procReads);
    pthread_mutex_unlock(&as->lock);
}
