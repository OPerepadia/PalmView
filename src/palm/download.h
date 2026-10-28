/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Saves the selected video to the card: its streams in chunks, then merged
 * into one MP4. */

#ifndef PALMVIEW_DOWNLOAD_H
#define PALMVIEW_DOWNLOAD_H

#include "app.h"

void CloseFile(AppState *s);
void StartDownload(AppState *s);
void FinishChunk(AppState *s);

#endif
