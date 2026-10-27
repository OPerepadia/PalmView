/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "thumb.h"
#include "thumb_font.h"
#include "tjpgd.h"

#define SOURCE_HEIGHT 90
#define TOP_BAR ((SOURCE_HEIGHT - THUMB_HEIGHT) / 2)
/* The list size is cut from the middle of the picture. */
#define LIST_LEFT ((THUMB_WIDTH - THUMB_LIST_WIDTH) / 2)
#define LIST_TOP (TOP_BAR + (THUMB_HEIGHT - THUMB_LIST_HEIGHT) / 2)

typedef struct ThumbDecoding {
    const UInt8 *dataP;
    UInt16 left;
    UInt16 *pixelsP;
    UInt16 column, width, top, height;
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

/* Copies the wanted rows and columns out of each decoded block. */
static int WritePixels(JDEC *decoderP, void *blockP, JRECT *rectP)
{
    ThumbDecoding *d = (ThumbDecoding *)decoderP->device;
    const uint16_t *srcP = (const uint16_t *)blockP;
    UInt16 width = (UInt16)(rectP->right - rectP->left + 1);
    UInt16 x, y, column;
    UInt16 *dstP;
    for (y = rectP->top; y <= rectP->bottom; y++, srcP += width) {
        if (y < d->top || y >= d->top + d->height) continue;
        dstP = d->pixelsP + (UInt32)(y - d->top) * d->width - d->column;
        for (x = 0; x < width; x++) {
            column = rectP->left + x;
            if (column >= d->column && column < d->column + d->width)
                dstP[column] = srcP[x];
        }
    }
    return 1;
}

Boolean ThumbDecode(const UInt8 *jpegP, UInt16 length, UInt16 *pixelsP,
                    void *workP, Boolean list)
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
        return false;
    return jd_decomp(&decoder, WritePixels, 0) == JDR_OK;
}

/* RGB 0xE0E0E0 */
#define PLACEHOLDER_GRAY 0xE71C
#define BADGE_INSET 2
#define BADGE_PAD_X 3
#define BADGE_PAD_Y 2
/* A quarter of each color is left */
#define DARKEN_MASK 0x39E7

static Int16 GlyphIndex(Char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    return c == ':' ? 10 : -1;
}

/* White at a coverage of 0 to 15 over a pixel */
static UInt16 Lighten(UInt16 pixel, UInt16 coverage)
{
    UInt16 red = pixel >> 11, green = (pixel >> 5) & 0x3F, blue = pixel & 0x1F;
    red += (UInt16)((31 - red) * coverage / 15);
    green += (UInt16)((63 - green) * coverage / 15);
    blue += (UInt16)((31 - blue) * coverage / 15);
    return (UInt16)((red << 11) | (green << 5) | blue);
}

void ThumbBadge(UInt16 *pixelsP, const Char *textP)
{
    UInt16 textWidth = 0;
    Int16 x, y, left, top, right, bottom, glyph;
    const Char *charP;
    for (charP = textP; *charP != '\0'; charP++) {
        glyph = GlyphIndex(*charP);
        if (glyph >= 0) textWidth += kGlyphWidths[glyph];
    }
    if (textWidth == 0) return;
    right = THUMB_LIST_WIDTH - BADGE_INSET;
    bottom = THUMB_LIST_HEIGHT - BADGE_INSET;
    left = right - (Int16)textWidth - 2 * BADGE_PAD_X;
    top = bottom - FONT_ROWS - 2 * BADGE_PAD_Y;
    if (left < 0 || top < 0) return;
    for (y = top; y < bottom; y++)
        for (x = left; x < right; x++) {
            if ((x == left || x == right - 1) && (y == top || y == bottom - 1))
                continue;
            pixelsP[y * THUMB_LIST_WIDTH + x] =
                (pixelsP[y * THUMB_LIST_WIDTH + x] >> 2) & DARKEN_MASK;
        }
    x = left + BADGE_PAD_X;
    for (charP = textP; *charP != '\0'; charP++) {
        UInt16 row, column;
        glyph = GlyphIndex(*charP);
        if (glyph < 0) continue;
        for (row = 0; row < FONT_ROWS; row++)
            for (column = 0; column < kGlyphWidths[glyph]; column++) {
                UInt16 coverage = kGlyphs[glyph][row][column];
                UInt16 *pixelP = pixelsP + (top + BADGE_PAD_Y + row) *
                    THUMB_LIST_WIDTH + x + column;
                if (coverage != 0) *pixelP = Lighten(*pixelP, coverage);
            }
        x += kGlyphWidths[glyph];
    }
}

void ThumbPlaceholder(UInt16 *pixelsP, const Char *textP)
{
    UInt16 i;
    for (i = 0; i < THUMB_LIST_WIDTH * THUMB_LIST_HEIGHT; i++)
        pixelsP[i] = PLACEHOLDER_GRAY;
    ThumbBadge(pixelsP, textP);
}
