/* SPDX-License-Identifier: GPL-3.0-or-later */

/* PalmView: search YouTube, download a video's 144p H.264 and AAC streams
 * over PalmTLS, merge them into one MP4 on the memory card, and play it. */

#include <PalmOS.h>
#include <PenInputMgr.h>

#include "app.h"
#include "detail.h"
#include "download.h"
#include "palm_tls.h"
#include "player.h"
#include "resource.h"
#include "saved.h"
#include "thumb.h"
#include "thumbs.h"

#define ARM_TIMEOUT_SECONDS 30
#define M68K_TIMEOUT_SECONDS 600
#define STEP_SLICE_TICKS 10
#define CLIENT_FILE STORE_DIR "/client.txt"
#define CLIENT_FILE_CAPACITY 1024
#define PREFS_ID 0
#define PREFS_VERSION 4
/* Three lines each: the title on up to two, then the details. With the
 * input area closed there is room for more, and the rows grow to fill it. */
#define RESULT_ROWS_MIN 3
#define RESULT_ROW_HEIGHT_MIN 38
#define RESULT_ROWS_MAX 6
/* Where the list starts, and what the status line below it takes */
#define RESULT_TOP 31
#define RESULT_BOTTOM 14



/* Small helpers */

void *GetObject(UInt16 id)
{
    FormType *formP = FrmGetActiveForm();
    return FrmGetObjectPtr(formP, FrmGetObjectIndex(formP, id));
}

Boolean OnForm(UInt16 formId)
{
    return FrmGetActiveFormID() == formId;
}

void Append(Char *dstP, UInt16 capacity, const Char *textP)
{
    UInt16 length = StrLen(dstP);
    TextAppend(dstP, capacity, &length, textP);
}

void AppendNumber(Char *dstP, UInt16 capacity, UInt32 value)
{
    UInt16 length = StrLen(dstP);
    TextAppendUInt32(dstP, capacity, &length, value);
}

void AppendHex(Char *dstP, UInt16 capacity, UInt16 value)
{
    static const Char kDigits[] = "0123456789ABCDEF";
    Char text[7];
    UInt16 i;
    text[0] = '0';
    text[1] = 'x';
    for (i = 0; i < 4; i++)
        text[2 + i] = kDigits[(value >> (12 - 4 * i)) & 0x0F];
    text[6] = '\0';
    Append(dstP, capacity, text);
}

/* "1.4 MB" */
void AppendSize(Char *dstP, UInt16 capacity, UInt32 bytes)
{
    AppendNumber(dstP, capacity, bytes / 1048576UL);
    Append(dstP, capacity, ".");
    AppendNumber(dstP, capacity, (bytes % 1048576UL) * 10UL / 1048576UL);
    Append(dstP, capacity, " MB");
}

/* "2.3 s" */
void AppendSeconds(Char *dstP, UInt16 capacity, UInt32 ticks)
{
    UInt32 perSecond = SysTicksPerSecond();
    UInt32 tenths = ticks * 10UL / perSecond;
    AppendNumber(dstP, capacity, tenths / 10UL);
    Append(dstP, capacity, ".");
    AppendNumber(dstP, capacity, tenths % 10UL);
    Append(dstP, capacity, " s");
}

void SetFieldText(UInt16 fieldId, Char *textP)
{
    FieldType *fieldP = (FieldType *)GetObject(fieldId);
    FldSetTextPtr(fieldP, textP);
    FldRecalculateField(fieldP, true);
    FldDrawField(fieldP);
}

static void SetStatus(AppState *s, const Char *textP)
{
    StrNCopy(s->status, textP, sizeof(s->status) - 1);
    s->status[sizeof(s->status) - 1] = '\0';
    if (OnForm(MainForm)) SetFieldText(StatusField, s->status);
}

void SetProgressText(AppState *s, const Char *textP)
{
    StrNCopy(s->progress, textP, sizeof(s->progress) - 1);
    s->progress[sizeof(s->progress) - 1] = '\0';
    if (OnForm(DetailForm)) SetFieldText(ProgressField, s->progress);
}

void ShowObject(UInt16 id, Boolean visible)
{
    FormType *formP = FrmGetActiveForm();
    UInt16 index = FrmGetObjectIndex(formP, id);
    if (visible) FrmShowObject(formP, index);
    else FrmHideObject(formP, index);
}

void DrawProgressBar(const AppState *s)
{
    FormType *formP;
    RectangleType bounds, fill, frame;
    if (!OnForm(DetailForm)) return;
    formP = FrmGetActiveForm();
    FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, ProgressGadget),
        &bounds);
    /* The frame is drawn just outside the bounds. */
    frame = bounds;
    frame.topLeft.x--;
    frame.topLeft.y--;
    frame.extent.x += 2;
    frame.extent.y += 2;
    WinEraseRectangle(&frame, 0);
    if (s->percent == 0 && s->task != taskDownload) return;
    WinDrawRectangleFrame(rectangleFrame, &bounds);
    fill = bounds;
    fill.extent.x = (Coord)((UInt32)bounds.extent.x * s->percent / 100UL);
    WinDrawRectangle(&fill, 0);
}

/* Libraries */

static Err OpenTlsLibrary(AppState *s)
{
    Err error = SysLibFind(PALM_TLS_LIB_NAME, &s->tlsRefNum);
    if (error != errNone) {
        error = SysLibLoad(sysFileTLibrary, PALM_TLS_LIB_CREATOR,
            &s->tlsRefNum);
        if (error != errNone) return error;
        s->tlsLoaded = true;
    }
    error = PalmTlsLibOpen(s->tlsRefNum);
    if (error != errNone) return error;
    s->tlsOpen = true;
    error = PalmTlsLibGetCapabilities(s->tlsRefNum, &s->tlsCapabilities);
    if (error != errNone) return error;
    if ((s->tlsCapabilities & (PALM_TLS_CAP_TLS_1_2 | PALM_TLS_CAP_TLS_1_3)) == 0 ||
        (s->tlsCapabilities & (PALM_TLS_CAP_SESSIONS | PALM_TLS_CAP_CA_STORE |
            PALM_TLS_CAP_COOPERATIVE_IO)) != (PALM_TLS_CAP_SESSIONS |
            PALM_TLS_CAP_CA_STORE | PALM_TLS_CAP_COOPERATIVE_IO))
        return sysErrParamErr;
    return errNone;
}

static void CloseTlsLibrary(AppState *s)
{
    if (s->tlsOpen) PalmTlsLibClose(s->tlsRefNum);
    if (s->tlsLoaded) SysLibRemove(s->tlsRefNum);
}

static Err ConfigureNetwork(AppState *s)
{
    Boolean native = (s->tlsCapabilities & PALM_TLS_CAP_NATIVE_ARM) != 0;
    const UInt8 *trustP;
    s->trustH = DmGetResource('cert', TrustCertificates);
    if (s->trustH == 0) return dmErrResourceNotFound;
    trustP = (const UInt8 *)MemHandleLock(s->trustH);
    MemSet(&s->netConfig, sizeof(s->netConfig), 0);
    s->netConfig.tlsRefNum = s->tlsRefNum;
    s->netConfig.tlsProtocol = (s->tlsCapabilities & PALM_TLS_CAP_TLS_1_3)
        ? palmTlsProtocolTls13 : palmTlsProtocolTls12;
    s->netConfig.trustP = trustP;
    s->netConfig.trustLength = MemHandleSize(s->trustH);
    s->netConfig.stepTimeoutTicks = (Int32)SysTicksPerSecond() / 2;
    if (s->netConfig.stepTimeoutTicks < 1) s->netConfig.stepTimeoutTicks = 1;
    s->netConfig.operationTimeoutTicks = (Int32)SysTicksPerSecond() *
        (native ? ARM_TIMEOUT_SECONDS : M68K_TIMEOUT_SECONDS);
    return errNone;
}

/* Brings up Wi-Fi on first use; the OS shows its own connection dialog. */
/* Turning the screen off turns Wi-Fi off and can close Net.lib without
 * telling the app. */
Boolean NetworkOpen(AppState *s)
{
    UInt16 count = 0;
    if (!s->netOpen) return false;
    if (NetLibOpenCount(s->netRefNum, &count) == errNone && count != 0)
        return true;
    s->netOpen = false;
    return false;
}

Err EnsureNetwork(AppState *s)
{
    UInt16 interfaceErrors = 0;
    Err error;
    if (NetworkOpen(s)) return errNone;
    error = SysLibFind("Net.lib", &s->netRefNum);
    if (error != errNone) return error;
    error = NetLibOpen(s->netRefNum, &interfaceErrors);
    if (error == netErrAlreadyOpen) error = errNone;
    if (error == errNone && interfaceErrors != 0) {
        NetLibClose(s->netRefNum, true);
        return interfaceErrors;
    }
    if (error != errNone) return error;
    s->netOpen = true;
    s->netConfig.netRefNum = s->netRefNum;
    /* Jobs keep the log pointer, so it stays the same log. */
    if (s->logP != 0)
        UdpLogReconnect(s->logP);
    else if (s->logDest[0] != '\0')
        s->logP = s->netConfig.logP = UdpLogOpen(s->netRefNum, s->logDest);
    return errNone;
}

/* The "log=" line of client.txt, which YtClientParse skips. */
static void ReadLogDest(AppState *s, const Char *textP, UInt16 length)
{
    UInt16 i = 0, n = 0;
    while (i + 4 < length) {
        if (StrNCompare(textP + i, "log=", 4) == 0) {
            for (i += 4; i < length && n < sizeof(s->logDest) - 1 &&
                 textP[i] != '\r' && textP[i] != '\n'; i++)
                s->logDest[n++] = textP[i];
            s->logDest[n] = '\0';
            return;
        }
        while (i < length && textP[i] != '\n') i++;
        i++;
    }
}

static void LoadSettings(AppState *s)
{
    UInt16 size = sizeof(s->prefs), i;
    Char *textP;
    UInt16 length = 0;
    MemSet(&s->prefs, sizeof(s->prefs), 0);
    /* Version 3 had everything before quality. */
    if (PrefGetAppPreferences(APP_CREATOR, PREFS_ID, &s->prefs, &size, true) ==
            noPreferenceFound || (size != sizeof(s->prefs) &&
            size != OffsetOf(AppPrefs, quality))) {
        MemSet(&s->prefs, sizeof(s->prefs), 0);
        s->prefs.volume = VOLUME_DEFAULT;
    }
    s->prefs.visitor[sizeof(s->prefs.visitor) - 1] = '\0';
    for (i = 0; i < HISTORY_MAX; i++)
        s->prefs.history[i][HISTORY_CAPACITY - 1] = '\0';
    if (s->prefs.volume >= VOLUME_LEVELS) s->prefs.volume = VOLUME_DEFAULT;
    if (s->prefs.quality >= YT_VIDEO_CHOICES) s->prefs.quality = quality144;
    YtClientDefaults(&s->client);
    /* A client.txt on the card overrides the client when YouTube changes
     * what it accepts; see README. */
    if (StoreOpenVolume(&s->volume) != errNone) return;
    textP = (Char *)MemPtrNew(CLIENT_FILE_CAPACITY);
    if (textP == 0) return;
    if (StoreReadText(s->volume, CLIENT_FILE, textP, CLIENT_FILE_CAPACITY,
            &length) == errNone) {
        YtClientParse(&s->client, textP, length);
        ReadLogDest(s, textP, length);
    }
    MemPtrFree(textP);
}

/* CJKOS draws Japanese in place of Western text, as Shift-JIS: the
 * encoding Palm's Japanese devices use. */
static void SetupText(AppState *s)
{
    s->codec.encoding = textWestern;
    /* Only with CJKOS on do these bytes start two-byte characters. */
    if ((TxtByteAttr(0x82) & byteAttrFirst) == 0) return;
    if (s->jisH == 0) {
        s->jisH = DmGetResource('Tjis', JisTable);
        if (s->jisH == 0) {
            s->codec.encoding = textAscii;
            return;
        }
        s->codec.jisP = (const UInt8 *)MemHandleLock(s->jisH);
        s->codec.jisCount = (UInt16)(MemHandleSize(s->jisH) / 4);
    }
    s->codec.encoding = textShiftJis;
}

void SaveVisitor(AppState *s, const Char *visitorP)
{
    if (visitorP[0] == '\0' || StrCompare(visitorP, s->prefs.visitor) == 0)
        return;
    StrCopy(s->prefs.visitor, visitorP);
    PrefSetAppPreferences(APP_CREATOR, PREFS_ID, PREFS_VERSION, &s->prefs,
        sizeof(s->prefs), true);
}

/* Error text for a finished network job */

void DescribeResult(const NetJobResult *r, Boolean toCard, Char *dstP,
                    UInt16 capacity)
{
    static const Char kPhases[5][8] = {
        "DNS", "Connect", "TLS", "Send", "Receive"
    };
    switch (r->status) {
        case netJobCancelled:
            Append(dstP, capacity, "Cancelled.");
            return;
        case netJobHttpError:
            if (r->httpStatus != 0) {
                Append(dstP, capacity, "Server answered HTTP ");
                AppendNumber(dstP, capacity, r->httpStatus);
            } else {
                Append(dstP, capacity, "Bad HTTP response ");
                AppendNumber(dstP, capacity, r->platformError);
            }
            return;
        case netJobSinkError:
            Append(dstP, capacity, toCard
                ? "Could not write to the card." : "Unexpected reply format.");
            return;
        case netJobTlsError:
            /* PalmTLS status / wolfSSL error, and NetLib's if any */
            Append(dstP, capacity, "TLS failed ");
            AppendNumber(dstP, capacity, r->tlsStatus);
            Append(dstP, capacity, r->tlsError < 0 ? "/-" : "/");
            AppendNumber(dstP, capacity, (UInt32)(r->tlsError < 0
                ? -(Int32)r->tlsError : r->tlsError));
            if (r->netError != 0) {
                Append(dstP, capacity, ", net ");
                AppendHex(dstP, capacity, r->netError);
            }
            break;
        default:
            Append(dstP, capacity, "Network error ");
            AppendHex(dstP, capacity, r->netError);
            break;
    }
    if (r->phase < 5) {
        Append(dstP, capacity, " (");
        Append(dstP, capacity, kPhases[r->phase]);
        Append(dstP, capacity, ")");
    }
}

void DescribeFailure(const AppState *s, Char *dstP, UInt16 capacity)
{
    dstP[0] = '\0';
    DescribeResult(&s->jobResult, s->task == taskDownload, dstP, capacity);
}

/* JSON responses */

static Int16 FeedJson(void *contextP, const UInt8 *dataP, UInt16 length)
{
    AppState *s = (AppState *)contextP;
    return JsonScanFeed(&s->scanner, dataP, length) == jsonOk ? 0 : 1;
}

/* Starts an InnerTube API request. Failures are reported in the status
 * line for searches and in the progress text otherwise. */
Boolean StartApiJob(AppState *s, UInt16 task, UInt16 requestLength)
{
    Char message[48];
    Err error;
    message[0] = '\0';
    if (requestLength == 0) {
        Append(message, sizeof(message), "The request is too long.");
    } else if ((error = EnsureNetwork(s)) != errNone) {
        Append(message, sizeof(message), "Wi-Fi unavailable, error ");
        AppendHex(message, sizeof(message), (UInt16)error);
    } else {
        StrCopy(s->host, YT_API_HOST);
        if (NetJobStart(&s->jobP, &s->netConfig, s->host, s->request,
                requestLength, FeedJson, s, &s->jobResult)) {
            s->task = task;
            s->taskStartTicks = TimGetTicks();
            return true;
        }
        Append(message, sizeof(message), "Out of memory.");
    }
    if (task == taskSearch) SetStatus(s, message);
    else StrCopy(s->progress, message);
    return false;
}

/* Main form */

static void GetResultBounds(RectangleType *boundsP)
{
    FormType *formP = FrmGetActiveForm();
    FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, ResultGadget), boundsP);
}

static void AppendDetail(Char *lineP, UInt16 capacity, const Char *textP)
{
    if (textP[0] == '\0') return;
    if (lineP[0] != '\0') Append(lineP, capacity, " \xB7 ");
    Append(lineP, capacity, textP);
}

static void AppendShort(Char *lineP, UInt16 capacity, const Char *textP)
{
    Char text[20];
    TextShorten(textP, text, sizeof(text));
    AppendDetail(lineP, capacity, text);
}

/* A gray box with the duration on it, in place of a thumbnail that has not
 * arrived. Returns false if there is no memory for it. */
static Boolean DrawPlaceholder(const RectangleType *thumbP, const Char *durationP)
{
    BitmapType *bitsP, *bitmapP = 0;
    Coord width, height;
    UInt16 rowBytes, error;
    Boolean drawn = false;
    bitsP = BmpCreate(THUMB_LIST_WIDTH, THUMB_LIST_HEIGHT, 16, 0, &error);
    if (bitsP != 0) {
        BmpGetDimensions(bitsP, &width, &height, &rowBytes);
        if (rowBytes == (UInt16)(THUMB_LIST_WIDTH * 2)) {
            ThumbPlaceholder((UInt16 *)BmpGetBits(bitsP), durationP);
            bitmapP = (BitmapType *)BmpCreateBitmapV3(bitsP, kDensityDouble,
                BmpGetBits(bitsP), 0);
        }
    }
    if (bitmapP != 0) {
        WinDrawBitmap(bitmapP, thumbP->topLeft.x, thumbP->topLeft.y);
        drawn = true;
    }
    FreeBitmap(&bitmapP, &bitsP);
    return drawn;
}

/* A result: its thumbnail, or a gray box until it arrives, the title on
 * one or two lines, cut with "..." after two, and "1.8B · 10y ago ·
 * Channel". Drawn in native pixels for a hairline under each result. */
void DrawResultRow(const AppState *s, UInt16 row)
{
    static const RGBColorType kText = { 0, 0x00, 0x00, 0x00 };
    static const RGBColorType kDetails = { 0, 0x80, 0x80, 0x80 };
    static const RGBColorType kLine = { 0, 0xC8, 0xC8, 0xC8 };
    static const RGBColorType kFocus = { 0, 0xC8, 0xDC, 0xF8 };
    static const RGBColorType kPlaceholder = { 0, 0xE0, 0xE0, 0xE0 };
    UInt16 item = s->resultTop + row;
    const YtSearchItem *itemP = &s->searchP->items[item];
    const Thumb *thumbP = &s->thumbs[item];
    RectangleType bounds, thumb;
    UInt16 coordinates, first, lines;
    Coord margin, lineHeight, left, width, y, bottom;
    Char line[80];
    GetResultBounds(&bounds);
    bounds.topLeft.y += row * s->resultRowHeight;
    bounds.extent.y = s->resultRowHeight;
    WinPushDrawState();
    coordinates = WinSetCoordinateSystem(kCoordinatesNative);
    WinScaleRectangle(&bounds);
    WinSetBackColor(UIColorGetTableEntryIndex(UIFormFill));
    WinEraseRectangle(&bounds, 0);
    if (item < s->searchP->count) {
        bottom = bounds.topLeft.y + bounds.extent.y - 1;
        WinSetForeColorRGB(&kLine, 0);
        WinDrawLine(bounds.topLeft.x, bottom,
            bounds.topLeft.x + bounds.extent.x - 1, bottom);
        bounds.extent.y--;
        if (item == s->resultFocus) {
            WinSetBackColorRGB(&kFocus, 0);
            WinEraseRectangle(&bounds, 0);
        }
        FntSetFont(stdFont);
        lineHeight = FntLineHeight();
        margin = WinScaleCoord(2, false);
        RctSetRectangle(&thumb, bounds.topLeft.x + margin, bounds.topLeft.y +
            (bounds.extent.y - THUMB_LIST_HEIGHT) / 2, THUMB_LIST_WIDTH,
            THUMB_LIST_HEIGHT);
        if (thumbP->bitmapP != 0) {
            WinDrawBitmap(thumbP->bitmapP, thumb.topLeft.x, thumb.topLeft.y);
        } else if (!DrawPlaceholder(&thumb, itemP->length)) {
            WinSetForeColorRGB(&kPlaceholder, 0);
            WinDrawRectangle(&thumb, 0);
        }
        left = thumb.topLeft.x + thumb.extent.x + 2 * margin;
        width = bounds.topLeft.x + bounds.extent.x - margin - left;
        first = FntWordWrap(itemP->title, (UInt16)width);
        lines = itemP->title[first] != '\0' ? 3 : 2;
        y = bounds.topLeft.y + (bounds.extent.y - lines * lineHeight) / 2;
        WinSetTextColorRGB(&kText, 0);
        WinDrawTruncChars(itemP->title, (Int16)first, left, y, width);
        if (lines == 3) {
            y += lineHeight;
            WinDrawTruncChars(itemP->title + first,
                (Int16)StrLen(itemP->title + first), left, y, width);
        }
        line[0] = '\0';
        if (itemP->live) AppendDetail(line, sizeof(line), "Live");
        AppendShort(line, sizeof(line), itemP->views);
        AppendShort(line, sizeof(line), itemP->published);
        AppendDetail(line, sizeof(line), itemP->channel);
        WinSetTextColorRGB(&kDetails, 0);
        WinDrawTruncChars(line, StrLen(line), left, y + lineHeight, width);
    }
    WinSetCoordinateSystem(coordinates);
    WinPopDrawState();
}

static void DrawResults(const AppState *s)
{
    UInt16 row;
    for (row = 0; row < s->resultRows; row++) DrawResultRow(s, row);
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

static void ScrollResults(AppState *s, Int16 rows)
{
    Int16 top = (Int16)s->resultTop + rows;
    s->resultTop = top > 0 ? (UInt16)top : 0;
    ShowResults(s, true);
}

/* Highlights item, or nothing with NO_RESULT. */
static void FocusResult(AppState *s, UInt16 item)
{
    UInt16 previous = s->resultFocus;
    s->resultFocus = item;
    if (previous - s->resultTop < s->resultRows)
        DrawResultRow(s, previous - s->resultTop);
    if (item != previous && item - s->resultTop < s->resultRows)
        DrawResultRow(s, item - s->resultTop);
}

/* Moves the highlight by rows, scrolling to keep it in view. */
static void MoveResultFocus(AppState *s, Int16 rows)
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

static void OpenResult(AppState *s, UInt16 item)
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

Boolean HasInputArea(void)
{
    UInt32 version;
    return FtrGet(pinCreator, pinFtrAPIVersion, &version) == errNone;
}

/* The form resizes with the input area, in LayoutMainForm and
 * LayoutDetailForm. */
static void UseWholeScreen(FormType *formP)
{
    if (!HasInputArea()) return;
    FrmSetDIAPolicyAttr(formP, frmDIAPolicyCustom);
    WinSetConstraintsSize(FrmGetWindowHandle(formP), 160, 240, 240, 160, 160, 160);
}

/* The user decides whether the input area is open, once for every screen
 * that resizes. The system remembers the choice, but forms that do not
 * resize, like the player's, open the area meanwhile; each form puts the
 * user's choice back when it comes up. */
static void EnterInputArea(void)
{
    if (!HasInputArea()) return;
    PINSetInputTriggerState(pinInputTriggerEnabled);
    PINSetInputAreaState(pinInputAreaUser);
}

/* The forms that follow the input area */
static Boolean IsWholeScreenForm(UInt16 formId)
{
    return formId == MainForm || formId == DetailForm || formId == SavedForm ||
        formId == PlayForm;
}

void SetObjectY(FormType *formP, UInt16 index, Coord y, Coord height)
{
    RectangleType bounds;
    FrmGetObjectBounds(formP, index, &bounds);
    bounds.topLeft.y = y;
    if (height != 0) bounds.extent.y = height;
    FrmSetObjectBounds(formP, index, &bounds);
}

/* Fits the form to the screen: the list takes the space above the status
 * line, in whole rows that grow to fill it. The input area opening or
 * closing changes the screen's height. */
void LayoutMainForm(AppState *s)
{
    FormType *formP = FrmGetActiveForm();
    RectangleType bounds;
    Coord width, height, listHeight;
    UInt16 i, rows;
    if (!HasInputArea()) return;
    WinGetDisplayExtent(&width, &height);
    RctSetRectangle(&bounds, 0, 0, width, height);
    WinSetBounds(FrmGetWindowHandle(formP), &bounds);
    listHeight = height - RESULT_TOP - RESULT_BOTTOM;
    rows = (UInt16)(listHeight / RESULT_ROW_HEIGHT_MIN);
    if (rows < RESULT_ROWS_MIN) rows = RESULT_ROWS_MIN;
    if (rows > RESULT_ROWS_MAX) rows = RESULT_ROWS_MAX;
    s->resultRows = rows;
    s->resultRowHeight = listHeight / (Coord)rows;
    listHeight = s->resultRowHeight * (Coord)rows;
    SetObjectY(formP, FrmGetObjectIndex(formP, ResultGadget), RESULT_TOP, listHeight);
    SetObjectY(formP, FrmGetObjectIndex(formP, ResultScrollBar), RESULT_TOP, listHeight);
    SetObjectY(formP, FrmGetObjectIndex(formP, StatusField),
        height - RESULT_BOTTOM, 0);
    for (i = 0; i < FrmGetNumberOfObjects(formP); i++)
        if (FrmGetObjectType(formP, i) == frmGraffitiStateObj) {
            Coord x, y;
            FrmGetObjectPosition(formP, i, &x, &y);
            FrmSetObjectPosition(formP, i, x, height - RESULT_BOTTOM + 1);
        }
    ShowResults(s, false);
}

static void InitMainForm(AppState *s)
{
    FieldType *fieldP = (FieldType *)GetObject(SearchField);
    FldSetTextHandle(fieldP, s->searchTextH);
    {
        FormType *formP = FrmGetActiveForm();
        UInt16 index = FrmGetObjectIndex(formP, ResultGadget);
        FrmSetGadgetData(formP, index, s);
        FrmSetGadgetHandler(formP, index, ResultGadgetHandler);
    }
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
static UInt16 ShowHistory(AppState *s)
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

static void StartSearch(AppState *s)
{
    FieldType *fieldP = (FieldType *)GetObject(SearchField);
    const Char *queryP = FldGetTextPtr(fieldP);
    StopThumbs(s);
    if (s->task != taskIdle || queryP == 0 || queryP[0] == '\0') return;
    RememberSearch(s, queryP);
    FreeThumbs(s);
    YtSearchInit(s->searchP, &s->codec);
    s->resultTop = 0;
    s->resultFocus = NO_RESULT;
    ShowResults(s, true);
    JsonScanInit(&s->scanner, s->jsonValue, sizeof(s->jsonValue),
        YtSearchValue, YtSearchEnd, s->searchP);
    SetStatus(s, "Searching...");
    StartApiJob(s, taskSearch, YtBuildSearchRequest(&s->client,
        s->prefs.visitor, queryP, &s->codec, s->body, s->request,
        sizeof(s->request)));
}

static void FinishSearch(AppState *s)
{
    Char message[80];
    if (s->jobResult.status != netJobOk ||
        JsonScanFinish(&s->scanner) != jsonOk) {
        if (s->jobResult.status == netJobOk)
            StrCopy(message, "Unexpected reply from YouTube.");
        else
            DescribeFailure(s, message, sizeof(message));
        SetStatus(s, message);
        return;
    }
    SaveVisitor(s, s->searchP->visitor);
    message[0] = '\0';
    AppendNumber(message, sizeof(message), s->searchP->count);
    Append(message, sizeof(message), " results in ");
    AppendSeconds(message, sizeof(message), TimGetTicks() - s->taskStartTicks);
    SetStatus(s, s->searchP->count == 0 ? "No results." : message);
    if (OnForm(MainForm)) ShowResults(s, true);
    StartThumbs(s);
}

/* Event loop glue */

static void FinishJob(AppState *s)
{
    UInt16 task = s->task;
    if (task == taskSearch) {
        s->task = taskIdle;
        FinishSearch(s);
    } else if (task == taskPlayer) {
        s->task = taskIdle;
        FinishPlayer(s);
        RefreshDetail(s);
        StartThumbs(s);
    } else if (task == taskDownload) {
        FinishChunk(s);
        RefreshDetail(s);
    } else if (task == taskThumbs) {
        ThumbsEnded(s);
    }
}

/* Runs job steps for a short slice so transfers are not limited to one
 * step per event. */
static void AdvanceJob(AppState *s)
{
    UInt32 start = TimGetTicks();
    /* Thumbnails wait while the list is in use: a decode or a handshake
     * step cannot be interrupted. */
    if (s->task == taskThumbs &&
            start - s->lastInputTicks < SysTicksPerSecond() / 2)
        return;
    while (s->jobP != 0) {
        UInt16 progress = NetJobStep(&s->jobP);
        if (progress == netJobFinished) {
            FinishJob(s);
            return;
        }
        /* Only thumbnail downloads keep connections open. */
        if (progress == netJobIdle) ThumbArrived(s);
        if (TimGetTicks() - start >= STEP_SLICE_TICKS || EvtSysEventAvail(true))
            return;
    }
}

static void CancelJob(AppState *s)
{
    if (s->jobP != 0) NetJobCancel(s->jobP);
}

static void ReturnToMain(AppState *s)
{
    if (s->jobP != 0) {
        NetJobCancel(s->jobP);
        while (s->jobP != 0) NetJobStep(&s->jobP);
    }
    CloseFile(s);
    s->task = taskIdle;
    StopThumbs(s);
    FreeBitmap(&s->detailThumbP, &s->detailBitsP);
    FrmReturnToForm(MainForm);
    LayoutMainForm(s);
    FrmDrawForm(FrmGetActiveForm());
    ShowResults(s, true);
    StartThumbs(s);
}

/* A 5-way button pressed just now, as palmOne reports it */
Boolean NavPressed(const EventType *eventP, UInt16 bit, UInt16 change)
{
    return eventP->data.keyDown.chr == vchrNavChange &&
        (eventP->data.keyDown.keyCode & (bit | change)) == (bit | change);
}

static Boolean HandleEvent(EventType *eventP, AppState *s)
{
    FormType *formP;
    /* Not one of eventsEnum's values, so not a case below. */
    if (eventP->eType == winDisplayChangedEvent) {
        if (OnForm(FullForm)) {
            FillFullScreen(s);
        } else if (OnForm(MainForm)) {
            LayoutMainForm(s);
            FrmDrawForm(FrmGetActiveForm());
        } else if (OnForm(DetailForm)) {
            LayoutDetailForm();
            FrmDrawForm(FrmGetActiveForm());
            RefreshDetail(s);
        } else if (OnForm(SavedForm)) {
            LayoutSavedForm();
            FrmDrawForm(FrmGetActiveForm());
        } else if (OnForm(PlayForm)) {
            LayoutPlayForm();
            FrmDrawForm(FrmGetActiveForm());
            ClearVideoArea();
            DrawSeekBar(s);
            DrawVolume(s);
            if (s->playbackP != 0) {
                AttachPlayScreen(s);
                s->playWait = 0;
            }
        }
        return false;
    }
    switch (eventP->eType) {
        case nilEvent:
            AdvanceJob(s);
            return false;
        case frmLoadEvent:
            formP = FrmInitForm(eventP->data.frmLoad.formID);
            /* Before the form is active: the system opens the input area for
             * a form that has not said it handles it. */
            if (IsWholeScreenForm(eventP->data.frmLoad.formID))
                UseWholeScreen(formP);
            FrmSetActiveForm(formP);
            return true;
        case frmOpenEvent:
            if (OnForm(MainForm)) {
                InitMainForm(s);
                EnterInputArea();
                LayoutMainForm(s);
                FrmDrawForm(FrmGetActiveForm());
            } else if (OnForm(SavedForm)) {
                EnterInputArea();
                LayoutSavedForm();
                OpenSavedForm(s);
                FrmDrawForm(FrmGetActiveForm());
            } else if (OnForm(PlayForm)) {
                EnterInputArea();
                LayoutPlayForm();
                OpenPlayForm(s);
            } else if (OnForm(FullForm)) {
                OpenFullForm(s);
            } else if (OnForm(DetailForm)) {
                EnterInputArea();
                LayoutDetailForm();
                FrmSetFocus(FrmGetActiveForm(), noFocus);
                ShowObject(DownloadButton, false);
                ShowObject(PlayButton, false);
                StartPlayer(s);
                FldSetTextPtr((FieldType *)GetObject(HeadField), s->head);
                FldSetTextPtr((FieldType *)GetObject(InfoField), s->info);
                FldSetTextPtr((FieldType *)GetObject(ProgressField),
                    s->progress);
                FrmDrawForm(FrmGetActiveForm());
                RefreshDetail(s);
            }
            return true;
        case frmUpdateEvent:
            FrmDrawForm(FrmGetActiveForm());
            if (OnForm(DetailForm)) RefreshDetail(s);
            if (OnForm(PlayForm)) {
                ClearVideoArea();
                DrawSeekBar(s);
                DrawVolume(s);
            }
            if (OnForm(FullForm)) FillFullScreen(s);
            return true;
        case lstSelectEvent:
            if (eventP->data.lstSelect.listID == SavedList &&
                eventP->data.lstSelect.selection >= 0 &&
                (UInt16)eventP->data.lstSelect.selection < s->savedCount) {
                ShowSavedActions(true);
                return true;
            }
            break;
        case ctlSelectEvent:
            switch (eventP->data.ctlSelect.controlID) {
                case SearchButton: StartSearch(s); return true;
                case DownloadButton: StartDownload(s); return true;
                case CancelButton: CancelJob(s); return true;
                case BackButton: ReturnToMain(s); return true;
                case SavedButton:
                    StopThumbs(s);
                    if (s->task == taskIdle) FrmPopupForm(SavedForm);
                    return true;
                case SavedBackButton: ReturnFromSaved(s); return true;
                case SavedPlayButton: PlaySaved(s); return true;
                case SavedDeleteButton: DeleteSaved(s); return true;
                /* With no history, nothing pops up. */
                case HistoryTrigger: return ShowHistory(s) == 0;
                case PlayButton:
                    StopThumbs(s);
                    if (s->saved) StrCopy(s->playPath, s->outPath);
                    else s->playPath[0] = '\0';
                    FrmPopupForm(PlayForm);
                    return true;
                case PlayPauseButton: TogglePause(s); return true;
                case PlayBackButton: ReturnFromPlay(s); return true;
                default: break;
            }
            break;
        case popSelectEvent:
            if (eventP->data.popSelect.controlID == DetailQualityTrigger) {
                Int16 selection = eventP->data.popSelect.selection;
                if (selection >= 0 && selection < YT_VIDEO_CHOICES)
                    SwitchDetailQuality(s, (UInt16)selection);
                return true;
            }
            if (eventP->data.popSelect.controlID == PlayQualityTrigger) {
                Int16 selection = eventP->data.popSelect.selection;
                if (selection >= 0 && selection < YT_VIDEO_CHOICES)
                    SwitchPlayQuality(s, (UInt16)selection);
                return true;
            }
            /* Handled here so the trigger keeps its empty label. */
            if (eventP->data.popSelect.controlID == HistoryTrigger) {
                Int16 selection = eventP->data.popSelect.selection;
                FieldType *fieldP = (FieldType *)GetObject(SearchField);
                if (selection >= 0 && selection < HISTORY_MAX) {
                    const Char *queryP = s->prefs.history[selection];
                    FldSetSelection(fieldP, 0, FldGetTextLength(fieldP));
                    FldInsert(fieldP, queryP, StrLen(queryP));
                    StartSearch(s);
                }
                return true;
            }
            break;
        case penMoveEvent:
        case penUpEvent:
            if (OnForm(PlayForm) && s->playbackP != 0 &&
                    (TrackSeekBar(s, eventP) || TrackVolume(s, eventP)))
                return true;
            break;
        case penDownEvent:
            if (OnForm(PlayForm) && s->playbackP != 0) {
                RectangleType bounds;
                if (TrackSeekBar(s, eventP) || TrackVolume(s, eventP))
                    return true;
                GetVideoBounds(&bounds);
                if (RctPtInRectangle(eventP->screenX, eventP->screenY, &bounds)) {
                    EnterFullScreen(s);
                    return true;
                }
            }
            if (OnForm(FullForm)) {
                ExitFullScreen(s);
                return true;
            }
            break;
        case winExitEvent:
            /* Something is about to cover the video: pause under it. */
            if (s->playbackP != 0 && s->playbackP->playing &&
                IsPlayWindow(s, eventP->data.winExit.exitWindow)) {
                PlaybackPause(s->playbackP);
                s->resumeOnEnter = true;
            }
            break;
        case winEnterEvent:
            /* Also when a dialog or pop-up closes: there is no frmOpenEvent. */
            if (IsWholeScreenForm(FrmGetActiveFormID()) &&
                    eventP->data.winEnter.enterWindow ==
                    FrmGetWindowHandle(FrmGetActiveForm()))
                EnterInputArea();
            if (s->playbackP != 0 && s->resumeOnEnter &&
                IsPlayWindow(s, eventP->data.winEnter.enterWindow)) {
                s->resumeOnEnter = false;
                PlaybackPlay(s->playbackP);
                s->playWait = 0;
            }
            break;
        case keyDownEvent:
            if (OnForm(MainForm) && (eventP->data.keyDown.chr == chrLineFeed ||
                    eventP->data.keyDown.chr == chrCarriageReturn)) {
                StartSearch(s);
                return true;
            }
            if (OnForm(MainForm)) {
                WChar c = eventP->data.keyDown.chr;
                if (c == vchrPageUp || c == vchrRockerUp ||
                        c == vchrPageDown || c == vchrRockerDown) {
                    MoveResultFocus(s, c == vchrPageUp || c == vchrRockerUp
                        ? -1 : 1);
                    return true;
                }
                /* One press may arrive as both of these. */
                if (c == vchrRockerCenter ||
                        NavPressed(eventP, navBitSelect, navChangeSelect)) {
                    UInt32 now = TimGetTicks();
                    if (now - s->lastCentre >= SysTicksPerSecond() / 4 &&
                            s->resultFocus != NO_RESULT)
                        OpenResult(s, s->resultFocus);
                    s->lastCentre = now;
                    return true;
                }
            }
            if ((OnForm(PlayForm) || OnForm(FullForm)) && HandlePlayKey(s, eventP))
                return true;
            if (OnForm(DetailForm) && (eventP->data.keyDown.chr == vchrPageUp ||
                    eventP->data.keyDown.chr == vchrPageDown)) {
                ScrollInfo(eventP->data.keyDown.chr == vchrPageUp ? -5 : 5);
                return true;
            }
            break;
        case sclRepeatEvent:
        case sclExitEvent:
            if (OnForm(MainForm)) {
                Int16 to = eventP->eType == sclRepeatEvent
                    ? eventP->data.sclRepeat.newValue
                    : eventP->data.sclExit.newValue;
                ScrollResults(s, to - (Int16)s->resultTop);
                /* Returning false keeps a held arrow repeating. */
                return eventP->eType == sclExitEvent;
            }
            if (OnForm(DetailForm)) {
                Int16 from = eventP->eType == sclRepeatEvent
                    ? eventP->data.sclRepeat.value : eventP->data.sclExit.value;
                Int16 to = eventP->eType == sclRepeatEvent
                    ? eventP->data.sclRepeat.newValue
                    : eventP->data.sclExit.newValue;
                ScrollInfo(to - from);
                return true;
            }
            break;
        case menuEvent:
            if (eventP->data.menu.itemID == MenuAbout) {
                FrmAlert(AboutAlert);
                return true;
            }
            if (eventP->data.menu.itemID == MenuClearHistory) {
                MemSet(s->prefs.history, sizeof(s->prefs.history), 0);
                s->prefsChanged = true;
                SetStatus(s, "Search history cleared.");
                return true;
            }
            if (eventP->data.menu.itemID == MenuResetVisitor) {
                s->prefs.visitor[0] = '\0';
                PrefSetAppPreferences(APP_CREATOR, PREFS_ID, PREFS_VERSION,
                    &s->prefs, sizeof(s->prefs), true);
                SetStatus(s, "YouTube visitor ID cleared.");
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

static Err StartApplication(AppState *s)
{
    Char errorText[12];
    Err error;
    s->searchP = (YtSearch *)MemPtrNew(sizeof(YtSearch));
    s->playerP = (YtPlayer *)MemPtrNew(sizeof(YtPlayer));
    s->searchTextH = MemHandleNew(64);
    if (s->searchP == 0 || s->playerP == 0 || s->searchTextH == 0)
        return memErrNotEnoughSpace;
    YtSearchInit(s->searchP, &s->codec);
    s->resultFocus = NO_RESULT;
    s->resultRows = RESULT_ROWS_MIN;
    s->resultRowHeight = RESULT_ROW_HEIGHT_MIN;
    s->thumbItem = NO_RESULT;
    YtPlayerInit(s->playerP, kVideoItags, kAudioItags, &s->codec);
    *(Char *)MemHandleLock(s->searchTextH) = '\0';
    MemHandleUnlock(s->searchTextH);
    StrCopy(s->status, "Type a search and tap Search.");
    error = OpenTlsLibrary(s);
    if (error == errNone) error = ConfigureNetwork(s);
    if (error != errNone) {
        errorText[0] = '\0';
        AppendNumber(errorText, sizeof(errorText), (UInt16)error);
        FrmCustomAlert(LibraryMissingAlert, errorText, "", "");
        return error;
    }
    LoadSettings(s);
    SetupText(s);
    return errNone;
}

static void StopApplication(AppState *s)
{
    if (s->fullScreen) ShowSystemBars(s);
    ClosePlayback(s);
    if (s->prefsChanged)
        PrefSetAppPreferences(APP_CREATOR, PREFS_ID, PREFS_VERSION, &s->prefs,
            sizeof(s->prefs), true);
    if (s->jobP != 0) {
        NetJobCancel(s->jobP);
        while (s->jobP != 0) NetJobStep(&s->jobP);
    }
    CloseFile(s);
    FreeThumbs(s);
    {
        /* The search text handle belongs to the app; detach it so closing
         * the form does not free it. */
        FormType *mainP = FrmGetFormPtr(MainForm);
        if (mainP != 0)
            FldSetTextHandle((FieldType *)FrmGetObjectPtr(mainP,
                FrmGetObjectIndex(mainP, SearchField)), 0);
    }
    FrmCloseAllForms();
    if (s->savedNamesP != 0) MemPtrFree(s->savedNamesP);
    if (s->jisH != 0) {
        MemHandleUnlock(s->jisH);
        DmReleaseResource(s->jisH);
    }
    if (s->trustH != 0) {
        MemHandleUnlock(s->trustH);
        DmReleaseResource(s->trustH);
    }
    UdpLogClose(s->logP);
    if (s->netOpen) NetLibClose(s->netRefNum, false);
    CloseTlsLibrary(s);
    if (s->searchTextH != 0) MemHandleFree(s->searchTextH);
    if (s->searchP != 0) MemPtrFree(s->searchP);
    if (s->playerP != 0) MemPtrFree(s->playerP);
}

UInt32 PilotMain(UInt16 cmd, void *cmdPBP, UInt16 launchFlags)
{
    AppState *s;
    EventType event;
    UInt32 romVersion = 0;
    Err error;
    (void)cmdPBP;
    (void)launchFlags;
    if (cmd != sysAppLaunchCmdNormalLaunch) return errNone;
    FtrGet(sysFtrCreator, sysFtrNumROMVersion, &romVersion);
    if (romVersion < sysMakeROMVersion(5, 0, 0, sysROMStageRelease, 0)) {
        FrmAlert(RomIncompatibleAlert);
        return sysErrRomIncompatible;
    }
    s = (AppState *)MemPtrNew(sizeof(AppState));
    if (s == 0) return memErrNotEnoughSpace;
    MemSet(s, sizeof(*s), 0);
    error = StartApplication(s);
    if (error == errNone) {
        FrmGotoForm(MainForm);
        s->playWait = evtWaitForever;
        do {
            Int32 timeout = s->jobP != 0 ? 1 : evtWaitForever;
            if (s->playbackP != 0 && s->playWait != evtWaitForever &&
                    (timeout == evtWaitForever || s->playWait < timeout))
                timeout = s->playWait;
            EvtGetEvent(&event, timeout);
            if (event.eType == penDownEvent || event.eType == penMoveEvent ||
                    event.eType == keyDownEvent || event.eType == sclRepeatEvent)
                s->lastInputTicks = TimGetTicks();
            if (!SysHandleEvent(&event) && !MenuHandleEvent(0, &event, &error) &&
                !HandleEvent(&event, s))
                FrmDispatchEvent(&event);
            if (event.eType != appStopEvent) StepPlayback(s);
            UdpLogFlush(s->logP);
        } while (event.eType != appStopEvent);
    }
    StopApplication(s);
    MemPtrFree(s);
    return errNone;
}
