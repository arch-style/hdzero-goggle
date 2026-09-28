#include "framestep.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

// #define LOG_NDEBUG 0
#define LOG_TAG "framestep"
#include <log/log.h>

#include <mpi_sys.h>
#include <mpi_vdec.h>

#include <libavformat/avformat.h>

// The decoder is asked for a picture without waiting before anything goes in,
// and with a short wait after. A 720p frame takes the hardware a few ms, and a
// step back decodes up to a second of them in a row, so this wait is most of
// what a step back costs. The long one is only for draining the end of file.
#define FRAMESTEP_getWAIT_MS   4
#define FRAMESTEP_drainWAIT_MS 200
#define FRAMESTEP_sendWAIT_MS  200

// Packets to put in ahead of asking. The decoder has an input queue and runs
// ahead into it; letting it wait for the next packet after every picture
// serialises the two.
#define FRAMESTEP_feedAHEAD 3

// This runs on the UI thread with the LVGL lock held, so it is bounded by
// wall clock rather than by packets: whatever happens, the dial comes back.
#define FRAMESTEP_deadlineMS 1500

struct FrameStep {
    AVFormatContext *fmt;
    int stream;
    AVPacket *pkt;
    bool pktPending; // pkt holds one the decoder has not accepted yet
    bool eof;

    // mp4/mov samples are length-prefixed and keep their parameter sets in
    // the container, where the hardware wants start codes and the parameter
    // sets in the stream. The shipped ffmpeg has no hevc_mp4toannexb, so the
    // rewrite is done here: the prefix size and the parameter sets come out of
    // the hvcC/avcC extradata once, and every packet is rewritten into
    // annexb on its way to the decoder.
    int nalLengthSize; // 0 = already start-coded, nothing to do
    uint8_t *paramSets; // the parameter sets, start-coded
    size_t paramSetsLen;
    uint8_t *annexb; // the rewritten packet
    size_t annexbCap;

    // Ours alone and never bound to anything. That is the whole point: the
    // player's channel is bound to the output, and the vendor library refuses
    // AW_MPI_VDEC_GetImage on a channel in tunnel mode ("can't call
    // getStream() in tunnel mode!"). Binding is what turns tunnel mode on, so
    // a channel that is never bound is the supported way to take frames out.
    VDEC_CHN vdecChn;
    bool started;

    // A picture is converted into pixels and given straight back: the decoder
    // has a small pool and keeping one out of it, across a seek in particular,
    // was what left it with nothing to decode into ("no picture within
    // 1500ms"). The one exception is the frame read past the target while
    // stepping back, kept so that the next step forward is that frame rather
    // than the one after it.
    VIDEO_FRAME_INFO_S pending;
    bool hasPending;

    uint8_t *pixels;
    int outWidth, outHeight; // 0 = whatever the stream is
    int bufWidth, bufHeight;

    // Where each output column and row reads from. Worked out once per size
    // rather than with a division per pixel, which on this core is a library
    // call two million times a frame.
    int *xMap, *cMap, *yMap;
    int mapSrcW, mapSrcH, mapOutW, mapOutH, mapCStep, mapTop, mapLeft;

    // The two most recent keyframes fed to the decoder, by pts. A step back
    // has to start decoding at the keyframe before the target, and asking the
    // demuxer to seek there is unreliable on a transport stream: it lands
    // wherever, and the decoder then skips silently to the next keyframe,
    // which can be past the target. Knowing where the keyframes are lets the
    // seek aim short of one on purpose.
    int keyMs[2];
    int keyCount;

    // Set by a seek, cleared by the first frame that could only have come
    // from after it. AW_MPI_VDEC_Seek does not empty the decoder's output:
    // the first GetImage after one still hands back what was decoded before
    // it. Measured: a step from 1533ms was placed at 7033ms, the frame after
    // the previous session's last, and a step back found "nothing before"
    // its target because the first frame out was a leftover past it.
    bool awaitingFresh;

    int shownMs;
    bool hasShown;
    bool describedFrame;
};

#define FRAMESTEP_noKEY (-1)

// The recorder's keyframe spacing. A guess the seeks start from and widen
// when it turns out wrong; nothing here depends on it being exact.
#define FRAMESTEP_gopMS 1000

// How far short of a keyframe to ask the demuxer to seek, so that where it
// lands is before the keyframe and the decoder starts there. Larger than any
// overshoot seen, smaller than the recorder's keyframe spacing.
#define FRAMESTEP_seekSHORT_MS 250

static void framestep_noteKey(FrameStep_t *fs, int ms) {
    if (fs->keyCount > 0 && fs->keyMs[0] == ms) {
        return;
    }
    fs->keyMs[1] = fs->keyMs[0];
    fs->keyMs[0] = ms;
    if (fs->keyCount < 2) {
        fs->keyCount++;
    }
}

// The keyframe at or before this position, if one has gone past.
static int framestep_keyBefore(const FrameStep_t *fs, int ms) {
    for (int i = 0; i < fs->keyCount; i++) {
        if (fs->keyMs[i] <= ms) {
            return fs->keyMs[i];
        }
    }
    return FRAMESTEP_noKEY;
}

static int framestep_frameMs(const VIDEO_FRAME_INFO_S *info) {
    return (int)(info->VFrame.mpts / 1000);
}

static bool framestep_isFresh(const FrameStep_t *fs, int ms) {
    for (int i = 0; i < fs->keyCount; i++) {
        if (ms >= fs->keyMs[i] - 20 && ms <= fs->keyMs[i] + FRAMESTEP_gopMS + 500) {
            return true;
        }
    }
    return false;
}

static void framestep_release(FrameStep_t *fs, VIDEO_FRAME_INFO_S *info) {
    AW_MPI_VDEC_ReleaseImage(fs->vdecChn, info);
}

static void framestep_dropPending(FrameStep_t *fs) {
    if (fs->hasPending) {
        framestep_release(fs, &fs->pending);
        fs->hasPending = false;
    }
}

///////////////////////////////////////////////////////////////////////////////
// The picture's layout, and the conversion.

// The frame's own account of its layout is only partly true. Measured on the
// goggle: a 1280x720 recording comes back as mWidth 1280, mHeight 736 (the
// buffer, padded), offsets top 0 / bottom 720 (the picture), and strides of
// 1280 for all three planes -- but the two chroma planes start 640*368 bytes
// apart, so their real stride is 640. Reading them at 1280 walks off the end
// of the last one, which shows as a moire over the picture and, when that
// buffer is the last in the pool, as a SIGSEGV. So the picture rectangle comes
// from the offsets and the chroma stride from where the planes actually are.
typedef struct {
    int srcW, srcH; // the picture, not the buffer
    int top, left;  // where it starts in the buffer
    int yStride, cStride;
    int cStep, uOff, vOff;
    const uint8_t *yPlane, *uPlane, *vPlane;
    size_t ySpan, cSpan; // how far a read can reach into each plane
} FrameLayout_t;

static bool framestep_layout(const VIDEO_FRAME_S *vf, FrameLayout_t *lo) {
    const bool semiPlanar = (vf->mPixelFormat == MM_PIXEL_FORMAT_YUV_SEMIPLANAR_420 ||
                             vf->mPixelFormat == MM_PIXEL_FORMAT_YVU_SEMIPLANAR_420);
    const bool vFirst = (vf->mPixelFormat == MM_PIXEL_FORMAT_YVU_PLANAR_420 ||
                         vf->mPixelFormat == MM_PIXEL_FORMAT_YVU_SEMIPLANAR_420);

    memset(lo, 0, sizeof(*lo));

    lo->top = vf->mOffsetTop > 0 ? vf->mOffsetTop : 0;
    lo->left = vf->mOffsetLeft > 0 ? vf->mOffsetLeft : 0;
    lo->srcH = (vf->mOffsetBottom > lo->top) ? vf->mOffsetBottom - lo->top : (int)vf->mHeight;
    lo->srcW = (vf->mOffsetRight > lo->left) ? vf->mOffsetRight - lo->left : (int)vf->mWidth;
    if (lo->srcW <= 0 || lo->srcH <= 0) {
        return false;
    }

    lo->yStride = (int)vf->mStride[0] > 0 ? (int)vf->mStride[0] : (int)vf->mWidth;

    lo->yPlane = vf->mpVirAddr[0];
    lo->uPlane = semiPlanar ? vf->mpVirAddr[1] : vf->mpVirAddr[vFirst ? 2 : 1];
    lo->vPlane = semiPlanar ? vf->mpVirAddr[1] : vf->mpVirAddr[vFirst ? 1 : 2];
    if (!lo->yPlane || !lo->uPlane || !lo->vPlane) {
        return false;
    }

    const int cRows = ((int)vf->mHeight + 1) / 2;

    if (semiPlanar) {
        lo->cStride = lo->yStride;
        lo->cStep = 2;
        lo->uOff = vFirst ? 1 : 0;
        lo->vOff = vFirst ? 0 : 1;
    } else {
        // The two planes are laid end to end, so their distance apart is one
        // plane, and a plane is stride * rows.
        const uint8_t *first = lo->uPlane < lo->vPlane ? lo->uPlane : lo->vPlane;
        const uint8_t *second = lo->uPlane < lo->vPlane ? lo->vPlane : lo->uPlane;
        const size_t apart = (size_t)(second - first);

        lo->cStride = (cRows > 0 && apart > 0 && apart % (size_t)cRows == 0)
                          ? (int)(apart / (size_t)cRows)
                          : lo->yStride / 2;
        lo->cStep = 1;
    }

    lo->ySpan = (size_t)lo->yStride * (size_t)vf->mHeight;
    lo->cSpan = (size_t)lo->cStride * (size_t)cRows;

    return true;
}

static bool framestep_sizeBuffer(FrameStep_t *fs, const FrameLayout_t *lo) {
    const int w = fs->outWidth > 0 ? fs->outWidth : lo->srcW;
    const int h = fs->outHeight > 0 ? fs->outHeight : lo->srcH;

    if (fs->pixels && fs->bufWidth == w && fs->bufHeight == h) {
        return true;
    }

    uint8_t *pixels = realloc(fs->pixels, (size_t)w * h * 4);
    if (!pixels) {
        LOGE("out of memory for a %dx%d picture", w, h);
        return false;
    }

    fs->pixels = pixels;
    fs->bufWidth = w;
    fs->bufHeight = h;

    return true;
}

static bool framestep_sizeMaps(FrameStep_t *fs, const FrameLayout_t *lo) {
    const int outW = fs->bufWidth;
    const int outH = fs->bufHeight;

    if (fs->xMap && fs->mapSrcW == lo->srcW && fs->mapSrcH == lo->srcH &&
        fs->mapOutW == outW && fs->mapOutH == outH && fs->mapCStep == lo->cStep &&
        fs->mapTop == lo->top && fs->mapLeft == lo->left) {
        return true;
    }

    int *xMap = realloc(fs->xMap, sizeof(int) * (size_t)outW);
    int *cMap = realloc(fs->cMap, sizeof(int) * (size_t)outW);
    int *yMap = realloc(fs->yMap, sizeof(int) * (size_t)outH);
    if (xMap)
        fs->xMap = xMap;
    if (cMap)
        fs->cMap = cMap;
    if (yMap)
        fs->yMap = yMap;
    if (!xMap || !cMap || !yMap) {
        return false;
    }

    // Nearest neighbour, and a plain multiply rather than an accumulator so
    // a rounding drift cannot walk off the end of the plane. 32-bit is
    // enough: 1920 * 1280 is nowhere near the limit.
    for (int ox = 0; ox < outW; ox++) {
        const int sx = lo->left + ((outW == lo->srcW) ? ox : ox * lo->srcW / outW);
        fs->xMap[ox] = sx;
        fs->cMap[ox] = (sx / 2) * lo->cStep;
    }
    for (int oy = 0; oy < outH; oy++) {
        fs->yMap[oy] = lo->top + ((outH == lo->srcH) ? oy : oy * lo->srcH / outH);
    }

    fs->mapSrcW = lo->srcW;
    fs->mapSrcH = lo->srcH;
    fs->mapOutW = outW;
    fs->mapOutH = outH;
    fs->mapCStep = lo->cStep;
    fs->mapTop = lo->top;
    fs->mapLeft = lo->left;

    return true;
}

// Colour conversion and the scaling in one pass. BT.709, which is what these
// recordings are, in integer form: the usual 1.164/1.793/0.213/0.533/2.112
// taken up by 256.
static void framestep_convert(FrameStep_t *fs, const FrameLayout_t *lo) {
    const int outW = fs->bufWidth;
    const int outH = fs->bufHeight;
    const int uOff = lo->uOff;
    const int vOff = lo->vOff;
    const int *xMap = fs->xMap;
    const int *cMap = fs->cMap;

    for (int oy = 0; oy < outH; oy++) {
        const int sy = fs->yMap[oy];
        const uint8_t *yRow = lo->yPlane + (size_t)sy * lo->yStride;
        const uint8_t *uRow = lo->uPlane + (size_t)(sy / 2) * lo->cStride + uOff;
        const uint8_t *vRow = lo->vPlane + (size_t)(sy / 2) * lo->cStride + vOff;
        uint8_t *out = fs->pixels + (size_t)oy * outW * 4;

        for (int ox = 0; ox < outW; ox++) {
            const int c = cMap[ox];
            const int y = ((int)yRow[xMap[ox]] - 16) * 298;
            const int u = (int)uRow[c] - 128;
            const int v = (int)vRow[c] - 128;

            int r = (y + 459 * v) >> 8;
            int g = (y - 55 * u - 136 * v) >> 8;
            int b = (y + 541 * u) >> 8;

            if (r < 0)
                r = 0;
            else if (r > 255)
                r = 255;
            if (g < 0)
                g = 0;
            else if (g > 255)
                g = 255;
            if (b < 0)
                b = 0;
            else if (b > 255)
                b = 255;

            // lv_color32_t order.
            *out++ = (uint8_t)b;
            *out++ = (uint8_t)g;
            *out++ = (uint8_t)r;
            *out++ = 0xff;
        }
    }
}

// Convert a decoded frame into the picture, and give the frame straight back
// to the decoder. Either way the frame is released here.
static bool framestep_publish(FrameStep_t *fs, VIDEO_FRAME_INFO_S *info, FrameStepPic_t *pic) {
    const VIDEO_FRAME_S *vf = &info->VFrame;
    FrameLayout_t lo;
    bool ok = false;

    if (!fs->describedFrame) {
        fs->describedFrame = true;
        LOGI("frame %ux%u, format %d, strides %u/%u/%u, planes %p/%p/%p, "
             "offsets t%d b%d l%d r%d",
             vf->mWidth, vf->mHeight, vf->mPixelFormat,
             vf->mStride[0], vf->mStride[1], vf->mStride[2],
             vf->mpVirAddr[0], vf->mpVirAddr[1], vf->mpVirAddr[2],
             vf->mOffsetTop, vf->mOffsetBottom, vf->mOffsetLeft, vf->mOffsetRight);
    }

    if (!framestep_layout(vf, &lo)) {
        LOGE("frame has no usable layout");
        goto out;
    }

    // Everything the loops will touch, checked once here rather than a
    // million times in there. A read past a plane is what took the process
    // down before this was measured.
    {
        const size_t yEnd = (size_t)(lo.top + lo.srcH - 1) * lo.yStride + lo.left + lo.srcW;
        const size_t cEnd = (size_t)((lo.top + lo.srcH - 1) / 2) * lo.cStride +
                            ((lo.left + lo.srcW - 1) / 2) * lo.cStep + 2;

        if (yEnd > lo.ySpan || cEnd > lo.cSpan) {
            LOGE("frame layout would be read past its planes (y %zu of %zu, c %zu of %zu)",
                 yEnd, lo.ySpan, cEnd, lo.cSpan);
            goto out;
        }
    }

    if (!framestep_sizeBuffer(fs, &lo) || !framestep_sizeMaps(fs, &lo)) {
        goto out;
    }

    framestep_convert(fs, &lo);

    fs->shownMs = framestep_frameMs(info);
    fs->hasShown = true;

    pic->pixels = fs->pixels;
    pic->width = fs->bufWidth;
    pic->height = fs->bufHeight;
    pic->pts_ms = fs->shownMs;
    ok = true;

out:
    framestep_release(fs, info);
    return ok;
}

///////////////////////////////////////////////////////////////////////////////
// Feeding the decoder.

static const uint8_t framestep_startCode[4] = {0, 0, 0, 1};

static bool framestep_grow(uint8_t **buf, size_t *cap, size_t need) {
    if (need <= *cap) {
        return true;
    }

    uint8_t *next = realloc(*buf, need);
    if (!next) {
        return false;
    }

    *buf = next;
    *cap = need;
    return true;
}

// Pull the parameter sets out of hvcC or avcC and write them start-coded.
// hvcC: nal length size in byte 21, then byte 22 counts arrays of (type,
// count, then count x (len, nal)). avcC: nal length size in byte 4, then
// SPS count in byte 5, that many (len, nal), then PPS count, that many.
static bool framestep_parseConfig(FrameStep_t *fs, const uint8_t *ex, int size, bool hevc) {
    size_t need = 0;
    const uint8_t *p;
    const uint8_t *end = ex + size;

    fs->nalLengthSize = hevc ? ((size > 21) ? (ex[21] & 3) + 1 : 0)
                             : ((size > 4) ? (ex[4] & 3) + 1 : 0);
    if (fs->nalLengthSize == 0) {
        return false;
    }

    // Two passes: size it, then fill it.
    for (int pass = 0; pass < 2; pass++) {
        size_t at = 0;

        if (hevc) {
            if (size < 23) {
                return false;
            }
            p = ex + 23;
            const int arrays = ex[22];

            for (int a = 0; a < arrays && p + 3 <= end; a++) {
                const int count = (p[1] << 8) | p[2];
                p += 3;
                for (int n = 0; n < count && p + 2 <= end; n++) {
                    const int len = (p[0] << 8) | p[1];
                    p += 2;
                    if (p + len > end) {
                        return false;
                    }
                    if (pass) {
                        memcpy(fs->paramSets + at, framestep_startCode, 4);
                        memcpy(fs->paramSets + at + 4, p, len);
                    }
                    at += 4 + len;
                    p += len;
                }
            }
        } else {
            if (size < 7) {
                return false;
            }
            p = ex + 5;
            for (int group = 0; group < 2 && p < end; group++) {
                const int count = group == 0 ? (p[0] & 0x1f) : p[0];
                p += 1;
                for (int n = 0; n < count && p + 2 <= end; n++) {
                    const int len = (p[0] << 8) | p[1];
                    p += 2;
                    if (p + len > end) {
                        return false;
                    }
                    if (pass) {
                        memcpy(fs->paramSets + at, framestep_startCode, 4);
                        memcpy(fs->paramSets + at + 4, p, len);
                    }
                    at += 4 + len;
                    p += len;
                }
            }
        }

        if (pass == 0) {
            need = at;
            if (need == 0) {
                return false;
            }
            fs->paramSets = malloc(need);
            if (!fs->paramSets) {
                return false;
            }
        } else {
            fs->paramSetsLen = at;
        }
    }

    return true;
}

// Rewrite a length-prefixed packet into start codes, with the parameter sets
// in front of a keyframe so the decoder has them after every seek. Returns
// the rewritten bytes and length through the arguments, or the packet's own
// when there is nothing to do.
static bool framestep_toAnnexB(FrameStep_t *fs, const AVPacket *pkt, const uint8_t **data, size_t *len) {
    if (fs->nalLengthSize == 0) {
        *data = pkt->data;
        *len = (size_t)pkt->size;
        return true;
    }

    const bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
    const size_t prefix = key ? fs->paramSetsLen : 0;
    const size_t worst = prefix + (size_t)pkt->size + ((size_t)pkt->size / (size_t)fs->nalLengthSize + 1) * 4;

    if (!framestep_grow(&fs->annexb, &fs->annexbCap, worst)) {
        return false;
    }

    size_t at = 0;
    if (prefix) {
        memcpy(fs->annexb, fs->paramSets, prefix);
        at = prefix;
    }

    const uint8_t *p = pkt->data;
    const uint8_t *end = pkt->data + pkt->size;

    while (p + fs->nalLengthSize <= end) {
        size_t nal = 0;
        for (int i = 0; i < fs->nalLengthSize; i++) {
            nal = (nal << 8) | p[i];
        }
        p += fs->nalLengthSize;
        if (nal == 0 || p + nal > end) {
            break;
        }
        memcpy(fs->annexb + at, framestep_startCode, 4);
        memcpy(fs->annexb + at + 4, p, nal);
        at += 4 + nal;
        p += nal;
    }

    *data = fs->annexb;
    *len = at;
    return at > prefix;
}

static int64_t framestep_nowMs(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// One packet from the file into the decoder. Returns false at the end of the
// file. A packet the decoder will not take is kept, not dropped: dropping one
// leaves the decoder waiting for the rest of a picture that never comes.
static bool framestep_feedOne(FrameStep_t *fs) {
    if (!fs->pktPending) {
        for (;;) {
            if (av_read_frame(fs->fmt, fs->pkt) < 0) {
                fs->eof = true;
                AW_MPI_VDEC_SetStreamEof(fs->vdecChn, 1);
                return false;
            }
            if (fs->pkt->stream_index == fs->stream) {
                break;
            }
            av_packet_unref(fs->pkt);
        }
        fs->pktPending = true;
    }

    VDEC_STREAM_S stream;
    const uint8_t *data;
    size_t len;
    int64_t pts = fs->pkt->pts;

    if (pts == AV_NOPTS_VALUE) {
        pts = fs->pkt->dts;
    }
    if (!framestep_toAnnexB(fs, fs->pkt, &data, &len)) {
        // Nothing usable in it; move on rather than feed the decoder junk.
        av_packet_unref(fs->pkt);
        fs->pktPending = false;
        return true;
    }

    if ((fs->pkt->flags & AV_PKT_FLAG_KEY) && pts != AV_NOPTS_VALUE) {
        framestep_noteKey(fs, (int)av_rescale_q(pts, fs->fmt->streams[fs->stream]->time_base,
                                                (AVRational){1, 1000}));
    }

    memset(&stream, 0, sizeof(stream));
    stream.pAddr = (unsigned char *)data;
    stream.mLen = (unsigned int)len;
    stream.mPTS = (pts == AV_NOPTS_VALUE)
                      ? 0
                      : (uint64_t)av_rescale_q(pts, fs->fmt->streams[fs->stream]->time_base,
                                               (AVRational){1, 1000000});
    stream.mbEndOfFrame = TRUE;

    if (AW_MPI_VDEC_SendStream(fs->vdecChn, &stream, FRAMESTEP_sendWAIT_MS) == SUCCESS) {
        av_packet_unref(fs->pkt);
        fs->pktPending = false;
    }

    return true;
}

// A frame just taken from the decoder: keep it, or let it go because it is
// from before the last seek.
static bool framestep_accept(FrameStep_t *fs, VIDEO_FRAME_INFO_S *out) {
    if (!fs->awaitingFresh) {
        return true;
    }

    const int ms = framestep_frameMs(out);

    if (framestep_isFresh(fs, ms)) {
        fs->awaitingFresh = false;
        return true;
    }

    LOGD("leftover %dms from before the seek, dropped", ms);
    framestep_release(fs, out);
    return false;
}

// One picture out of the decoder, feeding it packets until it gives one or the
// file runs out.
static bool framestep_decodeOne(FrameStep_t *fs, VIDEO_FRAME_INFO_S *out) {
    const int64_t deadline = framestep_nowMs() + FRAMESTEP_deadlineMS;

    for (;;) {
        if (framestep_nowMs() > deadline) {
            LOGE("no picture within %dms, giving up on this step", FRAMESTEP_deadlineMS);
            return false;
        }

        memset(out, 0, sizeof(*out));
        if (AW_MPI_VDEC_GetImage(fs->vdecChn, out, 0) == SUCCESS) {
            if (framestep_accept(fs, out)) {
                return true;
            }
            continue;
        }

        // Keep the decoder's queue topped up. Stop early if it will not take
        // any more: the packet stays pending and goes in next time round.
        for (int n = 0; n < FRAMESTEP_feedAHEAD && !fs->eof; n++) {
            if (!framestep_feedOne(fs) || fs->pktPending) {
                break;
            }
        }

        memset(out, 0, sizeof(*out));
        if (AW_MPI_VDEC_GetImage(fs->vdecChn, out,
                                 fs->eof ? FRAMESTEP_drainWAIT_MS : FRAMESTEP_getWAIT_MS) == SUCCESS) {
            if (framestep_accept(fs, out)) {
                return true;
            }
            continue;
        }
        if (fs->eof) {
            return false;
        }
    }
}

// Put the demuxer on the keyframe at or before this position and throw away
// whatever the decoder was holding, ours included.
static bool framestep_seekBefore(FrameStep_t *fs, int ms) {
    const int64_t ts = av_rescale_q(ms < 0 ? 0 : ms, (AVRational){1, 1000},
                                    fs->fmt->streams[fs->stream]->time_base);

    framestep_dropPending(fs);

    if (av_seek_frame(fs->fmt, fs->stream, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        LOGD("seek to %dms failed", ms);
        return false;
    }

    if (fs->pktPending) {
        av_packet_unref(fs->pkt);
        fs->pktPending = false;
    }
    fs->eof = false;
    fs->keyCount = 0;

    // Paused around the reset, the way the player's own seek does it
    // (media_seek(): pause, seek, start). Reset while the decode thread was
    // running, the HEVC decoder aborted the whole app one back-step in a few
    // dozen: SbmFrameFlushFramePic() in libvdecoder asserts (line 1013) that
    // the stream frame handed back is the next in its ring, and a flush racing
    // a decode breaks that. Crash record: SIGABRT in thread MPP_VDec, via
    // H265DecoderDecode -> HevcDecReturnAllSbmStreams -> SbmFrameFlushFramePic.
    AW_MPI_VDEC_Pause(fs->vdecChn);
    AW_MPI_VDEC_SetStreamEof(fs->vdecChn, 0);
    AW_MPI_VDEC_Seek(fs->vdecChn);

    // Whatever was already decoded is from before the seek. Take it out now,
    // and let the freshness check catch what is still in flight.
    {
        VIDEO_FRAME_INFO_S stale;
        int drained = 0;

        for (;;) {
            memset(&stale, 0, sizeof(stale));
            if (AW_MPI_VDEC_GetImage(fs->vdecChn, &stale, 0) != SUCCESS) {
                break;
            }
            framestep_release(fs, &stale);
            drained++;
        }
        if (drained) {
            LOGD("drained %d leftover frames after the seek", drained);
        }
    }
    AW_MPI_VDEC_StartRecvStream(fs->vdecChn);
    fs->awaitingFresh = true;

    return true;
}

///////////////////////////////////////////////////////////////////////////////

FrameStep_t *framestep_open(const char *path) {
    return framestep_open_ex(path, 0);
}

FrameStep_t *framestep_open_ex(const char *path, int extra_frames) {
    FrameStep_t *fs = calloc(1, sizeof(*fs));
    if (!fs) {
        return NULL;
    }

    fs->stream = -1;
    fs->vdecChn = MM_INVALID_CHN;

    if (avformat_open_input(&fs->fmt, path, NULL, NULL) < 0) {
        LOGE("cannot open %s", path);
        goto failed;
    }
    if (avformat_find_stream_info(fs->fmt, NULL) < 0) {
        LOGE("no stream info in %s", path);
        goto failed;
    }

    fs->stream = av_find_best_stream(fs->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (fs->stream < 0) {
        LOGE("no video stream in %s", path);
        goto failed;
    }

    const AVCodecParameters *par = fs->fmt->streams[fs->stream]->codecpar;

    PAYLOAD_TYPE_E type;
    switch (par->codec_id) {
    case AV_CODEC_ID_HEVC:
        type = PT_H265;
        break;
    case AV_CODEC_ID_H264:
        type = PT_H264;
        break;
    default:
        LOGE("codec %d is not one the hardware decoder takes", par->codec_id);
        goto failed;
    }

    // The shipped ffmpeg is built --disable-everything with only demuxers,
    // parsers and muxers, so it reads these files and decodes nothing, which
    // is the half needed: the SoC does the decoding. Transport streams, which
    // the recorder writes, hand over start-coded NALs the decoder takes as
    // they are. mp4 hands over length-prefixed ones and keeps the parameter
    // sets in the container, and there is no hevc_mp4toannexb in the build to
    // fix that, so it is fixed here from the extradata.
    if (par->extradata_size > 0 && par->extradata[0] == 1) {
        if (!framestep_parseConfig(fs, par->extradata, par->extradata_size,
                                   par->codec_id == AV_CODEC_ID_HEVC)) {
            LOGE("%s: cannot read the parameter sets out of its extradata", path);
            goto failed;
        }
        LOGI("length-prefixed samples, %d-byte prefix, %zu bytes of parameter sets",
             fs->nalLengthSize, fs->paramSetsLen);
    }

    // avformat_find_stream_info() normally gets the picture size by decoding a
    // frame, and this ffmpeg has no decoder, so codecpar comes back 0x0. Give
    // the channel the largest the recorder writes and let the stream itself
    // say what it really is.
    VDEC_CHN_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));
    attr.mType = type;
    attr.mPicWidth = par->width > 0 ? (unsigned int)par->width : 1920;
    attr.mPicHeight = par->height > 0 ? (unsigned int)par->height : 1080;
    // What the decoder produces whatever is asked of it, and what the player
    // asks for too. Asking for YU12 only earned a "unknown vdecPixelFormat"
    // from the vendor's format mapping and the same YV12 anyway.
    attr.mOutputPixelFormat = MM_PIXEL_FORMAT_YVU_PLANAR_420;
    attr.mVdecVideoAttr.mMode = VIDEO_MODE_FRAME;
    attr.mVdecVideoAttr.mSupportBFrame = 1;
    attr.mExtraFrameNum = extra_frames > 0 ? extra_frames : 0;

    // Channel 0 is the player's. Take the first one that is free and leave it
    // unbound, which is what makes GetImage legal on it.
    for (VDEC_CHN chn = 0; chn < VDEC_MAX_CHN_NUM; chn++) {
        const ERRORTYPE ret = AW_MPI_VDEC_CreateChn(chn, &attr);

        if (ret == SUCCESS) {
            fs->vdecChn = chn;
            break;
        }
        if (ret != ERR_VDEC_EXIST) {
            LOGE("create vdec channel %d: %x", chn, ret);
            break;
        }
    }

    if (fs->vdecChn == MM_INVALID_CHN) {
        LOGE("no free vdec channel");
        goto failed;
    }

    const ERRORTYPE started = AW_MPI_VDEC_StartRecvStream(fs->vdecChn);
    if (started != SUCCESS) {
        LOGE("start vdec channel %d: %x", fs->vdecChn, started);
        goto failed;
    }
    fs->started = true;

    fs->pkt = av_packet_alloc();
    if (!fs->pkt) {
        goto failed;
    }

    LOGI("stepping %s, codec %d %dx%d on vdec channel %d, %d extra frames",
         path, par->codec_id, par->width, par->height, fs->vdecChn, attr.mExtraFrameNum);

    return fs;

failed:
    framestep_close(fs);
    return NULL;
}

void framestep_close(FrameStep_t *fs) {
    if (!fs) {
        return;
    }

    framestep_dropPending(fs);

    if (fs->vdecChn != MM_INVALID_CHN) {
        if (fs->started) {
            AW_MPI_VDEC_StopRecvStream(fs->vdecChn);

            // Frames decoded ahead and never taken are still the decoder's
            // to account for: destroyed with them waiting, it logs "fatal
            // error! outReadyFrame must be 0!".
            VIDEO_FRAME_INFO_S frame;
            for (int n = 0; n < 64; n++) {
                memset(&frame, 0, sizeof(frame));
                if (AW_MPI_VDEC_GetImage(fs->vdecChn, &frame, 0) != SUCCESS) {
                    break;
                }
                framestep_release(fs, &frame);
            }
        }
        AW_MPI_VDEC_DestroyChn(fs->vdecChn);
    }

    av_packet_free(&fs->pkt);

    if (fs->fmt) {
        avformat_close_input(&fs->fmt);
    }

    free(fs->paramSets);
    free(fs->annexb);
    free(fs->xMap);
    free(fs->cMap);
    free(fs->yMap);
    free(fs->pixels);
    free(fs);
}

int framestep_key_before(FrameStep_t *fs, int ms) {
    if (!fs) {
        return FRAMESTEP_noKEY;
    }
    return framestep_keyBefore(fs, ms);
}

bool framestep_set_output_size(FrameStep_t *fs, int width, int height) {
    if (!fs || width <= 0 || height <= 0) {
        return false;
    }

    fs->outWidth = width;
    fs->outHeight = height;

    return true;
}

int framestep_frame_rate(FrameStep_t *fs) {
    if (!fs || fs->stream < 0) {
        return 0;
    }

    const AVStream *st = fs->fmt->streams[fs->stream];
    AVRational rate = st->avg_frame_rate;

    if (rate.num <= 0 || rate.den <= 0) {
        rate = st->r_frame_rate;
    }
    if (rate.num <= 0 || rate.den <= 0) {
        return 0;
    }

    return (int)av_rescale(1000, rate.num, rate.den);
}

// The first frame at or after this position, left out of the pool for the
// caller to publish or hand on.
static bool framestep_reach(FrameStep_t *fs, int ms, VIDEO_FRAME_INFO_S *out) {
    VIDEO_FRAME_INFO_S frame;

    // A transport stream seek lands wherever, and the decoder then starts at
    // the next keyframe -- which is past the target whenever the landing was
    // after the target's own keyframe. So aim a little short, look at where
    // the decoder actually started, and if that is past the target aim a GOP
    // before it: that first frame is a keyframe, so the one wanted is the
    // one before it. Widen the guess at the spacing if it keeps missing.
    int aim = ms - FRAMESTEP_seekSHORT_MS;
    int gop = FRAMESTEP_gopMS;

    for (int attempt = 0; attempt < 4; attempt++) {
        if (!framestep_seekBefore(fs, aim)) {
            return false;
        }
        if (!framestep_decodeOne(fs, &frame)) {
            return false;
        }

        const int first = framestep_frameMs(&frame);

        if (first <= ms) {
            // Started before the target; decode forward to it.
            for (;;) {
                if (framestep_frameMs(&frame) >= ms) {
                    *out = frame;
                    return true;
                }
                framestep_release(fs, &frame);
                if (!framestep_decodeOne(fs, &frame)) {
                    return false;
                }
            }
        }

        LOGD("seek for %dms started at %dms, aiming a GOP earlier", ms, first);
        framestep_release(fs, &frame);
        if (aim <= 0) {
            break; // the file starts after the target; nothing earlier exists
        }
        aim = first - gop - FRAMESTEP_seekSHORT_MS;
        gop *= 2;
    }

    LOGD("could not reach %dms", ms);
    return false;
}

bool framestep_to(FrameStep_t *fs, int ms, FrameStepPic_t *pic) {
    VIDEO_FRAME_INFO_S frame;

    if (!fs || !pic) {
        return false;
    }
    if (!framestep_reach(fs, ms, &frame)) {
        return false;
    }

    return framestep_publish(fs, &frame, pic);
}

bool framestep_get_frame(FrameStep_t *fs, VIDEO_FRAME_INFO_S *frame) {
    if (!fs || !frame) {
        return false;
    }

    if (fs->hasPending) {
        *frame = fs->pending;
        fs->hasPending = false;
        return true;
    }

    return framestep_decodeOne(fs, frame);
}

bool framestep_get_frame_at(FrameStep_t *fs, int ms, VIDEO_FRAME_INFO_S *frame) {
    if (!fs || !frame) {
        return false;
    }

    return framestep_reach(fs, ms, frame);
}

void framestep_put_frame(FrameStep_t *fs, VIDEO_FRAME_INFO_S *frame) {
    if (fs && frame) {
        framestep_release(fs, frame);
    }
}

void framestep_unget_frame(FrameStep_t *fs, VIDEO_FRAME_INFO_S *frame) {
    if (!fs || !frame) {
        return;
    }
    if (fs->hasPending) {
        // Two frames cannot be next; the later one goes back to the pool.
        LOGW("a frame is already next; %dms dropped", framestep_frameMs(frame));
        framestep_release(fs, frame);
        return;
    }
    fs->pending = *frame;
    fs->hasPending = true;
}

int framestep_duration_ms(FrameStep_t *fs) {
    if (!fs || !fs->fmt || fs->fmt->duration == AV_NOPTS_VALUE || fs->fmt->duration <= 0) {
        return 0;
    }

    return (int)(fs->fmt->duration / (AV_TIME_BASE / 1000));
}

bool framestep_next(FrameStep_t *fs, FrameStepPic_t *pic) {
    VIDEO_FRAME_INFO_S frame;

    if (!fs || !pic) {
        return false;
    }

    // Stepping back read one frame too far to know where to stop. That frame
    // is this one.
    if (fs->hasPending) {
        frame = fs->pending;
        fs->hasPending = false;
        return framestep_publish(fs, &frame, pic);
    }

    if (!fs->hasShown) {
        return framestep_to(fs, 0, pic);
    }
    if (!framestep_decodeOne(fs, &frame)) {
        LOGD("no frame after %dms", fs->shownMs);
        return false;
    }

    return framestep_publish(fs, &frame, pic);
}

// The last frame before wasMs, left out of the pool for the caller.
static bool framestep_prevRaw(FrameStep_t *fs, int wasMs, VIDEO_FRAME_INFO_S *out) {
    VIDEO_FRAME_INFO_S frame;
    VIDEO_FRAME_INFO_S candidate;

    // Keep the last frame before the one on screen. Going by what comes out
    // of the decoder rather than by arithmetic on the frame interval is what
    // makes this exact whether or not the stream reorders frames.
    //
    // Where to start decoding is the whole cost of a step back. The frames
    // wanted are the ones from the keyframe before wasMs up to wasMs, and the
    // keyframe's position is known from feeding it (keyMs). A transport
    // stream seek lands wherever, and the decoder then skips to the next
    // keyframe, so the seek is aimed a little short of the one wanted: it
    // lands before it, and the decoder starts exactly there. When the frame
    // on screen is itself a keyframe, the one before it is wanted instead,
    // and only its spacing is known -- a second, from the recorder.
    //
    // Failing all that, back off further and further until something turns
    // up, which is what this did before keyframes were tracked.
    const int key = framestep_keyBefore(fs, wasMs - 1);
    const int aimed = (key != FRAMESTEP_noKEY) ? key - FRAMESTEP_seekSHORT_MS
                                               : wasMs - 1000 - FRAMESTEP_seekSHORT_MS;
    const int backOffMs[] = {wasMs - aimed, 2000, 8000, 30000};

    for (size_t attempt = 0; attempt < sizeof(backOffMs) / sizeof(backOffMs[0]); attempt++) {
        const int from = wasMs - backOffMs[attempt];
        bool haveCandidate = false;

        if (!framestep_seekBefore(fs, from)) {
            break;
        }

        for (;;) {
            if (!framestep_decodeOne(fs, &frame)) {
                break;
            }
            if (framestep_frameMs(&frame) >= wasMs) {
                // One past the end. Kept for the next step forward.
                fs->pending = frame;
                fs->hasPending = true;
                break;
            }
            if (haveCandidate) {
                framestep_release(fs, &candidate);
            }
            candidate = frame;
            haveCandidate = true;
        }

        if (haveCandidate) {
            *out = candidate;
            return true;
        }
        if (from <= 0) {
            break; // already at the start of the file
        }
        LOGD("nothing before %dms from %dms, reaching further back", wasMs, from);
    }

    // Nothing found, and the decoder has been moved. Put it back just past the
    // picture on screen, which is still up, so that the next step forward is
    // the frame after it. (Kept at the picture itself, the first step forward
    // after a step back at the start showed the same frame again.)
    LOGD("no frame before %dms", wasMs);
    if (framestep_seekBefore(fs, wasMs)) {
        while (framestep_decodeOne(fs, &frame)) {
            if (framestep_frameMs(&frame) > wasMs) {
                fs->pending = frame;
                fs->hasPending = true;
                break;
            }
            framestep_release(fs, &frame);
        }
    }

    return false;
}

bool framestep_prev(FrameStep_t *fs, FrameStepPic_t *pic) {
    VIDEO_FRAME_INFO_S frame;

    if (!fs || !pic) {
        return false;
    }
    if (!fs->hasShown) {
        return framestep_to(fs, 0, pic);
    }
    if (!framestep_prevRaw(fs, fs->shownMs, &frame)) {
        return false;
    }

    return framestep_publish(fs, &frame, pic);
}

bool framestep_get_prev_frame(FrameStep_t *fs, int before_ms, VIDEO_FRAME_INFO_S *frame) {
    if (!fs || !frame) {
        return false;
    }

    return framestep_prevRaw(fs, before_ms, frame);
}
