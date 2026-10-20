/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_DECODER_H
#define PALMVIEW_DECODER_H

#include <stdint.h>

/* A thin layer over FFmpeg's h264 and aac_fixed decoders. It builds for the
 * host and for the Palm's ARM side, so both can be checked against each
 * other with the checksums below. Input buffers must have DEC_PADDING
 * readable bytes after the data. */

#define DEC_PADDING 64

enum {
    decOk = 0,
    decErrNoMemory = -1,
    decErrNoDecoder = -2,
    decErrOpen = -3,
    decErrDecode = -4,
    decErrState = -5
};

/* DecVideoDecode flags. Skip options trade quality for speed; they can
 * change on every call. */
#define DEC_SKIP_DEBLOCK 0x01          /* no in-loop deblocking filter */
#define DEC_SKIP_NONREF 0x02           /* drop frames nothing references */
#define DEC_SKIP_DEBLOCK_NONREF 0x04   /* no deblocking in those frames only */
#define DEC_CHECKSUM 0x80              /* add output frames to the checksum */

/* DecVideoToRgb565 flags */
#define DEC_RGB_BIG_ENDIAN 0x01        /* 68K byte order of Palm OS bitmaps */
#define DEC_RGB_MAX_SOURCE 1024

#define DEC_MAX_QUEUE 4

typedef struct DecVideoInfo {
    uint32_t frames;          /* frames output so far */
    uint32_t checksum;        /* Adler-32 over DEC_CHECKSUM frames */
    uint16_t width;
    uint16_t height;
} DecVideoInfo;

typedef struct DecAudioInfo {
    uint32_t samples;         /* samples per channel output so far */
    uint32_t checksum;        /* Adler-32 over DEC_CHECKSUM output */
    uint16_t channels;
    uint16_t sampleRate;
} DecAudioInfo;

/* Decoded frames wait in a queue of up to queueDepth (1 to DEC_MAX_QUEUE)
 * frames. When it is full, a new frame replaces the oldest. */
int DecVideoOpen(const uint8_t *avccP, uint32_t length, uint32_t queueDepth);
/* Decodes one sample; pts is its presentation time. At the end, call with
 * data 0 until it returns 0 to drain delayed frames one by one. Returns
 * the number of frames output, or a negative error. */
int DecVideoDecode(const uint8_t *dataP, uint32_t length, uint32_t pts,
                   uint32_t flags);
/* Returns the number of queued frames; *frontPtsP gets the oldest one's pts. */
uint32_t DecVideoQueued(uint32_t *frontPtsP);
/* Drops the oldest queued frame. */
void DecVideoPop(void);
void DecVideoGetInfo(DecVideoInfo *infoP);
/* Converts the oldest queued frame to RGB565, nearest-neighbor scaled to
 * width x height. dstP and stride (in bytes) must be even.
 * Frames wider than DEC_RGB_MAX_SOURCE are rejected. */
int DecVideoToRgb565(uint8_t *dstP, uint32_t stride, uint32_t width,
                     uint32_t height, uint32_t flags);
void DecVideoClose(void);

int DecAudioOpen(const uint8_t *configP, uint32_t length);
/* Decodes one AAC frame to interleaved 16-bit PCM in pcmP (room for
 * 2048 samples per channel), at most two channels. With DEC_CHECKSUM in
 * flags, the PCM is added to the checksum. Returns samples per channel, or
 * an error. */
int DecAudioDecode(const uint8_t *dataP, uint32_t length, int16_t *pcmP,
                   uint32_t flags);
void DecAudioGetInfo(DecAudioInfo *infoP);
void DecAudioClose(void);

#endif
