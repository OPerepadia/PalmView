/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The lists of videos: the search results on MainForm, and a channel's on
 * ChannelForm. */

#ifndef PALMVIEW_RESULTS_H
#define PALMVIEW_RESULTS_H

#include "app.h"

/* Three lines each: the title on up to two, then the details. With the
 * input area closed there is room for more, and the rows grow to fill it. */
#define RESULT_ROWS_MIN 3
#define RESULT_ROW_HEIGHT_MIN 38
#define RESULT_ROWS_MAX 6

void SetStatus(AppState *s, const Char *textP);
void ShowPager(AppState *s);

void DrawListRow(const RectangleType *rowP, const Char *titleP,
                 const Char *detailsP, const Char *durationP,
                 BitmapType *bitmapP, Boolean focused);
void DrawResultRow(const AppState *s, UInt16 row);
void FitResultRows(Coord listHeight, UInt16 *rowsP, Coord *rowHeightP);
void ShowResults(AppState *s, Boolean draw);
void ScrollResults(AppState *s, Int16 rows);
void FocusResult(AppState *s, UInt16 item);
void MoveResultFocus(AppState *s, Int16 rows);
void OpenResult(AppState *s, UInt16 item);

void LoadPage(AppState *s, UInt16 page);
void NextPage(AppState *s);
void PreviousPage(AppState *s);
void FreePageBuffers(AppState *s);

void InitMainForm(AppState *s);
void LayoutMainForm(AppState *s);
UInt16 ShowHistory(AppState *s);
void StartSearch(AppState *s);
void FinishSearch(AppState *s);

void InitChannelForm(AppState *s);
void LayoutChannelForm(AppState *s);
void OpenChannel(AppState *s);
void CloseChannel(AppState *s);

void ReturnToList(AppState *s);
void FreeResults(AppState *s);

#endif
