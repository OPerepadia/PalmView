/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The player, PlayForm, and the full screen it turns the picture to,
 * FullForm. */

#ifndef PALMVIEW_PLAYER_H
#define PALMVIEW_PLAYER_H

#include "app.h"

void GetVideoBounds(RectangleType *boundsP);
void ClearVideoArea(void);
void DrawSeekBar(const AppState *s);
void DrawVolume(const AppState *s);
Boolean TrackVolume(AppState *s, const EventType *eventP);
void ClosePlayback(AppState *s);
const Char *AttachPlayScreen(AppState *s);
void OpenPlayForm(AppState *s);
void SwitchPlayQuality(AppState *s, UInt16 choice);
void FillFullScreen(AppState *s);
void EnterFullScreen(AppState *s);
void OpenFullForm(AppState *s);
void ShowSystemBars(const AppState *s);
void ExitFullScreen(AppState *s);
void TogglePause(AppState *s);
Boolean TrackSeekBar(AppState *s, const EventType *eventP);
void LayoutPlayForm(void);
void ReturnFromPlay(AppState *s);
void StepPlayback(AppState *s);
Boolean IsPlayWindow(const AppState *s, WinHandle windowH);
Boolean HandlePlayKey(AppState *s, const EventType *eventP);

#endif
