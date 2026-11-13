/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The lists of videos: the search results on MainForm, the bookmarks on
 * BookmarksForm, and a channel's on ChannelForm. They share the drawing,
 * the page selector and the status line, with the same object IDs on each
 * form. */

#include <PalmOS.h>

#include "bookmarks.h"
#include "download.h"
#include "resource.h"
#include "results.h"
#include "thumb.h"
#include "thumbs.h"

/* Where the list starts, and what the status line below it takes */
#define RESULT_TOP 31
#define RESULT_BOTTOM 14
/* The status line's width, and what the page selector leaves of it */
#define STATUS_WIDTH 140
#define STATUS_PAGER_WIDTH 90
/* A channel's screen has no search row, so its list starts right under the
 * title. Back shares the row with the status line. The bookmarks' screen
 * is laid out the same. */
#define CHANNEL_TOP 16
#define CHANNEL_BOTTOM 18
#define CHANNEL_ROW_BOTTOM 14
#define CHANNEL_STATUS_PAGER_WIDTH 50
#define CHANNEL_STATUS_WIDTH 114
#define CHANNEL_TITLE_WIDTH 140
/* Tokens for this many replies stay within what 5 fewer thumbnails per page
 * free, so a channel never uses more memory than search results. */
#define CHANNEL_REPLIES_MAX 12

static void DrawPageNumber(const AppState *s)
{
    FormType *formP = FrmGetActiveForm();
    RectangleType bounds;
    Char number[12];
    FontID font = FntSetFont(stdFont);
    Coord width;
    FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, PageGadget), &bounds);
    StrCopy(number, "Page ");
    AppendNumber(number, sizeof(number), s->page + 1);
    width = FntCharsWidth(number, (Int16)StrLen(number));
    WinEraseRectangle(&bounds, 0);
    WinDrawChars(number, (Int16)StrLen(number),
        bounds.topLeft.x + (bounds.extent.x - width) / 2, bounds.topLeft.y);
    FntSetFont(font);
}

/* The channel's and the bookmarks' screens, which have a Back button */
static Boolean HasBackButton(void)
{
    return OnForm(ChannelForm) || OnForm(BookmarksForm);
}

static Boolean HasNextPage(const AppState *s)
{
    if (s->bookmarks)
        return (UInt16)(s->page + 1) * YT_SEARCH_MAX < s->bookmarkCount;
    if (s->channelId[0] == '\0') return s->searchP->nextP[0] != '\0';
    /* The first half of a reply leads to its second half. */
    if ((s->page & 1) == 0 && s->searchP->more) return true;
    return s->searchP->nextP[0] != '\0' &&
        s->page / 2 + 1 < CHANNEL_REPLIES_MAX;
}

/* The page selector on the status line, while the results have other
 * pages. A message too long to share the line hides it until the next tap
 * or key. */
void ShowPager(AppState *s)
{
    static const Char kPrev[] = "\002", kPrevOff[] = "\005";
    static const Char kNext[] = "\003", kNextOff[] = "\006";
    FieldType *fieldP = (FieldType *)GetObject(StatusField);
    ControlType *prevP = (ControlType *)GetObject(PagePrevButton);
    ControlType *nextP = (ControlType *)GetObject(PageNextButton);
    Boolean hasPrev = s->page != 0, hasNext = HasNextPage(s);
    Boolean visible = s->paged && !s->pagerHidden;
    Coord width = HasBackButton()
        ? (visible ? CHANNEL_STATUS_PAGER_WIDTH : CHANNEL_STATUS_WIDTH)
        : (visible ? STATUS_PAGER_WIDTH : STATUS_WIDTH);
    RectangleType bounds;
    FldGetBounds(fieldP, &bounds);
    if (!visible) {
        ShowObject(PagePrevButton, false);
        ShowObject(PageGadget, false);
        ShowObject(PageNextButton, false);
    }
    if (bounds.extent.x != width) {
        FldEraseField(fieldP);
        bounds.extent.x = width;
        FldSetBounds(fieldP, &bounds);
        FldDrawField(fieldP);
    }
    if (!visible) return;
    CtlSetLabel(prevP, hasPrev ? kPrev : kPrevOff);
    CtlSetEnabled(prevP, hasPrev);
    CtlSetLabel(nextP, hasNext ? kNext : kNextOff);
    CtlSetEnabled(nextP, hasNext);
    ShowObject(PagePrevButton, true);
    ShowObject(PageGadget, true);
    ShowObject(PageNextButton, true);
    DrawPageNumber(s);
}

void SetStatus(AppState *s, const Char *textP)
{
    FontID font;
    StrNCopy(s->status, textP, sizeof(s->status) - 1);
    s->status[sizeof(s->status) - 1] = '\0';
    if (!OnListForm()) return;
    font = FntSetFont(stdFont);
    s->pagerHidden = FntCharsWidth(s->status, (Int16)StrLen(s->status)) >
        (HasBackButton() ? CHANNEL_STATUS_PAGER_WIDTH : STATUS_PAGER_WIDTH);
    FntSetFont(font);
    ShowPager(s);
    SetFieldText(StatusField, s->status);
}

static void GetResultBounds(RectangleType *boundsP)
{
    FormType *formP = FrmGetActiveForm();
    FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, ResultGadget), boundsP);
}

static void AppendShort(Char *lineP, UInt16 capacity, const Char *textP)
{
    Char text[20];
    TextShorten(textP, text, sizeof(text));
    AppendDetail(lineP, capacity, text);
}

/* A list row: the thumbnail or a gray box with the duration, the title on
 * up to two lines, and the details. No title leaves it empty. Drawn in
 * native pixels for a hairline under each row. */
void DrawListRow(const RectangleType *rowP, const Char *titleP,
                 const Char *detailsP, const Char *durationP,
                 BitmapType *bitmapP, Boolean focused)
{
    static const RGBColorType kText = { 0, 0x00, 0x00, 0x00 };
    static const RGBColorType kDetails = { 0, 0x80, 0x80, 0x80 };
    static const RGBColorType kLine = { 0, 0xC8, 0xC8, 0xC8 };
    static const RGBColorType kFocus = { 0, 0xD6, 0xD6, 0xD6 };
    static const RGBColorType kFocusDetails = { 0, 0x60, 0x60, 0x60 };
    RectangleType bounds = *rowP, thumb;
    UInt16 coordinates, first, lines;
    Coord margin, lineHeight, left, width, y, bottom;
    WinPushDrawState();
    coordinates = WinSetCoordinateSystem(kCoordinatesNative);
    WinScaleRectangle(&bounds);
    WinSetBackColor(UIColorGetTableEntryIndex(UIFormFill));
    WinEraseRectangle(&bounds, 0);
    if (titleP != 0) {
        bottom = bounds.topLeft.y + bounds.extent.y - 1;
        WinSetForeColorRGB(&kLine, 0);
        WinDrawLine(bounds.topLeft.x, bottom,
            bounds.topLeft.x + bounds.extent.x - 1, bottom);
        bounds.extent.y--;
        if (focused) {
            WinSetBackColorRGB(&kFocus, 0);
            WinEraseRectangle(&bounds, 0);
        }
        FntSetFont(stdFont);
        lineHeight = FntLineHeight();
        margin = WinScaleCoord(2, false);
        RctSetRectangle(&thumb, bounds.topLeft.x + margin, bounds.topLeft.y +
            (bounds.extent.y - THUMB_LIST_HEIGHT) / 2, THUMB_LIST_WIDTH,
            THUMB_LIST_HEIGHT);
        if (bitmapP != 0)
            WinDrawBitmap(bitmapP, thumb.topLeft.x, thumb.topLeft.y);
        else
            DrawThumbPlaceholder(&thumb, true, durationP);
        left = thumb.topLeft.x + thumb.extent.x + 2 * margin;
        width = bounds.topLeft.x + bounds.extent.x - margin - left;
        first = FntWordWrap(titleP, (UInt16)width);
        lines = titleP[first] != '\0' ? 3 : 2;
        y = bounds.topLeft.y + (bounds.extent.y - lines * lineHeight) / 2;
        WinSetTextColorRGB(&kText, 0);
        WinDrawTruncChars(titleP, (Int16)first, left, y, width);
        if (lines == 3) {
            y += lineHeight;
            WinDrawTruncChars(titleP + first, (Int16)StrLen(titleP + first),
                left, y, width);
        }
        WinSetTextColorRGB(focused ? &kFocusDetails : &kDetails, 0);
        WinDrawTruncChars(detailsP, StrLen(detailsP), left, y + lineHeight,
            width);
    }
    WinSetCoordinateSystem(coordinates);
    WinPopDrawState();
}

/* A result, with "1.8B · 10y ago · Channel" for details */
void DrawResultRow(const AppState *s, UInt16 row)
{
    UInt16 item = s->resultTop + row;
    const YtSearchItem *itemP = &s->searchP->items[item];
    RectangleType bounds;
    Char line[80];
    GetResultBounds(&bounds);
    bounds.topLeft.y += row * s->resultRowHeight;
    bounds.extent.y = s->resultRowHeight;
    if (item >= s->searchP->count) {
        DrawListRow(&bounds, 0, 0, 0, 0, false);
        return;
    }
    line[0] = '\0';
    if (itemP->live) AppendDetail(line, sizeof(line), "Live");
    AppendShort(line, sizeof(line), itemP->views);
    AppendShort(line, sizeof(line), itemP->published);
    AppendDetail(line, sizeof(line), itemP->channel);
    DrawListRow(&bounds, itemP->title, line, itemP->length,
        s->thumbs[item].bitmapP, item == s->resultFocus);
}

void DrawListMessage(const RectangleType *listP, Coord rowHeight,
                     const Char *textP)
{
    Int16 length = (Int16)StrLen(textP);
    WinPushDrawState();
    FntSetFont(stdFont);
    WinDrawChars(textP, length, listP->topLeft.x +
        (listP->extent.x - FntCharsWidth(textP, length)) / 2,
        listP->topLeft.y + (rowHeight - FntLineHeight()) / 2);
    WinPopDrawState();
}

static void DrawResults(const AppState *s)
{
    RectangleType bounds;
    UInt16 row;
    for (row = 0; row < s->resultRows; row++) DrawResultRow(s, row);
    if (s->searchP->count != 0 || !s->listLoaded) return;
    GetResultBounds(&bounds);
    DrawListMessage(&bounds, s->resultRowHeight,
        s->bookmarks ? "No bookmarks yet."
        : s->page == 0 ? "No results." : "No more results.");
}

void ShowResults(AppState *s, Boolean draw)
{
    UInt16 count = s->searchP->count;
    UInt16 lastTop = count > s->resultRows ? count - s->resultRows : 0;
    if (s->resultTop > lastTop) s->resultTop = lastTop;
    SclSetScrollBar((ScrollBarType *)GetObject(ResultScrollBar),
        (Int16)s->resultTop, 0, (Int16)lastTop, s->resultRows);
    if (draw) DrawResults(s);
}

void ScrollResults(AppState *s, Int16 rows)
{
    Int16 top = (Int16)s->resultTop + rows;
    s->resultTop = top > 0 ? (UInt16)top : 0;
    ShowResults(s, true);
}

/* Highlights item, or nothing with NO_RESULT. */
void FocusResult(AppState *s, UInt16 item)
{
    UInt16 previous = s->resultFocus;
    s->resultFocus = item;
    if (previous - s->resultTop < s->resultRows)
        DrawResultRow(s, previous - s->resultTop);
    if (item != previous && item - s->resultTop < s->resultRows)
        DrawResultRow(s, item - s->resultTop);
}

/* Moves the highlight by rows, scrolling to keep it in view. */
void MoveResultFocus(AppState *s, Int16 rows)
{
    UInt16 count = s->searchP->count;
    Int16 item = (Int16)s->resultFocus + rows;
    if (count == 0) return;
    if (s->resultFocus == NO_RESULT) item = (Int16)s->resultTop;
    if (item < 0) item = 0;
    if (item >= (Int16)count) item = (Int16)count - 1;
    if ((UInt16)item < s->resultTop || (UInt16)item >= s->resultTop + s->resultRows) {
        s->resultFocus = (UInt16)item;
        s->resultTop = (UInt16)item < s->resultTop
            ? (UInt16)item : (UInt16)item - s->resultRows + 1;
        ShowResults(s, true);
    } else {
        FocusResult(s, (UInt16)item);
    }
}

void OpenResult(AppState *s, UInt16 item)
{
    StopThumbs(s);
    if (s->task != taskIdle || item >= s->searchP->count) return;
    if (s->searchP->items[item].live) {
        SetStatus(s, "Live streams are not supported.");
        return;
    }
    s->selected = item;
    FrmPopupForm(DetailForm);
}

/* Pages of results. The first comes from the query, each later one from
 * a token the page before it gave. */

static void ForgetTokens(AppState *s)
{
    if (s->tokensH != 0) MemHandleFree(s->tokensH);
    s->tokensH = 0;
}

/* Keeps the first count tokens and adds tokenP after them. */
static Boolean KeepTokens(AppState *s, UInt16 count, const Char *tokenP)
{
    UInt32 offset = 0, length = StrLen(tokenP) + 1;
    Char *tokensP;
    if (s->tokensH == 0) {
        s->tokensH = MemHandleNew(length);
        if (s->tokensH == 0) return false;
    } else {
        tokensP = (Char *)MemHandleLock(s->tokensH);
        while (count-- > 0) offset += StrLen(tokensP + offset) + 1;
        MemHandleUnlock(s->tokensH);
        if (MemHandleResize(s->tokensH, offset + length) != errNone)
            return false;
    }
    tokensP = (Char *)MemHandleLock(s->tokensH);
    MemMove(tokensP + offset, tokenP, (Int32)length);
    MemHandleUnlock(s->tokensH);
    return true;
}

void FreePageBuffers(AppState *s)
{
    if (s->pageBuffersP != 0) MemPtrFree(s->pageBuffersP);
    s->pageBuffersP = 0;
}

static const Char *LockToken(AppState *s, UInt16 index)
{
    const Char *tokenP = (const Char *)MemHandleLock(s->tokensH);
    while (index-- > 0) tokenP += StrLen(tokenP) + 1;
    return tokenP;
}

static UInt16 BuildChannelPage(AppState *s, UInt16 page)
{
    PageBuffers *buffersP = s->pageBuffersP;
    const Char *tokenP;
    UInt16 length;
    if (page < 2)
        return YtBuildChannelRequest(&s->client, s->prefs.visitor, s->channelId,
            buffersP->body, buffersP->request, sizeof(buffersP->request));
    tokenP = LockToken(s, page / 2 - 1);
    length = YtBuildChannelNextRequest(&s->client, s->prefs.visitor, tokenP,
        buffersP->body, buffersP->request, sizeof(buffersP->request));
    MemHandleUnlock(s->tokensH);
    return length;
}

/* A page of bookmarks, read without the network. After a bookmark was
 * removed, the same page keeps its place in the list where it can. */
static void LoadBookmarks(AppState *s, UInt16 page)
{
    Char message[24];
    UInt16 count, last;
    FreeThumbs(s);
    s->bookmarkCount = BookmarkCount();
    last = s->bookmarkCount != 0 ? (s->bookmarkCount - 1) / YT_SEARCH_MAX : 0;
    if (page > last) page = last;
    if (page != s->page) {
        s->resultTop = 0;
        if (s->resultFocus != NO_RESULT) s->resultFocus = 0;
    }
    s->page = page;
    YtSearchInit(s->searchP, &s->codec);
    ReadBookmarks(s, page * YT_SEARCH_MAX);
    count = s->searchP->count;
    if (s->resultFocus != NO_RESULT && s->resultFocus >= count)
        s->resultFocus = count != 0 ? count - 1 : NO_RESULT;
    s->paged = s->bookmarkCount > YT_SEARCH_MAX;
    s->bookmarksChanged = false;
    s->listLoaded = true;
    message[0] = '\0';
    if (s->bookmarkCount != 0) {
        AppendNumber(message, sizeof(message), s->bookmarkCount);
        Append(message, sizeof(message),
            s->bookmarkCount == 1 ? " item" : " items");
    }
    SetStatus(s, message);
    ShowResults(s, true);
    StartThumbs(s);
}

/* Loads a page of the results, 0 being the first. A channel's pages take
 * turns as the two halves of its replies. */
void LoadPage(AppState *s, UInt16 page)
{
    Boolean channel = s->channelId[0] != '\0';
    const Char *requestP = s->request;
    UInt16 length;
    StopThumbs(s);
    if (s->task != taskIdle) return;
    if (s->bookmarks) {
        LoadBookmarks(s, page);
        return;
    }
    FreeThumbs(s);
    if (channel || page != 0) {
        s->pageBuffersP = (PageBuffers *)MemPtrNew(sizeof(PageBuffers));
        if (s->pageBuffersP == 0) {
            SetStatus(s, "Out of memory.");
            return;
        }
    }
    if (channel) {
        length = BuildChannelPage(s, page);
        requestP = s->pageBuffersP->request;
        YtChannelPageInit(s->searchP, &s->codec, (page & 1) != 0,
            s->channelNextP, YT_CHANNEL_TOKEN_CAPACITY);
        JsonScanInit(&s->scanner, s->pageBuffersP->value,
            sizeof(s->pageBuffersP->value), YtSearchValue, YtSearchEnd,
            s->searchP);
    } else {
        if (page == 0) {
            length = YtBuildSearchRequest(&s->client, s->prefs.visitor, s->query,
                &s->codec, s->body, s->request, sizeof(s->request));
        } else {
            const Char *tokenP = LockToken(s, page - 1);
            length = YtBuildContinuationRequest(&s->client, s->prefs.visitor,
                tokenP, s->pageBuffersP->body, s->pageBuffersP->request,
                sizeof(s->pageBuffersP->request));
            MemHandleUnlock(s->tokensH);
            requestP = s->pageBuffersP->request;
        }
        YtSearchInit(s->searchP, &s->codec);
        JsonScanInit(&s->scanner, s->jsonValue, sizeof(s->jsonValue),
            YtSearchValue, YtSearchEnd, s->searchP);
    }
    s->page = page;
    s->resultTop = 0;
    if (s->resultFocus != NO_RESULT) s->resultFocus = 0;
    s->listLoaded = false;
    SetStatus(s, "Loading...");
    if (!StartApiRequest(s, taskSearch, requestP, length))
        FreePageBuffers(s);
    ShowResults(s, true);
}

/* A channel's next page is its reply's second half, or the next reply. */
void NextPage(AppState *s)
{
    Boolean channel = s->channelId[0] != '\0';
    UInt16 page = s->page + 1;
    StopThumbs(s);
    if (s->task != taskIdle || !HasNextPage(s)) return;
    if (!s->bookmarks &&
            (!channel || (s->page & 1) != 0 || !s->searchP->more)) {
        UInt16 kept = channel ? s->page / 2 : s->page;
        if (!KeepTokens(s, kept, s->searchP->nextP)) {
            SetStatus(s, "Out of memory.");
            return;
        }
        if (channel) page = (UInt16)((kept + 1) * 2);
    }
    LoadPage(s, page);
}

void PreviousPage(AppState *s)
{
    if (s->page != 0) LoadPage(s, s->page - 1);
}

/* The result at a screen point, or NO_RESULT */
static UInt16 ResultAt(const AppState *s, const RectangleType *boundsP,
                       Coord x, Coord y)
{
    UInt16 item;
    if (!RctPtInRectangle(x, y, boundsP)) return NO_RESULT;
    item = s->resultTop + (UInt16)(y - boundsP->topLeft.y) / s->resultRowHeight;
    return item < s->searchP->count ? item : NO_RESULT;
}

/* Highlights the tapped result while the pen stays on it, and opens it
 * when the pen lifts there. */
static void TrackResultPen(AppState *s, Coord x, Coord y)
{
    RectangleType bounds;
    UInt16 previous = s->resultFocus, pressed;
    Boolean penDown = true, inside = true;
    GetResultBounds(&bounds);
    pressed = ResultAt(s, &bounds, x, y);
    if (pressed == NO_RESULT) return;
    FocusResult(s, pressed);
    while (penDown) {
        EvtGetPen(&x, &y, &penDown);
        if ((ResultAt(s, &bounds, x, y) == pressed) != inside) {
            inside = !inside;
            FocusResult(s, inside ? pressed : previous);
        }
    }
    if (inside) OpenResult(s, pressed);
}

static Boolean ResultGadgetHandler(FormGadgetTypeInCallback *gadgetP,
                                   UInt16 cmd, void *paramP)
{
    AppState *s = (AppState *)gadgetP->data;
    if (cmd == formGadgetDrawCmd) {
        DrawResults(s);
        return true;
    }
    if (cmd == formGadgetEraseCmd) {
        WinEraseRectangle(&gadgetP->rect, 0);
        return true;
    }
    if (cmd == formGadgetHandleEventCmd &&
            ((EventType *)paramP)->eType == frmGadgetEnterEvent) {
        TrackResultPen(s, ((EventType *)paramP)->screenX,
            ((EventType *)paramP)->screenY);
        return true;
    }
    return false;
}

static Boolean PageGadgetHandler(FormGadgetTypeInCallback *gadgetP,
                                 UInt16 cmd, void *paramP)
{
    (void)paramP;
    if (cmd == formGadgetDrawCmd) {
        DrawPageNumber((AppState *)gadgetP->data);
        return true;
    }
    if (cmd == formGadgetEraseCmd) {
        WinEraseRectangle(&gadgetP->rect, 0);
        return true;
    }
    return false;
}

/* Whole rows that grow to fill listHeight */
void FitResultRows(Coord listHeight, UInt16 *rowsP, Coord *rowHeightP)
{
    UInt16 rows = (UInt16)(listHeight / RESULT_ROW_HEIGHT_MIN);
    if (rows < RESULT_ROWS_MIN) rows = RESULT_ROWS_MIN;
    if (rows > RESULT_ROWS_MAX) rows = RESULT_ROWS_MAX;
    *rowsP = rows;
    *rowHeightP = listHeight / (Coord)rows;
}

/* Fits the form to the screen: the list takes the space above the status
 * line, in whole rows that grow to fill it. The input area opening or
 * closing changes the screen's height. */
void LayoutMainForm(AppState *s)
{
    FormType *formP = FrmGetActiveForm();
    RectangleType bounds;
    Coord width, height, listHeight, statusY;
    UInt16 i;
    if (!HasInputArea()) {
        FitResultRows(160 - RESULT_TOP - RESULT_BOTTOM, &s->resultRows,
            &s->resultRowHeight);
        ShowResults(s, false);
        return;
    }
    WinGetDisplayExtent(&width, &height);
    statusY = height - RESULT_BOTTOM + 1;
    RctSetRectangle(&bounds, 0, 0, width, height);
    WinSetBounds(FrmGetWindowHandle(formP), &bounds);
    FitResultRows(height - RESULT_TOP - RESULT_BOTTOM, &s->resultRows,
        &s->resultRowHeight);
    listHeight = s->resultRowHeight * (Coord)s->resultRows;
    SetObjectY(formP, FrmGetObjectIndex(formP, ResultGadget), RESULT_TOP, listHeight);
    SetObjectY(formP, FrmGetObjectIndex(formP, ResultScrollBar), RESULT_TOP, listHeight);
    SetObjectY(formP, FrmGetObjectIndex(formP, StatusField), statusY, 0);
    SetObjectY(formP, FrmGetObjectIndex(formP, PagePrevButton), statusY, 0);
    SetObjectY(formP, FrmGetObjectIndex(formP, PageGadget), statusY, 0);
    SetObjectY(formP, FrmGetObjectIndex(formP, PageNextButton), statusY, 0);
    for (i = 0; i < FrmGetNumberOfObjects(formP); i++)
        if (FrmGetObjectType(formP, i) == frmGraffitiStateObj) {
            Coord x, y;
            FrmGetObjectPosition(formP, i, &x, &y);
            FrmSetObjectPosition(formP, i, x, statusY);
        }
    ShowResults(s, false);
}

static void InitListGadgets(AppState *s)
{
    FormType *formP = FrmGetActiveForm();
    UInt16 index = FrmGetObjectIndex(formP, ResultGadget);
    FrmSetGadgetData(formP, index, s);
    FrmSetGadgetHandler(formP, index, ResultGadgetHandler);
    index = FrmGetObjectIndex(formP, PageGadget);
    FrmSetGadgetData(formP, index, s);
    FrmSetGadgetHandler(formP, index, PageGadgetHandler);
}

void InitMainForm(AppState *s)
{
    FieldType *fieldP = (FieldType *)GetObject(SearchField);
    FldSetTextHandle(fieldP, s->searchTextH);
    InitListGadgets(s);
    LstSetSelection((ListType *)GetObject(MainScreenList), screenHome);
    ShowResults(s, false);
    FldSetTextPtr((FieldType *)GetObject(StatusField), s->status);
    FrmSetFocus(FrmGetActiveForm(), FrmGetObjectIndex(FrmGetActiveForm(),
        SearchField));
}

/* Puts queryP first in the search history, without repeats. */
static void RememberSearch(AppState *s, const Char *queryP)
{
    UInt16 i, moved = HISTORY_MAX - 1;
    for (i = 0; i < HISTORY_MAX; i++)
        if (StrCaselessCompare(s->prefs.history[i], queryP) == 0) {
            moved = i;
            break;
        }
    MemMove(s->prefs.history[1], s->prefs.history[0],
        (Int32)moved * HISTORY_CAPACITY);
    StrNCopy(s->prefs.history[0], queryP, HISTORY_CAPACITY - 1);
    s->prefs.history[0][HISTORY_CAPACITY - 1] = '\0';
    s->prefsChanged = true;
}

/* Fills the history list. Returns how many searches it shows. */
UInt16 ShowHistory(AppState *s)
{
    ListType *listP = (ListType *)GetObject(HistoryList);
    UInt16 count = 0;
    while (count < HISTORY_MAX && s->prefs.history[count][0] != '\0') {
        s->historyItems[count] = s->prefs.history[count];
        count++;
    }
    LstSetListChoices(listP, s->historyItems, count);
    LstSetHeight(listP, count);
    LstSetSelection(listP, noListSelection);
    return count;
}

void InitChannelForm(AppState *s)
{
    InitListGadgets(s);
    FrmSetTitle(FrmGetActiveForm(), s->channelTitle);
    FldSetTextPtr((FieldType *)GetObject(StatusField), s->status);
    ShowResults(s, false);
}

void InitBookmarksForm(AppState *s)
{
    InitListGadgets(s);
    LstSetSelection((ListType *)GetObject(BookmarksScreenList),
        screenBookmarks);
    FldSetTextPtr((FieldType *)GetObject(StatusField), s->status);
    ShowResults(s, false);
}

static void LayoutBackForm(AppState *s, UInt16 backId)
{
    static const UInt16 kRowIds[] = {
        StatusField, PagePrevButton, PageGadget, PageNextButton
    };
    FormType *formP = FrmGetActiveForm();
    RectangleType bounds;
    Coord width, height = 160, listHeight;
    UInt16 i;
    if (HasInputArea()) {
        WinGetDisplayExtent(&width, &height);
        RctSetRectangle(&bounds, 0, 0, width, height);
        WinSetBounds(FrmGetWindowHandle(formP), &bounds);
    }
    FitResultRows(height - CHANNEL_TOP - CHANNEL_BOTTOM, &s->resultRows,
        &s->resultRowHeight);
    listHeight = s->resultRowHeight * (Coord)s->resultRows;
    SetObjectY(formP, FrmGetObjectIndex(formP, ResultGadget), CHANNEL_TOP,
        listHeight);
    SetObjectY(formP, FrmGetObjectIndex(formP, ResultScrollBar), CHANNEL_TOP,
        listHeight);
    for (i = 0; i < sizeof(kRowIds) / sizeof(kRowIds[0]); i++)
        SetObjectY(formP, FrmGetObjectIndex(formP, kRowIds[i]),
            height - CHANNEL_ROW_BOTTOM, 0);
    SetObjectY(formP, FrmGetObjectIndex(formP, backId),
        height - CHANNEL_ROW_BOTTOM, 0);
    ShowResults(s, false);
}

void LayoutChannelForm(AppState *s)
{
    LayoutBackForm(s, ChannelBackButton);
}

void LayoutBookmarksForm(AppState *s)
{
    LayoutBackForm(s, BookmarksBackButton);
}

void StartSearch(AppState *s)
{
    FieldType *fieldP = (FieldType *)GetObject(SearchField);
    const Char *queryP = FldGetTextPtr(fieldP);
    StopThumbs(s);
    if (s->task != taskIdle || queryP == 0 || queryP[0] == '\0') return;
    RememberSearch(s, queryP);
    StrNCopy(s->query, queryP, sizeof(s->query) - 1);
    s->query[sizeof(s->query) - 1] = '\0';
    ForgetTokens(s);
    s->paged = false;
    s->resultFocus = NO_RESULT;
    LoadPage(s, 0);
}

void FinishSearch(AppState *s)
{
    Char message[80];
    Boolean ok = s->jobResult.status == netJobOk &&
        JsonScanFinish(&s->scanner) == jsonOk;
    message[0] = '\0';
    if (HasNextPage(s)) s->paged = true;
    if (!ok && s->jobResult.status == netJobOk) {
        StrCopy(message, "Unexpected reply from YouTube.");
    } else if (!ok) {
        DescribeFailure(s, message, sizeof(message));
    } else if (s->searchP->count == 0) {
        /* The list says so. */
    } else if (s->channelId[0] != '\0') {
        AppendNumber(message, sizeof(message), s->searchP->count);
        Append(message, sizeof(message), " videos");
    } else {
        AppendNumber(message, sizeof(message), s->searchP->count);
        Append(message, sizeof(message), " results in ");
        AppendSeconds(message, sizeof(message),
            TimGetTicks() - s->taskStartTicks);
    }
    if (ok) SaveVisitor(s, s->searchP->visitor);
    s->listLoaded = ok;
    SetStatus(s, message);
    if (OnListForm()) ShowResults(s, true);
    if (ok) StartThumbs(s);
}

static void StopJob(AppState *s)
{
    if (s->jobP != 0) {
        NetJobCancel(s->jobP);
        while (s->jobP != 0) NetJobStep(&s->jobP);
    }
    CloseFile(s);
    s->task = taskIdle;
    StopThumbs(s);
    FreeBitmap(&s->detailThumbP, &s->detailBitsP);
}

/* Back to the list the details screen was opened from. The bookmarks
 * are read again if they changed meanwhile. */
void ReturnToList(AppState *s)
{
    UInt16 formId = s->channelId[0] != '\0' ? ChannelForm
        : s->bookmarks ? BookmarksForm : MainForm;
    StopJob(s);
    FrmReturnToForm(formId);
    if (formId == ChannelForm) LayoutChannelForm(s);
    else if (formId == BookmarksForm) LayoutBookmarksForm(s);
    else LayoutMainForm(s);
    FrmDrawForm(FrmGetActiveForm());
    if (s->bookmarks && s->bookmarksChanged) {
        LoadBookmarks(s, s->page);
        return;
    }
    ShowResults(s, true);
    StartThumbs(s);
}

/* Puts the list on screen aside, under searchP's. */
static void PushList(AppState *s, YtSearch *searchP)
{
    ListState *listP = &s->lists[s->listDepth++];
    listP->searchP = s->searchP;
    listP->tokensH = s->tokensH;
    listP->page = s->page;
    listP->top = s->resultTop;
    listP->focus = s->resultFocus;
    listP->paged = s->paged;
    listP->bookmarks = s->bookmarks;
    listP->loaded = s->listLoaded;
    StrCopy(listP->status, s->status);
    YtSearchInit(searchP, &s->codec);
    s->searchP = searchP;
    s->tokensH = 0;
    s->page = 0;
    s->resultTop = 0;
    s->resultFocus = NO_RESULT;
    s->paged = false;
    s->pagerHidden = false;
    s->bookmarks = false;
    s->listLoaded = false;
    s->status[0] = '\0';
}

/* Frees the list on screen for the one put aside under it. */
static void PopList(AppState *s)
{
    ListState *listP = &s->lists[--s->listDepth];
    ForgetTokens(s);
    MemPtrFree(s->searchP);
    s->searchP = listP->searchP;
    listP->searchP = 0;
    s->tokensH = listP->tokensH;
    listP->tokensH = 0;
    s->page = listP->page;
    s->resultTop = listP->top;
    s->resultFocus = listP->focus;
    s->paged = listP->paged;
    s->bookmarks = listP->bookmarks;
    s->listLoaded = listP->loaded;
    s->pagerHidden = false;
    StrCopy(s->status, listP->status);
}

/* Opens the selected video's channel on its own screen. The list it was
 * opened from waits in memory, without its thumbnails. */
void OpenChannel(AppState *s)
{
    static const Char kChannelSuffix[] = " - Videos";
    const YtSearchItem *itemP = &s->searchP->items[s->selected];
    UInt16 listForm = s->bookmarks ? BookmarksForm : MainForm;
    YtSearch *channelP;
    FontID font;
    if (s->task == taskDownload || s->channelId[0] != '\0' ||
            s->listDepth == LIST_DEPTH)
        return;
    channelP = (YtSearch *)MemPtrNew(sizeof(YtSearch));
    s->channelNextP = (Char *)MemPtrNew(YT_CHANNEL_TOKEN_CAPACITY);
    if (channelP == 0 || s->channelNextP == 0) {
        if (channelP != 0) MemPtrFree(channelP);
        if (s->channelNextP != 0) MemPtrFree(s->channelNextP);
        s->channelNextP = 0;
        SetProgressText(s, "Out of memory.");
        return;
    }
    StopJob(s);
    FreeThumbs(s);
    StrCopy(s->channelId, itemP->channelId);
    StrCopy(s->channelName, itemP->channel);
    font = FntSetFont(boldFont);
    FitText(&s->codec, s->channelTitle, itemP->channel, CHANNEL_TITLE_WIDTH -
        FntCharsWidth(kChannelSuffix, sizeof(kChannelSuffix) - 1));
    FntSetFont(font);
    Append(s->channelTitle, sizeof(s->channelTitle), kChannelSuffix);
    PushList(s, channelP);
    /* Closing the details form shows the list under it. Locked, it is not
     * seen. Deleting the form later instead left too little memory to play. */
    s->screenLocked = WinScreenLock(winLockCopy) != 0;
    FrmReturnToForm(listForm);
    FrmPopupForm(ChannelForm);
}

void CloseChannel(AppState *s)
{
    StopJob(s);
    FreeThumbs(s);
    FreePageBuffers(s);
    MemPtrFree(s->channelNextP);
    s->channelNextP = 0;
    PopList(s);
    s->channelId[0] = '\0';
    /* Read again for the thumbnails stored with them */
    if (s->bookmarks) s->bookmarksChanged = true;
    ReturnToList(s);
}

/* Opens the bookmarks on their own screen, over the search results. */
Boolean OpenBookmarks(AppState *s)
{
    YtSearch *searchP;
    StopThumbs(s);
    if (s->task != taskIdle || s->listDepth != 0) return false;
    searchP = (YtSearch *)MemPtrNew(sizeof(YtSearch));
    if (searchP == 0) {
        SetStatus(s, "Out of memory.");
        return false;
    }
    FreeThumbs(s);
    PushList(s, searchP);
    s->bookmarks = true;
    FrmPopupForm(BookmarksForm);
    return true;
}

/* Back to the search results */
void CloseBookmarks(AppState *s)
{
    StopJob(s);
    FreeThumbs(s);
    PopList(s);
    ReturnToList(s);
}

void FreeResults(AppState *s)
{
    ForgetTokens(s);
    FreePageBuffers(s);
    if (s->channelNextP != 0) MemPtrFree(s->channelNextP);
    while (s->listDepth > 0) {
        ListState *listP = &s->lists[--s->listDepth];
        if (listP->searchP != 0) MemPtrFree(listP->searchP);
        if (listP->tokensH != 0) MemHandleFree(listP->tokensH);
    }
}
