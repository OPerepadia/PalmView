/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "thumb_decode.h"
#include "tjpgd.h"

#define SOURCE_HEIGHT 90
#define TOP_BAR ((SOURCE_HEIGHT - THUMB_HEIGHT) / 2)
/* The list size is cut from the middle of the picture. */
#define LIST_LEFT ((THUMB_WIDTH - THUMB_LIST_WIDTH) / 2)
#define LIST_TOP (TOP_BAR + (THUMB_HEIGHT - THUMB_LIST_HEIGHT) / 2)

typedef struct ThumbDecoding {
    const uint8_t *dataP;
    uint16_t left;
    uint16_t *pixelsP;
    uint16_t column, width, top, height;
} ThumbDecoding;

/* Reads the next bytes, or skips them when bufferP is 0. */
static size_t ReadJpeg(JDEC *decoderP, uint8_t *bufferP, size_t length)
{
    ThumbDecoding *d = (ThumbDecoding *)decoderP->device;
    size_t i;
    if (length > d->left) length = d->left;
    if (bufferP != 0)
        for (i = 0; i < length; i++) bufferP[i] = d->dataP[i];
    d->dataP += length;
    d->left -= (uint16_t)length;
    return length;
}

/* Copies the wanted rows and columns out of each decoded block. */
static int WritePixels(JDEC *decoderP, void *blockP, JRECT *rectP)
{
    ThumbDecoding *d = (ThumbDecoding *)decoderP->device;
    const uint16_t *srcP = (const uint16_t *)blockP;
    uint16_t width = (uint16_t)(rectP->right - rectP->left + 1);
    uint16_t x, y, column;
    uint16_t *dstP;
    for (y = rectP->top; y <= rectP->bottom; y++, srcP += width) {
        if (y < d->top || y >= d->top + d->height) continue;
        dstP = d->pixelsP + (uint32_t)(y - d->top) * d->width - d->column;
        for (x = 0; x < width; x++) {
            column = rectP->left + x;
            if (column >= d->column && column < d->column + d->width)
                dstP[column] = srcP[x];
        }
    }
    return 1;
}

int ThumbDecode(const uint8_t *jpegP, uint16_t length, uint16_t *pixelsP,
                void *workP, int list)
{
    JDEC decoder;
    ThumbDecoding d;
    d.dataP = jpegP;
    d.left = length;
    d.pixelsP = pixelsP;
    d.column = list ? LIST_LEFT : 0;
    d.width = list ? THUMB_LIST_WIDTH : THUMB_WIDTH;
    d.top = list ? LIST_TOP : TOP_BAR;
    d.height = list ? THUMB_LIST_HEIGHT : THUMB_HEIGHT;
    if (jd_prepare(&decoder, ReadJpeg, workP, THUMB_WORK_SIZE, &d) != JDR_OK ||
        decoder.width != THUMB_WIDTH || decoder.height != SOURCE_HEIGHT)
        return 0;
    return jd_decomp(&decoder, WritePixels, 0) == JDR_OK;
}
