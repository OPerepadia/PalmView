/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_RGB_H
#define PALMVIEW_RGB_H

#include <stdint.h>
#include "libavutil/frame.h"

/* Converts frame f to RGB565, as DecVideoToRgb565 describes. */
int RgbConvert(const AVFrame *f, uint8_t *dstP, uint32_t stride, uint32_t width,
               uint32_t height, uint32_t flags);

#endif
