/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The details screen, DetailForm: the selected video's stream info, with
 * the quality picker and the download's progress. */

#ifndef PALMVIEW_DETAIL_H
#define PALMVIEW_DETAIL_H

#include "app.h"

void LayoutDetailForm(void);
void ScrollInfo(Int16 lines);
UInt16 VideoChoice(const AppState *s);
void BuildInfo(AppState *s);
void RefreshDetail(AppState *s);
void SwitchDetailQuality(AppState *s, UInt16 choice);
void MakeOutPath(AppState *s);
void StartPlayer(AppState *s);
void FinishPlayer(AppState *s);

#endif
