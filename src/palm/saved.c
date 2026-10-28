/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Lists the MP4 files in the card's PalmView folder. The list is read each
 * time the screen opens and freed when it closes. */

#include <PalmOS.h>

#include "resource.h"
#include "saved.h"
#include "thumbs.h"

#define MAX_SAVED 32
#define SAVED_NAME_CAPACITY 64

static void DrawSavedRow(Int16 itemNum, RectangleType *boundsP,
                         Char **itemsText)
{
    /* The list's choices pointer carries the name table. */
    const Char *nameP = (const Char *)itemsText +
        (UInt32)itemNum * SAVED_NAME_CAPACITY;
    UInt16 length = StrLen(nameP);
    if (length > 4) length -= 4;    /* ".mp4" */
    WinDrawTruncChars(nameP, length, boundsP->topLeft.x + 1,
        boundsP->topLeft.y, boundsP->extent.x - 2);
}

static void LoadSaved(AppState *s)
{
    ListType *listP = (ListType *)GetObject(SavedList);
    UInt16 volume;
    s->savedCount = s->savedNamesP != 0 && StoreOpenVolume(&volume) == errNone
        ? StoreListVideos(volume, s->savedNamesP, SAVED_NAME_CAPACITY, MAX_SAVED)
        : 0;
    LstSetListChoices(listP, (Char **)s->savedNamesP, (Int16)s->savedCount);
    LstSetSelection(listP, noListSelection);
}

static const Char *SavedHint(const AppState *s)
{
    return s->savedCount == 0 ? "No saved videos yet." : "Select a video.";
}

void OpenSavedForm(AppState *s)
{
    if (s->savedNamesP == 0)
        s->savedNamesP = (Char *)MemPtrNew((UInt32)MAX_SAVED * SAVED_NAME_CAPACITY);
    LstSetDrawFunction((ListType *)GetObject(SavedList), DrawSavedRow);
    LoadSaved(s);
    StrCopy(s->savedStatus, SavedHint(s));
    FldSetTextPtr((FieldType *)GetObject(SavedStatusField), s->savedStatus);
}

/* With a video selected, Play and Delete take the status line's place. */
void ShowSavedActions(Boolean selected)
{
    ShowObject(SavedStatusField, !selected);
    ShowObject(SavedPlayButton, selected);
    ShowObject(SavedDeleteButton, selected);
}

void PlaySaved(AppState *s)
{
    Int16 row = LstGetSelection((ListType *)GetObject(SavedList));
    if (row < 0 || (UInt16)row >= s->savedCount) return;
    StoreMakePath(s->playPath, s->savedNamesP + (UInt32)row * SAVED_NAME_CAPACITY);
    FrmPopupForm(PlayForm);
}

/* Asks first. */
void DeleteSaved(AppState *s)
{
    ListType *listP = (ListType *)GetObject(SavedList);
    FormType *formP = FrmGetActiveForm();
    RectangleType bounds;
    Char name[SAVED_NAME_CAPACITY], path[STORE_PATH_CAPACITY];
    Int16 row = LstGetSelection(listP);
    UInt16 volume, length;
    if (row < 0 || (UInt16)row >= s->savedCount) return;
    StrCopy(name, s->savedNamesP + (UInt32)row * SAVED_NAME_CAPACITY);
    StoreMakePath(path, name);
    length = StrLen(name);
    if (length > 4) name[length - 4] = '\0';
    if (FrmCustomAlert(DeleteVideoAlert, name, "", "") != 0) return;
    if (StoreOpenVolume(&volume) == errNone && VFSFileDelete(volume, path) == errNone) {
        LoadSaved(s);
        FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, SavedList), &bounds);
        WinEraseRectangle(&bounds, 0);
        LstDrawList(listP);
        StrCopy(s->savedStatus, s->savedCount == 0 ? SavedHint(s) : "Deleted.");
    } else {
        StrCopy(s->savedStatus, "Could not delete.");
    }
    ShowSavedActions(false);
    SetFieldText(SavedStatusField, s->savedStatus);
}

/* The saved videos form at 160 high, from app.rcp */
#define SAVED_HEIGHT 160
#define SAVED_ITEMS 11
#define SAVED_ROW 11
#define SAVED_BUTTONS_Y 145
#define SAVED_STATUS_Y 146

/* Fits the form to the screen: the list shows more rows, and the buttons
 * stay at the bottom. */
void LayoutSavedForm(void)
{
    static const UInt16 kButtonIds[] = {
        SavedPlayButton, SavedDeleteButton, SavedBackButton
    };
    FormType *formP = FrmGetActiveForm();
    RectangleType bounds;
    Coord width, height, extra;
    UInt16 i;
    if (!HasInputArea()) return;
    WinGetDisplayExtent(&width, &height);
    RctSetRectangle(&bounds, 0, 0, width, height);
    WinSetBounds(FrmGetWindowHandle(formP), &bounds);
    extra = height > SAVED_HEIGHT ? height - SAVED_HEIGHT : 0;
    LstSetHeight((ListType *)GetObject(SavedList),
        (Int16)(SAVED_ITEMS + extra / SAVED_ROW));
    SetObjectY(formP, FrmGetObjectIndex(formP, SavedStatusField),
        SAVED_STATUS_Y + extra, 0);
    for (i = 0; i < sizeof(kButtonIds) / sizeof(kButtonIds[0]); i++)
        SetObjectY(formP, FrmGetObjectIndex(formP, kButtonIds[i]),
            SAVED_BUTTONS_Y + extra, 0);
}

void ReturnFromSaved(AppState *s)
{
    FrmReturnToForm(MainForm);
    LayoutMainForm(s);
    FrmDrawForm(FrmGetActiveForm());
    ShowResults(s, true);
    if (s->savedNamesP != 0) MemPtrFree(s->savedNamesP);
    s->savedNamesP = 0;
    s->savedCount = 0;
    StartThumbs(s);
}
