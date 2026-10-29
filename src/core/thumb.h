/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_THUMB_H
#define PALMVIEW_THUMB_H

#include <PalmOS.h>
#include "thumb_decode.h"

/* Draws a duration such as "12:34" on a list-size thumbnail, white on a
 * dark box in its bottom right corner. Characters other than digits and
 * colons are skipped. */
void ThumbBadge(UInt16 *pixelsP, const Char *textP);

/* Fills a list-size thumbnail with light gray and puts the duration on it,
 * for a result whose picture has not arrived. */
void ThumbPlaceholder(UInt16 *pixelsP, const Char *textP);

#endif
