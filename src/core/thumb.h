/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_THUMB_H
#define PALMVIEW_THUMB_H

#include <PalmOS.h>

/* YouTube's smallest thumbnail, default.jpg, is 120x90: a 16:9 picture
 * between black bars. Only the picture is kept. */
#define THUMB_WIDTH 120
#define THUMB_HEIGHT 68
/* For the search list: the middle of the picture, drawn at full size */
#define THUMB_LIST_WIDTH 90
#define THUMB_LIST_HEIGHT 54
/* default.jpg is 2-4 KB. */
#define THUMB_JPEG_CAPACITY 8192
/* Working memory for the decoder */
#define THUMB_WORK_SIZE 3600

/* Decodes a 120x90 JPEG into RGB565 pixels, in the CPU's byte order, rows
 * packed: THUMB_WIDTH x THUMB_HEIGHT, or the list size. Returns false for
 * other sizes and for data the decoder rejects. */
Boolean ThumbDecode(const UInt8 *jpegP, UInt16 length, UInt16 *pixelsP,
                    void *workP, Boolean list);

/* Draws a duration such as "12:34" on a list-size thumbnail, white on a
 * dark box in its bottom right corner. Characters other than digits and
 * colons are skipped. */
void ThumbBadge(UInt16 *pixelsP, const Char *textP);

/* Fills a list-size thumbnail with light gray and puts the duration on it,
 * for a result whose picture has not arrived. */
void ThumbPlaceholder(UInt16 *pixelsP, const Char *textP);

#endif
