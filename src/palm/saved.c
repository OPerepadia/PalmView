/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Lists the MP4 files in the card's PalmView folder, drawn like the search
 * results. The list is read each time the screen opens and freed when it
 * closes. */

#include <PalmOS.h>

#include "resource.h"
#include "mp4_read.h"
#include "saved.h"
#include "thumbs.h"

#define MAX_SAVED 32
#define SAVED_NAME_CAPACITY 64

static const Char *SavedName(const AppState *s, UInt16 item)
{
    return s->savedNamesP + (UInt32)item * SAVED_NAME_CAPACITY;
}

static void GetSavedBounds(RectangleType *boundsP)
{
    FormType *formP = FrmGetActiveForm();
    FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, SavedGadget), boundsP);
}

static Boolean InView(const AppState *s, UInt16 item)
{
    return item >= s->savedTop && item - s->savedTop < s->savedRows;
}

static void FreeEntryThumbs(AppState *s)
{
    UInt16 i;
    if (s->savedEntriesP == 0) return;
    for (i = 0; i < MAX_SAVED; i++) {
        SavedEntry *entryP = &s->savedEntriesP[i];
        FreeBitmap(&entryP->bitmapP, &entryP->bitsP);
        entryP->thumbState = thumbMissing;
    }
}

void FreeSaved(AppState *s)
{
    FreeEntryThumbs(s);
    if (s->savedEntriesP != 0) MemPtrFree(s->savedEntriesP);
    if (s->savedNamesP != 0) MemPtrFree(s->savedNamesP);
    s->savedEntriesP = 0;
    s->savedNamesP = 0;
    s->savedCount = 0;
}

/* Reads a row's details and thumbnail when it is first drawn. */
static void ReadEntry(AppState *s, UInt16 item)
{
    SavedEntry *entryP = &s->savedEntriesP[item];
    Char path[STORE_PATH_CAPACITY], duration[12];
    StoreMakePath(path, SavedName(s, item));
    if (!entryP->read) {
        SavedVideo video;
        entryP->read = true;
        if (ReadSavedVideo(path, &video)) {
            entryP->size = video.size;
            entryP->ms = video.ms;
            entryP->quality = SavedQuality(&video);
        }
    }
    if (entryP->thumbState == thumbMissing) {
        duration[0] = '\0';
        if (entryP->ms != 0) AppendClock(duration, sizeof(duration), entryP->ms);
        entryP->bitmapP = LoadSavedThumb(s, path, duration, &entryP->bitsP);
        entryP->thumbState = entryP->bitmapP != 0 ? thumbLoaded : thumbFailed;
    }
}

/* "240p · 9.8 MB" */
static void DrawSavedRow(AppState *s, UInt16 row)
{
    UInt16 item = s->savedTop + row, length;
    const SavedEntry *entryP;
    RectangleType bounds;
    Char title[SAVED_NAME_CAPACITY], line[32], size[16], duration[12];
    GetSavedBounds(&bounds);
    bounds.topLeft.y += row * s->savedRowHeight;
    bounds.extent.y = s->savedRowHeight;
    if (item >= s->savedCount) {
        DrawListRow(&bounds, 0, 0, 0, 0, false);
        return;
    }
    ReadEntry(s, item);
    entryP = &s->savedEntriesP[item];
    StrCopy(title, SavedName(s, item));
    length = StrLen(title);
    if (length > 4) title[length - 4] = '\0';    /* ".mp4" */
    line[0] = '\0';
    if (entryP->size != 0) {
        size[0] = '\0';
        AppendSize(size, sizeof(size), entryP->size);
        AppendDetail(line, sizeof(line), kQualityNames[entryP->quality]);
        AppendDetail(line, sizeof(line), size);
    }
    duration[0] = '\0';
    if (entryP->ms != 0) AppendClock(duration, sizeof(duration), entryP->ms);
    DrawListRow(&bounds, title, line, duration, entryP->bitmapP,
        item == s->savedFocus);
}

static void DrawSaved(AppState *s)
{
    UInt16 row;
    for (row = 0; row < s->savedRows; row++) DrawSavedRow(s, row);
}

/* Keeps thumbnails only for the rows in view. */
static void ShowSaved(AppState *s, Boolean draw)
{
    UInt16 lastTop = s->savedCount > s->savedRows ? s->savedCount - s->savedRows : 0;
    UInt16 i;
    if (s->savedTop > lastTop) s->savedTop = lastTop;
    for (i = 0; i < s->savedCount; i++)
        if (!InView(s, i)) {
            SavedEntry *entryP = &s->savedEntriesP[i];
            FreeBitmap(&entryP->bitmapP, &entryP->bitsP);
            entryP->thumbState = thumbMissing;
        }
    SclSetScrollBar((ScrollBarType *)GetObject(SavedScrollBar),
        (Int16)s->savedTop, 0, (Int16)lastTop, (Int16)s->savedRows);
    if (draw) DrawSaved(s);
}

void ScrollSaved(AppState *s, Int16 rows)
{
    Int16 top = (Int16)s->savedTop + rows;
    s->savedTop = top > 0 ? (UInt16)top : 0;
    ShowSaved(s, true);
}

static void LoadSaved(AppState *s)
{
    UInt16 volume;
    FreeEntryThumbs(s);
    if (s->savedEntriesP != 0)
        MemSet(s->savedEntriesP, MAX_SAVED * sizeof(SavedEntry), 0);
    s->savedCount = s->savedNamesP != 0 && s->savedEntriesP != 0 &&
        StoreOpenVolume(&volume) == errNone
        ? StoreListVideos(volume, s->savedNamesP, SAVED_NAME_CAPACITY, MAX_SAVED)
        : 0;
    s->savedTop = 0;
    s->savedFocus = NO_RESULT;
}

static const Char *SavedHint(const AppState *s)
{
    return s->savedCount == 0 ? "No downloads yet." : "Select a video.";
}

/* With a video selected, Play and Delete take the status line's place. */
static void ShowSavedActions(Boolean selected)
{
    ShowObject(SavedStatusField, !selected);
    ShowObject(SavedPlayButton, selected);
    ShowObject(SavedDeleteButton, selected);
}

/* Highlights item, or nothing with NO_RESULT. */
static void FocusSaved(AppState *s, UInt16 item)
{
    UInt16 previous = s->savedFocus;
    s->savedFocus = item;
    if (InView(s, previous)) DrawSavedRow(s, previous - s->savedTop);
    if (item != previous && InView(s, item)) DrawSavedRow(s, item - s->savedTop);
}

/* Selects a video, scrolling to keep it in view. */
static void ChooseSaved(AppState *s, UInt16 item)
{
    if (!InView(s, item)) {
        s->savedFocus = item;
        s->savedTop = item < s->savedTop ? item : item - s->savedRows + 1;
        ShowSaved(s, true);
    } else {
        FocusSaved(s, item);
    }
    ShowSavedActions(true);
}

/* The video at a screen point, or NO_RESULT */
static UInt16 SavedAt(const AppState *s, const RectangleType *boundsP,
                      Coord x, Coord y)
{
    UInt16 item;
    if (!RctPtInRectangle(x, y, boundsP)) return NO_RESULT;
    item = s->savedTop + (UInt16)(y - boundsP->topLeft.y) / s->savedRowHeight;
    return item < s->savedCount ? item : NO_RESULT;
}

/* Highlights the video under the pen, and selects it on pen up there. */
static void TrackSavedPen(AppState *s, Coord x, Coord y)
{
    RectangleType bounds;
    UInt16 previous = s->savedFocus, pressed;
    Boolean penDown = true, inside = true;
    GetSavedBounds(&bounds);
    pressed = SavedAt(s, &bounds, x, y);
    if (pressed == NO_RESULT) return;
    FocusSaved(s, pressed);
    while (penDown) {
        EvtGetPen(&x, &y, &penDown);
        if ((SavedAt(s, &bounds, x, y) == pressed) != inside) {
            inside = !inside;
            FocusSaved(s, inside ? pressed : previous);
        }
    }
    if (inside) ChooseSaved(s, pressed);
}

static Boolean SavedGadgetHandler(FormGadgetTypeInCallback *gadgetP,
                                  UInt16 cmd, void *paramP)
{
    AppState *s = (AppState *)gadgetP->data;
    if (cmd == formGadgetDrawCmd) {
        DrawSaved(s);
        return true;
    }
    if (cmd == formGadgetEraseCmd) {
        WinEraseRectangle(&gadgetP->rect, 0);
        return true;
    }
    if (cmd == formGadgetHandleEventCmd &&
            ((EventType *)paramP)->eType == frmGadgetEnterEvent) {
        TrackSavedPen(s, ((EventType *)paramP)->screenX,
            ((EventType *)paramP)->screenY);
        return true;
    }
    return false;
}

void OpenSavedForm(AppState *s)
{
    FormType *formP = FrmGetActiveForm();
    UInt16 index = FrmGetObjectIndex(formP, SavedGadget);
    if (s->savedNamesP == 0)
        s->savedNamesP = (Char *)MemPtrNew((UInt32)MAX_SAVED * SAVED_NAME_CAPACITY);
    if (s->savedEntriesP == 0)
        s->savedEntriesP = (SavedEntry *)MemPtrNew(MAX_SAVED * sizeof(SavedEntry));
    FrmSetGadgetData(formP, index, s);
    FrmSetGadgetHandler(formP, index, SavedGadgetHandler);
    LstSetSelection((ListType *)GetObject(SavedScreenList), screenDownloads);
    LoadSaved(s);
    StrCopy(s->savedStatus, SavedHint(s));
    FldSetTextPtr((FieldType *)GetObject(SavedStatusField), s->savedStatus);
}

Boolean ReadSavedVideo(const Char *pathP, SavedVideo *videoP)
{
    Mp4Reader *readerP;
    FileRef file;
    UInt16 volume;
    MemSet(videoP, sizeof(*videoP), 0);
    if (StoreOpenVolume(&volume) != errNone ||
        StoreOpenRead(volume, pathP, &file, &videoP->size) != errNone)
        return false;
    readerP = (Mp4Reader *)MemPtrNew(sizeof(Mp4Reader));
    if (readerP != 0) {
        const Mp4ReadTrack *trackP = &readerP->video;
        MemSet(readerP, sizeof(Mp4Reader), 0);
        if (Mp4ReaderOpen(readerP, StoreMp4Read, (void *)file, videoP->size) ==
                mp4MergeOk && trackP->present) {
            videoP->width = trackP->width;
            videoP->height = trackP->height;
            if (trackP->duration != 0 && trackP->timescale != 0)
                videoP->ms = trackP->duration / trackP->timescale * 1000UL;
        }
        MemPtrFree(readerP);
    }
    VFSFileClose(file);
    return true;
}

/* 144p fits in 256x144, so a wider film at 256x128 is 144p too. */
UInt16 SavedQuality(const SavedVideo *videoP)
{
    return (UInt32)videoP->width * videoP->height > 256UL * 144UL
        ? quality240 : quality144;
}

void PlaySaved(AppState *s)
{
    if (s->savedFocus == NO_RESULT || s->savedFocus >= s->savedCount) return;
    StoreMakePath(s->playPath, SavedName(s, s->savedFocus));
    /* Playback needs the memory. */
    FreeEntryThumbs(s);
    FrmPopupForm(PlayForm);
}

void FocusSavedList(AppState *s)
{
    if (s->savedCount != 0) ChooseSaved(s, s->savedTop);
}

/* 5-way: up and down select, centre plays. Up from the first video goes to
 * the screen picker. */
Boolean HandleSavedKey(AppState *s, const EventType *eventP)
{
    WChar c = eventP->data.keyDown.chr;
    if (c == vchrPageUp || c == vchrRockerUp ||
            c == vchrPageDown || c == vchrRockerDown) {
        Int16 item;
        if ((c == vchrPageUp || c == vchrRockerUp) &&
                (s->savedFocus == NO_RESULT || s->savedFocus == 0) &&
                FocusTitlePicker(s, SavedScreenTrigger)) {
            FocusSaved(s, NO_RESULT);
            ShowSavedActions(false);
            return true;
        }
        if (s->savedCount == 0) return true;
        item = s->savedFocus == NO_RESULT ? (Int16)s->savedTop
            : (Int16)s->savedFocus + (c == vchrPageUp || c == vchrRockerUp ? -1 : 1);
        if (item < 0) item = 0;
        if (item >= (Int16)s->savedCount) item = (Int16)s->savedCount - 1;
        ChooseSaved(s, (UInt16)item);
        return true;
    }
    /* One press may arrive as both of these. */
    if (c == vchrRockerCenter || NavPressed(eventP, navBitSelect, navChangeSelect)) {
        UInt32 now = TimGetTicks();
        if (now - s->lastCentre >= SysTicksPerSecond() / 4) PlaySaved(s);
        s->lastCentre = now;
        return true;
    }
    return false;
}

/* Asks first. Deletes the thumbnail too. */
void DeleteSaved(AppState *s)
{
    Char name[SAVED_NAME_CAPACITY], path[STORE_PATH_CAPACITY];
    UInt16 volume, length;
    if (s->savedFocus == NO_RESULT || s->savedFocus >= s->savedCount) return;
    StrCopy(name, SavedName(s, s->savedFocus));
    StoreMakePath(path, name);
    length = StrLen(name);
    if (length > 4) name[length - 4] = '\0';
    if (FrmCustomAlert(DeleteVideoAlert, name, "", "") != 0) return;
    if (StoreOpenVolume(&volume) == errNone && VFSFileDelete(volume, path) == errNone) {
        Char thumbPath[STORE_PATH_CAPACITY];
        StoreThumbPath(thumbPath, path);
        VFSFileDelete(volume, thumbPath);
        LoadSaved(s);
        ShowSaved(s, true);
        StrCopy(s->savedStatus, s->savedCount == 0 ? SavedHint(s) : "Deleted.");
    } else {
        StrCopy(s->savedStatus, "Could not delete.");
    }
    ShowSavedActions(false);
    SetFieldText(SavedStatusField, s->savedStatus);
}

/* The Downloads form at 160 high, from app.rcp */
#define SAVED_HEIGHT 160
#define SAVED_LIST_Y 16
#define SAVED_BOTTOM 18
#define SAVED_BUTTONS_Y 145
#define SAVED_STATUS_Y 146

/* Fits the form to the screen, with whole rows in the list. */
void LayoutSavedForm(AppState *s)
{
    static const UInt16 kButtonIds[] = {
        SavedPlayButton, SavedDeleteButton, SavedBackButton
    };
    FormType *formP = FrmGetActiveForm();
    RectangleType bounds;
    Coord width, height = SAVED_HEIGHT, extra, listHeight;
    UInt16 i;
    if (HasInputArea()) {
        WinGetDisplayExtent(&width, &height);
        RctSetRectangle(&bounds, 0, 0, width, height);
        WinSetBounds(FrmGetWindowHandle(formP), &bounds);
    }
    extra = height > SAVED_HEIGHT ? height - SAVED_HEIGHT : 0;
    FitResultRows(height - SAVED_LIST_Y - SAVED_BOTTOM, &s->savedRows,
        &s->savedRowHeight);
    listHeight = s->savedRowHeight * (Coord)s->savedRows;
    SetObjectY(formP, FrmGetObjectIndex(formP, SavedGadget), SAVED_LIST_Y,
        listHeight);
    SetObjectY(formP, FrmGetObjectIndex(formP, SavedScrollBar), SAVED_LIST_Y,
        listHeight);
    SetObjectY(formP, FrmGetObjectIndex(formP, SavedStatusField),
        SAVED_STATUS_Y + extra, 0);
    for (i = 0; i < sizeof(kButtonIds) / sizeof(kButtonIds[0]); i++)
        SetObjectY(formP, FrmGetObjectIndex(formP, kButtonIds[i]),
            SAVED_BUTTONS_Y + extra, 0);
    ShowSaved(s, false);
}

void ReturnFromSaved(AppState *s)
{
    FrmReturnToForm(MainForm);
    LstSetSelection((ListType *)GetObject(MainScreenList), screenHome);
    ReleaseTitlePicker(s);
    LayoutMainForm(s);
    FrmDrawForm(FrmGetActiveForm());
    ShowResults(s, true);
    FreeSaved(s);
    StartThumbs(s);
}
