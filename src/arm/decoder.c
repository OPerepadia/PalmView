/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "decoder.h"

#include <string.h>
#include "libavcodec/avcodec.h"
#include "libavutil/mem.h"

#define ADLER_MOD 65521U

typedef struct DecStream {
    AVCodecContext *ctxP;
    AVPacket *packetP;
    AVFrame *frameP;
    /* Decoded video frames, oldest first */
    AVFrame *queue[DEC_MAX_QUEUE];
    uint32_t queueHead;
    uint32_t queueCount;
    uint32_t queueDepth;
    uint32_t count;
    int draining;
    uint32_t adlerA;
    uint32_t adlerB;
} DecStream;

/* RGB565 conversion tables, see InitRgbTables. */
#define RGB_TABLE_BIAS 256
#define RGB_TABLE_SIZE 768

static DecStream gVideo;
static DecStream gAudio;
static uint32_t gRgbTable[3 * RGB_TABLE_SIZE];
static uint16_t gChroma[512][2];
static uint8_t gRepeat[DEC_RGB_MAX_SOURCE];
static uint32_t gRgbTableFlags;
static int gRgbTablesReady;

static void Adler(DecStream *s, const uint8_t *p, uint32_t length)
{
    uint32_t a = s->adlerA, b = s->adlerB;
    while (length != 0) {
        uint32_t n = length < 3800 ? length : 3800;
        length -= n;
        while (n-- != 0) {
            a += *p++;
            b += a;
        }
        a %= ADLER_MOD;
        b %= ADLER_MOD;
    }
    s->adlerA = a;
    s->adlerB = b;
}

static void CloseStream(DecStream *s)
{
    uint32_t i;
    avcodec_free_context(&s->ctxP);
    av_packet_free(&s->packetP);
    av_frame_free(&s->frameP);
    for (i = 0; i < DEC_MAX_QUEUE; i++) av_frame_free(&s->queue[i]);
    memset(s, 0, sizeof(*s));
}

static int OpenStream(DecStream *s, enum AVCodecID id, const uint8_t *configP,
                      uint32_t length)
{
    const AVCodec *codecP = avcodec_find_decoder(id);
    CloseStream(s);
    s->adlerA = 1;
    if (codecP == 0) return decErrNoDecoder;
    s->ctxP = avcodec_alloc_context3(codecP);
    s->packetP = av_packet_alloc();
    s->frameP = av_frame_alloc();
    if (s->ctxP == 0 || s->packetP == 0 || s->frameP == 0) goto no_memory;
    if (length != 0) {
        s->ctxP->extradata = av_mallocz(length + AV_INPUT_BUFFER_PADDING_SIZE);
        if (s->ctxP->extradata == 0) goto no_memory;
        memcpy(s->ctxP->extradata, configP, length);
        s->ctxP->extradata_size = (int)length;
    }
    s->ctxP->thread_count = 1;
    return 0;
no_memory:
    CloseStream(s);
    return decErrNoMemory;
}

int DecVideoOpen(const uint8_t *avccP, uint32_t length, uint32_t queueDepth)
{
    int result = OpenStream(&gVideo, AV_CODEC_ID_H264, avccP, length);
    uint32_t i;
    if (result != 0) return result;
    if (queueDepth < 1) queueDepth = 1;
    if (queueDepth > DEC_MAX_QUEUE) queueDepth = DEC_MAX_QUEUE;
    gVideo.queueDepth = queueDepth;
    for (i = 0; i < queueDepth; i++) {
        gVideo.queue[i] = av_frame_alloc();
        if (gVideo.queue[i] == 0) {
            CloseStream(&gVideo);
            return decErrNoMemory;
        }
    }
    if (avcodec_open2(gVideo.ctxP, 0, 0) < 0) {
        CloseStream(&gVideo);
        return decErrOpen;
    }
    return 0;
}

static void ChecksumFrame(DecStream *s, const AVFrame *f)
{
    int plane;
    for (plane = 0; plane < 3; plane++) {
        int w = plane == 0 ? f->width : (f->width + 1) >> 1;
        int h = plane == 0 ? f->height : (f->height + 1) >> 1;
        const uint8_t *rowP = f->data[plane];
        int y;
        for (y = 0; y < h; y++, rowP += f->linesize[plane])
            Adler(s, rowP, (uint32_t)w);
    }
}

/* FFmpeg reads these per slice, so they can change between samples. */
static void SetVideoSkip(uint32_t flags)
{
    AVCodecContext *ctxP = gVideo.ctxP;
    ctxP->skip_loop_filter = (flags & DEC_SKIP_DEBLOCK) ? AVDISCARD_ALL
        : (flags & DEC_SKIP_DEBLOCK_NONREF) ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
    ctxP->skip_frame = (flags & DEC_SKIP_NONREF) ? AVDISCARD_NONREF
        : AVDISCARD_DEFAULT;
}

/* Adds the frame just decoded to the queue, dropping the oldest when the
 * queue is full. */
static void QueueFrame(DecStream *s)
{
    uint32_t tail;
    if (s->queueCount == s->queueDepth) {
        av_frame_unref(s->queue[s->queueHead]);
        s->queueHead = (s->queueHead + 1) % s->queueDepth;
        s->queueCount--;
    }
    tail = (s->queueHead + s->queueCount) % s->queueDepth;
    av_frame_move_ref(s->queue[tail], s->frameP);
    s->queueCount++;
}

static const AVFrame *FrontFrame(const DecStream *s)
{
    return s->queueCount != 0 ? s->queue[s->queueHead] : 0;
}

int DecVideoDecode(const uint8_t *dataP, uint32_t length, uint32_t pts,
                   uint32_t flags)
{
    int frames = 0, result;
    if (gVideo.ctxP == 0) return decErrState;
    SetVideoSkip(flags);
    if (dataP != 0) {
        gVideo.packetP->data = (uint8_t *)dataP;
        gVideo.packetP->size = (int)length;
        gVideo.packetP->pts = pts;
        result = avcodec_send_packet(gVideo.ctxP, gVideo.packetP);
    } else if (!gVideo.draining) {
        gVideo.draining = 1;
        result = avcodec_send_packet(gVideo.ctxP, 0);
    } else {
        result = 0;
    }
    if (result < 0 && result != AVERROR_EOF) return decErrDecode;
    /* Draining gives one frame per call, so a player can show each. */
    while (dataP != 0 || frames == 0) {
        result = avcodec_receive_frame(gVideo.ctxP, gVideo.frameP);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
        if (result < 0) return decErrDecode;
        if (flags & DEC_CHECKSUM) ChecksumFrame(&gVideo, gVideo.frameP);
        gVideo.count++;
        frames++;
        QueueFrame(&gVideo);
    }
    return frames;
}

uint32_t DecVideoQueued(uint32_t *frontPtsP)
{
    const AVFrame *f = FrontFrame(&gVideo);
    *frontPtsP = f != 0 && f->pts != AV_NOPTS_VALUE ? (uint32_t)f->pts : 0;
    return gVideo.queueCount;
}

void DecVideoPop(void)
{
    if (gVideo.queueCount == 0) return;
    av_frame_unref(gVideo.queue[gVideo.queueHead]);
    gVideo.queueHead = (gVideo.queueHead + 1) % gVideo.queueDepth;
    gVideo.queueCount--;
}

void DecVideoGetInfo(DecVideoInfo *infoP)
{
    infoP->frames = gVideo.count;
    infoP->checksum = (gVideo.adlerB << 16) | gVideo.adlerA;
    infoP->width = gVideo.ctxP != 0 ? (uint16_t)gVideo.ctxP->width : 0;
    infoP->height = gVideo.ctxP != 0 ? (uint16_t)gVideo.ctxP->height : 0;
}

/* Integer BT.601, limited range. Chroma is folded into the luma index:
 * R = clamp(1.164 * (Y + dR(V) - 16)), and likewise for G and B.
 * gRgbTable has an R, a G and a B section, each holding that color's bits
 * in place, so a pixel is the sum of three reads. The bits do not overlap,
 * so byte-swapped entries give big-endian pixels.
 * gChroma[u] holds byte offsets into gRgbTable for B and half of G;
 * gChroma[256 + v] for R and the other half of G. */
static int32_t ChromaStep(int32_t c, int32_t factor)
{
    int32_t v = c * factor;
    return (v + (v < 0 ? -38142 : 38142)) / 76284;
}

static uint32_t Swap16(uint32_t v)
{
    return ((v >> 8) & 0xFF) | ((v & 0xFF) << 8);
}

static void InitRgbTables(uint32_t flags)
{
    int i;
    for (i = 0; i < RGB_TABLE_SIZE; i++) {
        int32_t v = (76284 * (i - RGB_TABLE_BIAS - 16) + 32768) >> 16;
        uint32_t r, g, b;
        v = v < 0 ? 0 : v > 255 ? 255 : v;
        r = ((uint32_t)v >> 3) << 11;
        g = ((uint32_t)v >> 2) << 5;
        b = (uint32_t)v >> 3;
        if (flags & DEC_RGB_BIG_ENDIAN) {
            r = Swap16(r);
            g = Swap16(g);
            b = Swap16(b);
        }
        gRgbTable[i] = r;
        gRgbTable[RGB_TABLE_SIZE + i] = g;
        gRgbTable[2 * RGB_TABLE_SIZE + i] = b;
    }
    for (i = 0; i < 256; i++) {
        int32_t c = i - 128;
        gChroma[i][0] = (uint16_t)(4 * (2 * RGB_TABLE_SIZE + RGB_TABLE_BIAS +
            ChromaStep(c, 132252)));
        gChroma[i][1] = (uint16_t)(4 * (RGB_TABLE_BIAS / 2 - ChromaStep(c, 25625)));
        gChroma[256 + i][0] = (uint16_t)(4 * (RGB_TABLE_BIAS + ChromaStep(c, 104595)));
        gChroma[256 + i][1] = (uint16_t)(4 * (RGB_TABLE_SIZE + RGB_TABLE_BIAS / 2 -
            ChromaStep(c, 53281)));
    }
    gRgbTableFlags = flags;
    gRgbTablesReady = 1;
}

/* Stores pixel p n times. The fast form always stores two pixels and
 * advances by n, which is right for n of 1 or 2 when another pixel
 * follows. */
static inline __attribute__((always_inline)) uint16_t *Put(uint16_t *o,
    uint32_t p, uint32_t n, int fast)
{
    if (fast) {
        o[0] = (uint16_t)p;
        o[1] = (uint16_t)p;
        return o + n;
    }
    while (n-- != 0) *o++ = (uint16_t)p;
    return o;
}

#define TABLE_AT(offset) ((const uint32_t *)((const uint8_t *)tableP + (offset)))

/* Converts one source row into one output row, or two when dual. Source
 * pixel x covers gRepeat[x] output pixels. */
static inline __attribute__((always_inline)) void ConvertRow(uint16_t *o0,
    uint16_t *o1, const uint8_t *yRowP, const uint8_t *uRowP,
    const uint8_t *vRowP, uint32_t sourceWidth, int fast, int dual)
{
    const uint32_t *tableP = gRgbTable;
    const uint16_t (*chromaP)[2] = gChroma;
    const uint8_t *yP = yRowP, *uP = uRowP, *vP = vRowP, *repeatP = gRepeat;
    /* The fast loop leaves the last two source pixels to the exact one. */
    uint32_t x = fast && sourceWidth >= 4 ? (sourceWidth - 2) & ~1U : 0;
    const uint8_t *fastEndP = yRowP + x;
    while (yP < fastEndP) {
        const uint16_t *cuP = chromaP[*uP++], *cvP = chromaP[256 + *vP++];
        const uint32_t *rP = TABLE_AT(cvP[0]);
        const uint32_t *gP = TABLE_AT(cvP[1] + cuP[1]);
        const uint32_t *bP = TABLE_AT(cuP[0]);
        uint32_t y0 = yP[0], y1 = yP[1];
        uint32_t p0 = rP[y0] + gP[y0] + bP[y0];
        uint32_t p1 = rP[y1] + gP[y1] + bP[y1];
        uint32_t n0 = repeatP[0], n1 = repeatP[1];
        yP += 2;
        repeatP += 2;
        o0 = Put(o0, p0, n0, 1);
        if (dual) o1 = Put(o1, p0, n0, 1);
        o0 = Put(o0, p1, n1, 1);
        if (dual) o1 = Put(o1, p1, n1, 1);
    }
    for (; x < sourceWidth; x++) {
        const uint16_t *cuP = chromaP[uRowP[x >> 1]];
        const uint16_t *cvP = chromaP[256 + vRowP[x >> 1]];
        uint32_t y = yRowP[x];
        uint32_t p = TABLE_AT(cvP[0])[y] + TABLE_AT(cvP[1] + cuP[1])[y] +
            TABLE_AT(cuP[0])[y];
        o0 = Put(o0, p, gRepeat[x], 0);
        if (dual) o1 = Put(o1, p, gRepeat[x], 0);
    }
}

static void ConvertRowFast(uint16_t *o0, const uint8_t *yP, const uint8_t *uP,
                           const uint8_t *vP, uint32_t sourceWidth)
{
    ConvertRow(o0, 0, yP, uP, vP, sourceWidth, 1, 0);
}

static void ConvertRowFastDual(uint16_t *o0, uint16_t *o1, const uint8_t *yP,
                               const uint8_t *uP, const uint8_t *vP,
                               uint32_t sourceWidth)
{
    ConvertRow(o0, o1, yP, uP, vP, sourceWidth, 1, 1);
}

static void ConvertRowExact(uint16_t *o0, const uint8_t *yP, const uint8_t *uP,
                            const uint8_t *vP, uint32_t sourceWidth)
{
    ConvertRow(o0, 0, yP, uP, vP, sourceWidth, 0, 0);
}

/* Nearest source index for output index i of n, sampling pixel centers. */
static uint32_t SourceIndex(uint32_t i, uint32_t n, uint32_t sourceN)
{
    return ((2 * i + 1) * sourceN) / (2 * n);
}

int DecVideoToRgb565(uint8_t *dstP, uint32_t stride, uint32_t width,
                     uint32_t height, uint32_t flags)
{
    const AVFrame *f = FrontFrame(&gVideo);
    uint32_t sourceWidth, sourceHeight, x, y;
    int fast = 1;
    if (f == 0 || f->data[0] == 0 || width == 0 || height == 0)
        return decErrState;
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P)
        return decErrState;
    sourceWidth = (uint32_t)f->width;
    sourceHeight = (uint32_t)f->height;
    if (sourceWidth > DEC_RGB_MAX_SOURCE || width > 4 * sourceWidth ||
        height > 4 * sourceHeight || (((uintptr_t)dstP | stride) & 1) != 0)
        return decErrState;
    flags &= DEC_RGB_BIG_ENDIAN;
    if (!gRgbTablesReady || gRgbTableFlags != flags) InitRgbTables(flags);
    memset(gRepeat, 0, sourceWidth);
    for (x = 0; x < width; x++) gRepeat[SourceIndex(x, width, sourceWidth)]++;
    for (x = 0; x < sourceWidth; x++)
        if (gRepeat[x] == 0 || gRepeat[x] > 2) fast = 0;
    for (y = 0; y < height;) {
        uint32_t sy = SourceIndex(y, height, sourceHeight), rows = 1;
        const uint8_t *yP = f->data[0] + sy * (uint32_t)f->linesize[0];
        const uint8_t *uP = f->data[1] + (sy >> 1) * (uint32_t)f->linesize[1];
        const uint8_t *vP = f->data[2] + (sy >> 1) * (uint32_t)f->linesize[2];
        while (y + rows < height && SourceIndex(y + rows, height, sourceHeight) == sy)
            rows++;
        for (; rows != 0; rows--, y++) {
            uint16_t *o0 = (uint16_t *)(dstP + y * stride);
            if (!fast) {
                ConvertRowExact(o0, yP, uP, vP, sourceWidth);
            } else if (rows >= 2) {
                ConvertRowFastDual(o0, (uint16_t *)(dstP + (y + 1) * stride),
                    yP, uP, vP, sourceWidth);
                rows--;
                y++;
            } else {
                ConvertRowFast(o0, yP, uP, vP, sourceWidth);
            }
        }
    }
    return 0;
}

void DecVideoClose(void)
{
    CloseStream(&gVideo);
}

int DecAudioOpen(const uint8_t *configP, uint32_t length)
{
    int result = OpenStream(&gAudio, AV_CODEC_ID_AAC, configP, length);
    if (result != 0) return result;
    if (avcodec_open2(gAudio.ctxP, 0, 0) < 0) {
        CloseStream(&gAudio);
        return decErrOpen;
    }
    return 0;
}

static int16_t ToS16(int32_t sample)
{
    return (int16_t)(sample >> 16);
}

/* Interleaves one decoded frame into pcmP as 16-bit samples. */
static int StoreAudio(DecStream *s, const AVFrame *f, int16_t *pcmP,
                      uint32_t flags)
{
    int stride = f->ch_layout.nb_channels, channels = stride > 2 ? 2 : stride;
    int n = f->nb_samples, i, c;
    if (n > 2048) return decErrDecode;
    for (c = 0; c < channels; c++) {
        int16_t *outP = pcmP + c;
        switch (f->format) {
            case AV_SAMPLE_FMT_S32P: {
                const int32_t *inP = (const int32_t *)f->extended_data[c];
                for (i = 0; i < n; i++, outP += channels) *outP = ToS16(inP[i]);
                break;
            }
            case AV_SAMPLE_FMT_S32: {
                const int32_t *inP = (const int32_t *)f->data[0] + c;
                for (i = 0; i < n; i++, outP += channels, inP += stride)
                    *outP = ToS16(*inP);
                break;
            }
            case AV_SAMPLE_FMT_S16P: {
                const int16_t *inP = (const int16_t *)f->extended_data[c];
                for (i = 0; i < n; i++, outP += channels) *outP = inP[i];
                break;
            }
            case AV_SAMPLE_FMT_S16: {
                const int16_t *inP = (const int16_t *)f->data[0] + c;
                for (i = 0; i < n; i++, outP += channels, inP += stride) *outP = *inP;
                break;
            }
            default:
                return decErrDecode;
        }
    }
    if (flags & DEC_CHECKSUM)
        Adler(s, (const uint8_t *)pcmP, (uint32_t)(n * channels * 2));
    s->count += (uint32_t)n;
    return n;
}

int DecAudioDecode(const uint8_t *dataP, uint32_t length, int16_t *pcmP,
                   uint32_t flags)
{
    int samples = 0, result;
    if (gAudio.ctxP == 0) return decErrState;
    gAudio.packetP->data = (uint8_t *)dataP;
    gAudio.packetP->size = (int)length;
    if (avcodec_send_packet(gAudio.ctxP, gAudio.packetP) < 0)
        return decErrDecode;
    for (;;) {
        int n;
        result = avcodec_receive_frame(gAudio.ctxP, gAudio.frameP);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
        if (result < 0) return decErrDecode;
        /* AAC gives one frame per packet; later ones would overwrite. */
        n = StoreAudio(&gAudio, gAudio.frameP, pcmP, flags);
        av_frame_unref(gAudio.frameP);
        if (n < 0) return n;
        samples = n;
    }
    return samples;
}

void DecAudioGetInfo(DecAudioInfo *infoP)
{
    infoP->samples = gAudio.count;
    infoP->checksum = (gAudio.adlerB << 16) | gAudio.adlerA;
    infoP->channels = gAudio.ctxP != 0
        ? (uint16_t)gAudio.ctxP->ch_layout.nb_channels : 0;
    infoP->sampleRate = gAudio.ctxP != 0
        ? (uint16_t)gAudio.ctxP->sample_rate : 0;
}

void DecAudioClose(void)
{
    CloseStream(&gAudio);
}
