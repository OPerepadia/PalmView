/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "thumb.h"
#include "thumb_font.h"

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
