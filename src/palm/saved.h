/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The saved videos on the card, SavedForm: select one, then play or
 * delete it. */

#ifndef PALMVIEW_SAVED_H
#define PALMVIEW_SAVED_H

#include "app.h"

void OpenSavedForm(AppState *s);
void ShowSavedActions(Boolean selected);
void PlaySaved(AppState *s);
void DeleteSaved(AppState *s);
void LayoutSavedForm(void);
void ReturnFromSaved(AppState *s);

#endif
