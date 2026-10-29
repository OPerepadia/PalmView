/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The saved videos on the card, SavedForm: select one, then play or
 * delete it. */

#ifndef PALMVIEW_SAVED_H
#define PALMVIEW_SAVED_H

#include "app.h"

typedef struct SavedVideo {
    UInt16 width, height;       /* 0 when the moov cannot be read */
    UInt32 ms;                  /* 0 when unknown */
    UInt32 size;
} SavedVideo;

/* False when the file cannot be opened. */
Boolean ReadSavedVideo(const Char *pathP, SavedVideo *videoP);
UInt16 SavedQuality(const SavedVideo *videoP);

void OpenSavedForm(AppState *s);
void ScrollSaved(AppState *s, Int16 rows);
void FocusSavedList(AppState *s);
void PlaySaved(AppState *s);
Boolean HandleSavedKey(AppState *s, const EventType *eventP);
void DeleteSaved(AppState *s);
void LayoutSavedForm(AppState *s);
void ReturnFromSaved(AppState *s);
void FreeSaved(AppState *s);

#endif
