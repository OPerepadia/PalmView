/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_THUMB_H
#define PALMVIEW_THUMB_H

#include <PalmOS.h>

/* YouTube's smallest thumbnail, default.jpg, is 120x90: a 16:9 picture
 * between black bars. Only the picture is kept. */
#define THUMB_WIDTH 120
#define THUMB_HEIGHT 68
/* At half size, without the rows that blend into the bars */
#define THUMB_SMALL_WIDTH 60
#define THUMB_SMALL_HEIGHT 33
/* default.jpg is 2-4 KB. */
#define THUMB_JPEG_CAPACITY 8192
/* Working memory for the decoder */
#define THUMB_WORK_SIZE 3600

/* Decodes a 120x90 JPEG into RGB565 pixels, in the CPU's byte order, rows
 * packed: THUMB_WIDTH x THUMB_HEIGHT, or the small size. Returns false for
 * other sizes and for data the decoder rejects. */
Boolean ThumbDecode(const UInt8 *jpegP, UInt16 length, UInt16 *pixelsP,
                    void *workP, Boolean small);

#endif
