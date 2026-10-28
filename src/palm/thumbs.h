/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The search results' thumbnails: downloaded in the background over one
 * kept-open connection, those in view first, and decoded for the list and
 * the details screen. */

#ifndef PALMVIEW_THUMBS_H
#define PALMVIEW_THUMBS_H

#include "app.h"

void FreeBitmap(BitmapType **bitmapPP, BitmapType **bitsPP);
void StopThumbs(AppState *s);
UInt32 ThumbBytes(const AppState *s);
void FreeThumbs(AppState *s);
void ShowDetailThumb(AppState *s);
void StartThumbs(AppState *s);
void ThumbArrived(AppState *s);
void ThumbsEnded(AppState *s);

#endif
