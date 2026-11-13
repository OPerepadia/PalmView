/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Bookmarked videos, newest first, in a database in handheld memory, and
 * the star on the details screen's thumbnail that adds and removes them. */

#ifndef PALMVIEW_BOOKMARKS_H
#define PALMVIEW_BOOKMARKS_H

#include "app.h"

UInt16 BookmarkCount(void);
/* Reads up to YT_SEARCH_MAX bookmarks, from the first-th on, into the list.
 * Those with a thumbnail are marked thumbStored. */
void ReadBookmarks(AppState *s, UInt16 first);
/* A copy of the bookmark's thumbnail, or 0 */
UInt8 *ReadBookmarkThumb(const Char *videoIdP, UInt16 *lengthP);
/* Adds the thumbnail to a bookmark that has none. */
void SaveBookmarkThumb(const Char *videoIdP, const UInt8 *jpegP,
                       UInt16 length);

void InitBookmarkStar(AppState *s);
/* Over the details screen's thumbnail, after it is drawn */
void DrawBookmarkStar(const AppState *s, Boolean pressed);

#endif
