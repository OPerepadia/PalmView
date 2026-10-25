/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "thumb.h"
#include "tjpgd.h"

#define SOURCE_HEIGHT 90
#define TOP_BAR ((SOURCE_HEIGHT - THUMB_HEIGHT) / 2)
/* Half-size rows 6 to 38 come from picture rows 12 to 77 only. */
#define SMALL_TOP 6

typedef struct ThumbDecoding {
    const UInt8 *dataP;
    UInt16 left;
    UInt16 *pixelsP;
    UInt16 width, top, height;
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
    d->left -= (UInt16)length;
    return length;
}

/* Copies the picture's rows out of each decoded block. */
static int WritePixels(JDEC *decoderP, void *blockP, JRECT *rectP)
{
    ThumbDecoding *d = (ThumbDecoding *)decoderP->device;
    const uint16_t *srcP = (const uint16_t *)blockP;
    UInt16 width = (UInt16)(rectP->right - rectP->left + 1);
    UInt16 x, y;
    UInt16 *dstP;
    for (y = rectP->top; y <= rectP->bottom; y++, srcP += width) {
        if (y < d->top || y >= d->top + d->height) continue;
        dstP = d->pixelsP + (UInt32)(y - d->top) * d->width + rectP->left;
        for (x = 0; x < width; x++) dstP[x] = srcP[x];
    }
    return 1;
}

Boolean ThumbDecode(const UInt8 *jpegP, UInt16 length, UInt16 *pixelsP,
                    void *workP, Boolean small)
{
    JDEC decoder;
    ThumbDecoding d;
    d.dataP = jpegP;
    d.left = length;
    d.pixelsP = pixelsP;
    d.width = small ? THUMB_SMALL_WIDTH : THUMB_WIDTH;
    d.top = small ? SMALL_TOP : TOP_BAR;
    d.height = small ? THUMB_SMALL_HEIGHT : THUMB_HEIGHT;
    if (jd_prepare(&decoder, ReadJpeg, workP, THUMB_WORK_SIZE, &d) != JDR_OK ||
        decoder.width != THUMB_WIDTH || decoder.height != SOURCE_HEIGHT)
        return false;
    return jd_decomp(&decoder, WritePixels, small ? 1 : 0) == JDR_OK;
}
