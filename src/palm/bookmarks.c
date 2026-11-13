/* SPDX-License-Identifier: GPL-3.0-or-later */

/* One record per bookmark, newest first, in the format bookmark.h sets.
 * The database is opened for each change and closed again. */

#include <PalmOS.h>

#include "bookmark.h"
#include "bookmarks.h"
#include "resource.h"
#include "thumb_decode.h"

#define BOOKMARK_DB_NAME "PalmView Bookmarks"
#define BOOKMARK_DB_TYPE 'Bkmk'

/* The star, a bit per pixel from the left, at double and single density.
 * It sits on a dark box in the thumbnail's top right corner. */
#define STAR_LARGE_WIDTH 13
#define STAR_LARGE_HEIGHT 12
#define STAR_SMALL_WIDTH 9
#define STAR_SMALL_HEIGHT 8
/* The tap area, in the thumbnail's top right corner */
#define STAR_TAP_WIDTH 14
#define STAR_TAP_HEIGHT 14

static const UInt32 kStarLarge[STAR_LARGE_HEIGHT] = {
    0x040, 0x0E0, 0x0E0, 0x1F0, 0x1FFF, 0x1FFF, 0x7FC, 0x3F8, 0x3F8, 0x3F8,
    0x7BC, 0x60C
};
static const UInt32 kStarSmall[STAR_SMALL_HEIGHT] = {
    0x010, 0x038, 0x07C, 0x1FF, 0x0FE, 0x07C, 0x0EE, 0x0C6
};

/* Creates the database the first time, with HotSync backing it up. */
static DmOpenRef OpenDatabase(UInt16 mode, Boolean create)
{
    DmOpenRef dbP = DmOpenDatabaseByTypeCreator(BOOKMARK_DB_TYPE, APP_CREATOR,
        mode);
    LocalID id;
    UInt16 attributes = 0;
    if (dbP != 0 || !create) return dbP;
    if (DmCreateDatabase(0, BOOKMARK_DB_NAME, APP_CREATOR, BOOKMARK_DB_TYPE,
            false) != errNone)
        return 0;
    id = DmFindDatabase(0, BOOKMARK_DB_NAME);
    if (id == 0) return 0;
    DmDatabaseInfo(0, id, 0, &attributes, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    attributes |= dmHdrAttrBackup;
    DmSetDatabaseInfo(0, id, 0, &attributes, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    return DmOpenDatabase(0, id, mode);
}

/* The bookmark's index, or NO_RESULT */
static UInt16 FindBookmark(DmOpenRef dbP, const Char *videoIdP)
{
    UInt16 count = DmNumRecords(dbP), i;
    for (i = 0; i < count; i++) {
        MemHandle recordH = DmQueryRecord(dbP, i);
        const Char *idP;
        Boolean found;
        if (recordH == 0) continue;
        idP = BookmarkVideoId((const Char *)MemHandleLock(recordH),
            MemHandleSize(recordH));
        found = idP != 0 && StrCompare(idP, videoIdP) == 0;
        MemHandleUnlock(recordH);
        if (found) return i;
    }
    return NO_RESULT;
}

UInt16 BookmarkCount(void)
{
    DmOpenRef dbP = OpenDatabase(dmModeReadOnly, false);
    UInt16 count;
    if (dbP == 0) return 0;
    count = DmNumRecords(dbP);
    DmCloseDatabase(dbP);
    return count;
}

void ReadBookmarks(AppState *s, UInt16 first)
{
    DmOpenRef dbP = OpenDatabase(dmModeReadOnly, false);
    YtSearch *searchP = s->searchP;
    UInt16 count, i, thumbLength;
    if (dbP == 0) return;
    count = DmNumRecords(dbP);
    for (i = first; i < count && searchP->count < YT_SEARCH_MAX; i++) {
        MemHandle recordH = DmQueryRecord(dbP, i);
        const Char *recordP;
        UInt32 size;
        if (recordH == 0) continue;
        recordP = (const Char *)MemHandleLock(recordH);
        size = MemHandleSize(recordH);
        if (BookmarkUnpack(recordP, size, &s->codec,
                &searchP->items[searchP->count])) {
            if (BookmarkThumb(recordP, size, &thumbLength) != 0) {
                s->thumbs[searchP->count].state = thumbStored;
                s->storedThumbs = true;
            }
            searchP->count++;
        }
        MemHandleUnlock(recordH);
    }
    DmCloseDatabase(dbP);
}

UInt8 *ReadBookmarkThumb(const Char *videoIdP, UInt16 *lengthP)
{
    DmOpenRef dbP = OpenDatabase(dmModeReadOnly, false);
    UInt8 *copyP = 0;
    UInt16 index;
    *lengthP = 0;
    if (dbP == 0) return 0;
    index = FindBookmark(dbP, videoIdP);
    if (index != NO_RESULT) {
        MemHandle recordH = DmQueryRecord(dbP, index);
        const Char *recordP = (const Char *)MemHandleLock(recordH);
        const UInt8 *jpegP = BookmarkThumb(recordP, MemHandleSize(recordH),
            lengthP);
        if (jpegP != 0) copyP = (UInt8 *)MemPtrNew(*lengthP);
        if (copyP != 0) MemMove(copyP, jpegP, *lengthP);
        MemHandleUnlock(recordH);
    }
    DmCloseDatabase(dbP);
    return copyP;
}

void SaveBookmarkThumb(const Char *videoIdP, const UInt8 *jpegP,
                       UInt16 length)
{
    DmOpenRef dbP = OpenDatabase(dmModeReadWrite, false);
    UInt16 index, thumbLength;
    if (dbP == 0) return;
    index = FindBookmark(dbP, videoIdP);
    if (index != NO_RESULT) {
        MemHandle recordH = DmGetRecord(dbP, index);
        UInt32 size = MemHandleSize(recordH);
        Boolean has = BookmarkThumb((const Char *)MemHandleLock(recordH),
            size, &thumbLength) != 0;
        MemHandleUnlock(recordH);
        if (!has) {
            MemHandle resizedH = DmResizeRecord(dbP, index, size + length);
            if (resizedH != 0) {
                DmWrite(MemHandleLock(resizedH), size, jpegP, length);
                MemHandleUnlock(resizedH);
            }
        }
        DmReleaseRecord(dbP, index, !has);
    }
    DmCloseDatabase(dbP);
}

static Boolean IsBookmarked(const Char *videoIdP)
{
    DmOpenRef dbP = OpenDatabase(dmModeReadOnly, false);
    Boolean found;
    if (dbP == 0) return false;
    found = FindBookmark(dbP, videoIdP) != NO_RESULT;
    DmCloseDatabase(dbP);
    return found;
}

static Err AddBookmark(const YtSearchItem *itemP, const TextCodec *codecP,
                       const Thumb *thumbP)
{
    Char record[BOOKMARK_CAPACITY];
    UInt16 length = BookmarkPack(itemP, codecP, record, sizeof(record));
    UInt16 thumbLength = thumbP->state == thumbLoaded && thumbP->jpegP != 0
        ? thumbP->length : 0;
    UInt16 index = 0;
    DmOpenRef dbP;
    MemHandle recordH;
    Err error = errNone;
    if (length == 0) return dmErrMemError;
    dbP = OpenDatabase(dmModeReadWrite, true);
    if (dbP == 0) return dmErrCantOpen;
    if (FindBookmark(dbP, itemP->videoId) == NO_RESULT) {
        recordH = DmNewRecord(dbP, &index, (UInt32)length + thumbLength);
        if (recordH == 0) {
            error = DmGetLastErr();
            if (error == errNone) error = dmErrMemError;
        } else {
            void *recordP = MemHandleLock(recordH);
            error = DmWrite(recordP, 0, record, length);
            if (error == errNone && thumbLength != 0)
                error = DmWrite(recordP, length, thumbP->jpegP, thumbLength);
            MemHandleUnlock(recordH);
            DmReleaseRecord(dbP, index, true);
            if (error != errNone) DmRemoveRecord(dbP, index);
        }
    }
    DmCloseDatabase(dbP);
    return error;
}

static Err RemoveBookmark(const Char *videoIdP)
{
    DmOpenRef dbP = OpenDatabase(dmModeReadWrite, false);
    UInt16 index;
    Err error = errNone;
    if (dbP == 0) return errNone;
    index = FindBookmark(dbP, videoIdP);
    if (index != NO_RESULT) error = DmRemoveRecord(dbP, index);
    DmCloseDatabase(dbP);
    return error;
}

/* Gold when the video is bookmarked, gray when not, on a lighter box while
 * pressed */
void DrawBookmarkStar(const AppState *s, Boolean pressed)
{
    static const RGBColorType kOn = { 0, 0xFF, 0xC4, 0x00 };
    static const RGBColorType kOff = { 0, 0xC8, 0xC8, 0xC8 };
    static const RGBColorType kBox = { 0, 0x30, 0x30, 0x30 };
    static const RGBColorType kPressedBox = { 0, 0x70, 0x70, 0x70 };
    FormType *formP;
    RectangleType thumb, box;
    const UInt32 *maskP = kStarLarge;
    Coord width = STAR_LARGE_WIDTH, height = STAR_LARGE_HEIGHT, x, y, column;
    Coord inset = 2, pad = 2, boxWidth, boxHeight;
    UInt16 coordinates;
    if (!OnForm(DetailForm)) return;
    formP = FrmGetActiveForm();
    FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, ThumbGadget), &thumb);
    WinPushDrawState();
    coordinates = WinSetCoordinateSystem(kCoordinatesNative);
    WinScaleRectangle(&thumb);
    if (thumb.extent.y < THUMB_HEIGHT) {
        maskP = kStarSmall;
        width = STAR_SMALL_WIDTH;
        height = STAR_SMALL_HEIGHT;
        inset = 1;
        pad = 1;
    }
    boxWidth = width + 2 * pad;
    boxHeight = height + 2 * pad;
    x = thumb.topLeft.x + thumb.extent.x - inset - boxWidth;
    y = thumb.topLeft.y + inset;
    /* Corners cut, like the badge's */
    WinSetBackColorRGB(pressed ? &kPressedBox : &kBox, 0);
    RctSetRectangle(&box, x, y + 1, boxWidth, boxHeight - 2);
    WinEraseRectangle(&box, 0);
    RctSetRectangle(&box, x + 1, y, boxWidth - 2, boxHeight);
    WinEraseRectangle(&box, 0);
    x += pad;
    y += pad;
    WinSetForeColorRGB(s->bookmarked ? &kOn : &kOff, 0);
    for (; height > 0; height--, y++, maskP++) {
        for (column = 0; column < width; column++) {
            Coord start = column;
            while (column < width && (*maskP >> (width - 1 - column) & 1) != 0)
                column++;
            if (column > start) WinDrawLine(x + start, y, x + column - 1, y);
        }
    }
    WinSetCoordinateSystem(coordinates);
    WinPopDrawState();
}

static void ToggleBookmark(AppState *s)
{
    YtSearchItem item;
    Err error;
    MemMove(&item, &s->searchP->items[s->selected], sizeof(item));
    if (s->bookmarked) {
        error = RemoveBookmark(item.videoId);
    } else {
        /* A channel's own videos do not name it. */
        if (item.channelId[0] == '\0' && s->channelId[0] != '\0') {
            StrCopy(item.channel, s->channelName);
            StrCopy(item.channelId, s->channelId);
        }
        error = AddBookmark(&item, &s->codec, &s->thumbs[s->selected]);
    }
    if (error == errNone) {
        s->bookmarked = !s->bookmarked;
        s->bookmarksChanged = true;
    } else {
        SetProgressText(s, "Could not save the bookmark.");
    }
    DrawBookmarkStar(s, false);
}

/* Where a tap reaches the star: more than the star, in the thumbnail's
 * corner */
static void StarTapBounds(const RectangleType *thumbP, RectangleType *tapP)
{
    RctSetRectangle(tapP,
        thumbP->topLeft.x + thumbP->extent.x - STAR_TAP_WIDTH,
        thumbP->topLeft.y, STAR_TAP_WIDTH, STAR_TAP_HEIGHT);
}

/* Toggles the bookmark when the pen lifts in the star's corner. */
static void TrackStar(AppState *s, const RectangleType *tapP)
{
    Coord x, y;
    Boolean penDown = true, inside = true;
    DrawBookmarkStar(s, true);
    while (penDown) {
        EvtGetPen(&x, &y, &penDown);
        if (RctPtInRectangle(x, y, tapP) != inside) {
            inside = !inside;
            DrawBookmarkStar(s, inside);
        }
    }
    if (inside) ToggleBookmark(s);
}

/* The thumbnail's gadget takes the star's taps: a gadget over it does not
 * get them. The thumbnail is drawn by DrawThumb, not here. */
static Boolean ThumbGadgetHandler(FormGadgetTypeInCallback *gadgetP,
                                  UInt16 cmd, void *paramP)
{
    const EventType *eventP = (const EventType *)paramP;
    RectangleType tap;
    if (cmd == formGadgetDrawCmd || cmd == formGadgetEraseCmd) return true;
    if (cmd != formGadgetHandleEventCmd ||
            eventP->eType != frmGadgetEnterEvent)
        return false;
    StarTapBounds(&gadgetP->rect, &tap);
    if (RctPtInRectangle(eventP->screenX, eventP->screenY, &tap))
        TrackStar((AppState *)gadgetP->data, &tap);
    return true;
}

void InitBookmarkStar(AppState *s)
{
    FormType *formP = FrmGetActiveForm();
    UInt16 index = FrmGetObjectIndex(formP, ThumbGadget);
    FrmSetGadgetData(formP, index, s);
    FrmSetGadgetHandler(formP, index, ThumbGadgetHandler);
    s->bookmarked = IsBookmarked(s->searchP->items[s->selected].videoId);
}
