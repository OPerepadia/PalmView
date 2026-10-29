/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_THUMB_DECODE_H
#define PALMVIEW_THUMB_DECODE_H

#include <stdint.h>

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
 * packed: THUMB_WIDTH x THUMB_HEIGHT, or the list size. Returns 0 for
 * other sizes and for data the decoder rejects. Needs no Palm OS headers:
 * the ARM thumbnail module builds it too. */
int ThumbDecode(const uint8_t *jpegP, uint16_t length, uint16_t *pixelsP,
                void *workP, int list);

#endif
