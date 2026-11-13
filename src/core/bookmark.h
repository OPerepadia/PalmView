/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_BOOKMARK_H
#define PALMVIEW_BOOKMARK_H

#include <PalmOS.h>
#include "text.h"
#include "yt_api.h"

/* A bookmark record: a version byte, then the video's ID, title, channel,
 * channel ID and length, each NUL-terminated, then the thumbnail's JPEG
 * when there is one. Text is kept as UTF-8, so titles still read right
 * after CJKOS is turned on or off. */
#define BOOKMARK_VERSION 1
#define BOOKMARK_CAPACITY 400

/* The text part. Returns its length, or 0 if it does not fit. */
UInt16 BookmarkPack(const YtSearchItem *itemP, const TextCodec *codecP,
                    Char *dstP, UInt16 capacity);
/* False for a record it cannot read. */
Boolean BookmarkUnpack(const Char *srcP, UInt32 length,
                       const TextCodec *codecP, YtSearchItem *itemP);
/* The record's video ID, or 0 if it cannot read it. */
const Char *BookmarkVideoId(const Char *srcP, UInt32 length);
/* The record's JPEG, or 0 without one. */
const UInt8 *BookmarkThumb(const Char *srcP, UInt32 length,
                           UInt16 *thumbLengthP);

#endif
