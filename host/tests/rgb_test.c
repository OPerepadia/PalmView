/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/arm/decoder.c"

static uint32_t gRandom = 1;
static unsigned gCases;

static uint8_t RandomByte(void)
{
    gRandom ^= gRandom << 13;
    gRandom ^= gRandom >> 17;
    gRandom ^= gRandom << 5;
    return (uint8_t)gRandom;
}

static unsigned Component(int y, int delta)
{
    double value = floor((y + delta - 16) * (76284.0 / 65536.0) + 0.5);
    return value < 0 ? 0 : value > 255 ? 255 : (unsigned)value;
}

static uint16_t ReferencePixel(int y, int u, int v)
{
    /* Preserve the converter's rounding of each chroma contribution. */
    int r = (int)round((v - 128) * (104595.0 / 76284.0));
    int g = -(int)round((u - 128) * (25625.0 / 76284.0))
        - (int)round((v - 128) * (53281.0 / 76284.0));
    int b = (int)round((u - 128) * (132252.0 / 76284.0));
    return (uint16_t)(((Component(y, r) >> 3) << 11) |
        ((Component(y, g) >> 2) << 5) | (Component(y, b) >> 3));
}

/* Turned, the picture's rows run down the screen's columns, right to
 * left. */
static void CheckFrame(unsigned sourceWidth, unsigned sourceHeight,
    unsigned width, unsigned height, unsigned padding, uint32_t flags)
{
    AVFrame frame = {0};
    int turn = (flags & DEC_RGB_TURN) != 0;
    unsigned stride = 2 * (turn ? height : width) + padding;
    unsigned offset = 16 + (padding ? 2 : 0);
    size_t size = offset + stride * (turn ? width : height) + 16;
    uint8_t *actualP = malloc(size), *expectedP = malloc(size);
    unsigned x, y, plane;
    if (actualP == 0 || expectedP == 0) abort();
    frame.width = (int)sourceWidth;
    frame.height = (int)sourceHeight;
    frame.format = padding ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_YUV420P;
    for (plane = 0; plane < 3; plane++) {
        unsigned w = plane ? (sourceWidth + 1) / 2 : sourceWidth;
        unsigned h = plane ? (sourceHeight + 1) / 2 : sourceHeight;
        size_t i, bytes;
        frame.linesize[plane] = (int)(w + (padding ? padding + plane : 0));
        bytes = (size_t)frame.linesize[plane] * h;
        frame.data[plane] = malloc(bytes);
        if (frame.data[plane] == 0) abort();
        for (i = 0; i < bytes; i++) frame.data[plane][i] = RandomByte();
    }
    memset(actualP, 0xA5, size);
    memset(expectedP, 0xA5, size);
    for (y = 0; y < height; y++) {
        unsigned sy = (unsigned)((y + 0.5) * sourceHeight / height);
        for (x = 0; x < width; x++) {
            unsigned sx = (unsigned)((x + 0.5) * sourceWidth / width);
            uint16_t pixel = ReferencePixel(frame.data[0][sy * frame.linesize[0] + sx],
                frame.data[1][(sy / 2) * frame.linesize[1] + sx / 2],
                frame.data[2][(sy / 2) * frame.linesize[2] + sx / 2]);
            uint8_t *outP = expectedP + offset + (turn
                ? x * stride + 2 * (height - 1 - y) : y * stride + 2 * x);
            outP[0] = (uint8_t)(flags & DEC_RGB_BIG_ENDIAN ? pixel >> 8 : pixel);
            outP[1] = (uint8_t)(flags & DEC_RGB_BIG_ENDIAN ? pixel : pixel >> 8);
        }
    }
    gVideo.queueHead = 1;
    gVideo.queueCount = 1;
    gVideo.queue[1] = &frame;
    if (DecVideoToRgb565(actualP + offset + (turn ? 2 * (height - 1) : 0), stride,
            width, height, flags) != 0 ||
        memcmp(actualP, expectedP, size) != 0) {
        fprintf(stderr, "RGB mismatch: %ux%u to %ux%u, padding %u, flags %u\n",
            sourceWidth, sourceHeight, width, height, padding, flags);
        exit(1);
    }
    gVideo.queue[1] = 0;
    gVideo.queueCount = 0;
    for (plane = 0; plane < 3; plane++) free(frame.data[plane]);
    free(actualP);
    free(expectedP);
    gCases++;
}

/* A turned picture wider than its band buffer is refused. */
static void CheckTurnTooWide(void)
{
    static uint8_t plane[DEC_RGB_MAX_SOURCE], out[4];
    AVFrame frame = {0};
    unsigned i;
    frame.width = DEC_RGB_MAX_SOURCE;
    frame.height = 1;
    frame.format = AV_PIX_FMT_YUV420P;
    for (i = 0; i < 3; i++) {
        frame.data[i] = plane;
        frame.linesize[i] = DEC_RGB_MAX_SOURCE;
    }
    gVideo.queueHead = 1;
    gVideo.queueCount = 1;
    gVideo.queue[1] = &frame;
    if (DecVideoToRgb565(out + 2, 2, DEC_RGB_MAX_TURNED + 1, 1, DEC_RGB_TURN) == 0) {
        fprintf(stderr, "turned picture too wide was not refused\n");
        exit(1);
    }
    gVideo.queue[1] = 0;
    gVideo.queueCount = 0;
}

int main(void)
{
    static const unsigned sizes[][4] = {
        {256, 144, 320, 180}, {257, 145, 321, 181}, {426, 240, 320, 180},
        {256, 144, 256, 144}, {255, 143, 128, 72},
        {256, 144, 512, 288}, {31, 17, 124, 68},
        {DEC_RGB_MAX_SOURCE, 3, 4 * DEC_RGB_MAX_SOURCE, 12},
        {DEC_RGB_MAX_SOURCE, 3, 1, 1}, {426, 240, 426, 240}, {256, 144, 480, 270}
    };
    unsigned flags, padding, sw, sh, w, h, i;
    for (flags = 0; flags <= (DEC_RGB_BIG_ENDIAN | DEC_RGB_TURN); flags++) {
        for (padding = 0; padding <= 6; padding += 6) {
            for (sw = 1; sw <= 9; sw++)
                for (sh = 1; sh <= 7; sh++)
                    for (w = 1; w <= 4 * sw; w++)
                        for (h = 1; h <= 4 * sh; h++)
                            CheckFrame(sw, sh, w, h, padding, flags);
            for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
                if (!(flags & DEC_RGB_TURN) || sizes[i][2] <= DEC_RGB_MAX_TURNED)
                    CheckFrame(sizes[i][0], sizes[i][1], sizes[i][2], sizes[i][3],
                        padding, flags);
        }
    }
    CheckFrame(9, 7, 13, 11, 0, 0x80);
    CheckFrame(9, 7, 13, 11, 6, 0x80 | DEC_RGB_BIG_ENDIAN);
    CheckTurnTooWide();
    printf("rgb tests ok: %u conversions\n", gCases);
    return 0;
}
