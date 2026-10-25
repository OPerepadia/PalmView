/* SPDX-License-Identifier: GPL-3.0-or-later */

/* PalmView: search YouTube, download a video's 144p H.264 and AAC streams
 * over PalmTLS, merge them into one MP4 on the memory card, and play it. */

#include <PalmOS.h>

#include "json_scan.h"
#include "mp4_merge.h"
#include "net_job.h"
#include "palm_tls.h"
#include "playback.h"
#include "resource.h"
#include "store.h"
#include "stream_feed.h"
#include "text.h"
#include "thumb.h"
#include "udp_log.h"
#include "yt_api.h"

#define CHUNK_BYTES (4UL * 1024UL * 1024UL)
#define ARM_TIMEOUT_SECONDS 30
#define M68K_TIMEOUT_SECONDS 600
#define STEP_SLICE_TICKS 10
#define PROGRESS_INTERVAL_TICKS 50
#define CLIENT_FILE STORE_DIR "/client.txt"
#define CLIENT_FILE_CAPACITY 1024
#define PREFS_ID 0
#define PREFS_VERSION 3
#define MAX_SAVED 32
#define SAVED_NAME_CAPACITY 64
/* Redirects in a row a download follows. */
#define MAX_REDIRECTS 5
/* Recent searches kept, newest first; the search field holds 63 chars. */
#define HISTORY_MAX 8
#define HISTORY_CAPACITY 64
/* palmOne's 5-way reports its buttons this way too (PalmChars.h). */
#define vchrNavChange 0x0503
#define navBitLeft 0x0004
#define navBitRight 0x0008
#define navBitSelect 0x0010
#define navChangeLeft 0x0400
#define navChangeRight 0x0800
#define navChangeSelect 0x1000
/* 5-way left and right skip about this far, once no press has come for
 * SKIP_DELAY_TICKS: presses in a row add up to one seek. */
#define SKIP_MS 10000UL
#define SKIP_DELAY_TICKS (SysTicksPerSecond() / 2)
/* Level 0 is silent. */
#define VOLUME_LEVELS 11
#define VOLUME_DEFAULT 6
#define RESULT_ROWS 5
#define RESULT_ROW_HEIGHT 23
/* Leaves room for the title's tab around the text. */
#define PLAY_TITLE_WIDTH 150
#define NO_RESULT 0xFFFF

/* 3 dB apart; 1024 is unity gain. */
static const UInt16 kVolumeGain[VOLUME_LEVELS] = {
    0, 45, 64, 91, 128, 181, 256, 362, 512, 724, 1024
};

static const UInt16 kVideoItags[] = { 160, 0 };
static const UInt16 kAudioItags[] = { 140, 139, 0 };

typedef enum Task {
    taskIdle = 0,
    taskSearch,
    taskPlayer,
    taskDownload,
    taskThumb           /* in the background: Play and Download stop it */
} Task;

typedef struct AppPrefs {
    Char visitor[YT_VISITOR_CAPACITY];
    UInt16 volume;              /* level, below VOLUME_LEVELS */
    Char history[HISTORY_MAX][HISTORY_CAPACITY];    /* "" when unused */
} AppPrefs;

typedef struct AppState {
    UInt16 tlsRefNum;
    Boolean tlsLoaded;
    Boolean tlsOpen;
    UInt32 tlsCapabilities;
    UInt16 netRefNum;
    Boolean netOpen;
    MemHandle trustH;
    NetJobConfig netConfig;

    YtClient client;
    AppPrefs prefs;
    YtSearch *searchP;
    YtPlayer *playerP;
    UInt16 selected;
    UInt16 resultTop;           /* result in the first row */
    UInt16 resultFocus;         /* highlighted result, or NO_RESULT */

    JsonScanner scanner;
    Char jsonValue[YT_JSON_VALUE_CAPACITY];
    Char host[64];
    Char request[YT_REQUEST_CAPACITY];
    Char body[YT_BODY_CAPACITY];
    NetJob *jobP;
    NetJobResult jobResult;
    UInt16 task;
    UInt32 taskStartTicks;
    UInt8 *thumbJpegP;          /* while the thumbnail downloads */
    UInt16 thumbLength;
    BitmapType *thumbBitsP;     /* the decoded pixels */
    BitmapType *thumbP;         /* the same, drawn at double density */

    UInt16 volume;
    UInt16 stream;
    FileRef file;
    Boolean fileOpen;
    UInt32 offset;
    UInt32 sessionBytes;
    UInt32 sessionStartTicks;
    UInt32 lastProgressTicks;
    UInt16 redirects;
    Char location[YT_URL_CAPACITY];     /* where a redirect points */
    Char videoPath[STORE_PATH_CAPACITY];
    Char audioPath[STORE_PATH_CAPACITY];
    Char outPath[STORE_PATH_CAPACITY];

    MemHandle searchTextH;
    Char status[80];
    Char head[144];             /* title and channel, beside the thumbnail */
    Char info[384];
    Char progress[192];
    UInt16 percent;
    Boolean saved;              /* the selected video is on the card */

    Char *savedNamesP;          /* MAX_SAVED names, while SavedForm is up */
    UInt16 savedCount;
    Char savedStatus[48];

    Playback *playbackP;        /* while PlayForm is up */
    StreamFeed *feedP;          /* while streaming */
    Int32 playWait;             /* ticks until playback needs to run */
    UInt32 playStatusTicks;
    Boolean resumeOnEnter;
    Boolean prefsChanged;       /* saved when the app stops */
    UInt32 lastCentre;          /* ticks of the last 5-way centre press */
    UInt32 lastSkip;            /* and of the last left or right one */
    Boolean seekDragging;       /* the pen is on the seek bar */
    Boolean volumeDragging;     /* or on the volume bars */
    Boolean skipPending;        /* 5-way presses not applied yet */
    UInt32 seekTargetMs;        /* where either points */
    Char *historyItems[HISTORY_MAX];    /* the history list's choices */
    Char logDest[24];           /* "a.b.c.d:port" from client.txt, or "" */
    UdpLog *logP;               /* 0 without a log= line */
    Char playPath[STORE_PATH_CAPACITY];     /* empty to stream */
    Char playTitle[64];
    Char playStatus[128];
} AppState;

/* Small helpers */

static void *GetObject(UInt16 id)
{
    FormType *formP = FrmGetActiveForm();
    return FrmGetObjectPtr(formP, FrmGetObjectIndex(formP, id));
}

static Boolean OnForm(UInt16 formId)
{
    return FrmGetActiveFormID() == formId;
}

static void Append(Char *dstP, UInt16 capacity, const Char *textP)
{
    UInt16 length = StrLen(dstP);
    TextAppend(dstP, capacity, &length, textP);
}

static void AppendNumber(Char *dstP, UInt16 capacity, UInt32 value)
{
    UInt16 length = StrLen(dstP);
    TextAppendUInt32(dstP, capacity, &length, value);
}

static void AppendHex(Char *dstP, UInt16 capacity, UInt16 value)
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
static void AppendSize(Char *dstP, UInt16 capacity, UInt32 bytes)
{
    AppendNumber(dstP, capacity, bytes / 1048576UL);
    Append(dstP, capacity, ".");
    AppendNumber(dstP, capacity, (bytes % 1048576UL) * 10UL / 1048576UL);
    Append(dstP, capacity, " MB");
}

/* "2.3 s" */
static void AppendSeconds(Char *dstP, UInt16 capacity, UInt32 ticks)
{
    UInt32 perSecond = SysTicksPerSecond();
    UInt32 tenths = ticks * 10UL / perSecond;
    AppendNumber(dstP, capacity, tenths / 10UL);
    Append(dstP, capacity, ".");
    AppendNumber(dstP, capacity, tenths % 10UL);
    Append(dstP, capacity, " s");
}

static void SetFieldText(UInt16 fieldId, Char *textP)
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

static void SetProgressText(AppState *s, const Char *textP)
{
    StrNCopy(s->progress, textP, sizeof(s->progress) - 1);
    s->progress[sizeof(s->progress) - 1] = '\0';
    if (OnForm(DetailForm)) SetFieldText(ProgressField, s->progress);
}

static void ShowObject(UInt16 id, Boolean visible)
{
    FormType *formP = FrmGetActiveForm();
    UInt16 index = FrmGetObjectIndex(formP, id);
    if (visible) FrmShowObject(formP, index);
    else FrmHideObject(formP, index);
}

static void DrawProgressBar(const AppState *s)
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
static Err EnsureNetwork(AppState *s)
{
    UInt16 interfaceErrors = 0;
    Err error;
    if (s->netOpen) return errNone;
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
    if (s->logDest[0] != '\0')
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
    if (PrefGetAppPreferences(APP_CREATOR, PREFS_ID, &s->prefs, &size, true) ==
            noPreferenceFound || size != sizeof(s->prefs)) {
        MemSet(&s->prefs, sizeof(s->prefs), 0);
        s->prefs.volume = VOLUME_DEFAULT;
    }
    s->prefs.visitor[sizeof(s->prefs.visitor) - 1] = '\0';
    for (i = 0; i < HISTORY_MAX; i++)
        s->prefs.history[i][HISTORY_CAPACITY - 1] = '\0';
    if (s->prefs.volume >= VOLUME_LEVELS) s->prefs.volume = VOLUME_DEFAULT;
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

static void SaveVisitor(AppState *s, const Char *visitorP)
{
    if (visitorP[0] == '\0' || StrCompare(visitorP, s->prefs.visitor) == 0)
        return;
    StrCopy(s->prefs.visitor, visitorP);
    PrefSetAppPreferences(APP_CREATOR, PREFS_ID, PREFS_VERSION, &s->prefs,
        sizeof(s->prefs), true);
}

/* Error text for a finished network job */

static void DescribeResult(const NetJobResult *r, Boolean toCard, Char *dstP,
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

static void DescribeFailure(const AppState *s, Char *dstP, UInt16 capacity)
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
static Boolean StartApiJob(AppState *s, UInt16 task, UInt16 requestLength)
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

/* A result in two lines: the title, then "1.8B · 10y ago · Channel".
 * Drawn in native pixels for a hairline under each result. */
static void DrawResultRow(const AppState *s, UInt16 row)
{
    static const RGBColorType kText = { 0, 0x00, 0x00, 0x00 };
    static const RGBColorType kDetails = { 0, 0x80, 0x80, 0x80 };
    static const RGBColorType kLine = { 0, 0xC8, 0xC8, 0xC8 };
    static const RGBColorType kFocus = { 0, 0xC8, 0xDC, 0xF8 };
    UInt16 item = s->resultTop + row;
    const YtSearchItem *itemP = &s->searchP->items[item];
    RectangleType bounds;
    UInt16 coordinates;
    Coord margin, lineHeight, width, bottom;
    Char line[80];
    GetResultBounds(&bounds);
    bounds.topLeft.y += row * RESULT_ROW_HEIGHT;
    bounds.extent.y = RESULT_ROW_HEIGHT;
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
        line[0] = '\0';
        if (itemP->live) AppendDetail(line, sizeof(line), "Live");
        AppendShort(line, sizeof(line), itemP->views);
        AppendShort(line, sizeof(line), itemP->published);
        AppendDetail(line, sizeof(line), itemP->channel);
        FntSetFont(stdFont);
        lineHeight = FntLineHeight();
        margin = WinScaleCoord(2, false);
        width = bounds.extent.x - 2 * margin;
        bounds.topLeft.x += margin;
        bounds.topLeft.y += (bounds.extent.y - 2 * lineHeight) / 2;
        WinSetTextColorRGB(&kText, 0);
        WinDrawTruncChars(itemP->title, StrLen(itemP->title),
            bounds.topLeft.x, bounds.topLeft.y, width);
        WinSetTextColorRGB(&kDetails, 0);
        WinDrawTruncChars(line, StrLen(line), bounds.topLeft.x,
            bounds.topLeft.y + lineHeight, width);
    }
    WinSetCoordinateSystem(coordinates);
    WinPopDrawState();
}

static void DrawResults(const AppState *s)
{
    UInt16 row;
    for (row = 0; row < RESULT_ROWS; row++) DrawResultRow(s, row);
}

static void ShowResults(AppState *s, Boolean draw)
{
    UInt16 count = s->searchP->count;
    UInt16 lastTop = count > RESULT_ROWS ? count - RESULT_ROWS : 0;
    if (s->resultTop > lastTop) s->resultTop = lastTop;
    SclSetScrollBar((ScrollBarType *)GetObject(ResultScrollBar),
        (Int16)s->resultTop, 0, (Int16)lastTop, RESULT_ROWS);
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
    if (previous - s->resultTop < RESULT_ROWS)
        DrawResultRow(s, previous - s->resultTop);
    if (item != previous && item - s->resultTop < RESULT_ROWS)
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
    if ((UInt16)item < s->resultTop || (UInt16)item >= s->resultTop + RESULT_ROWS) {
        s->resultFocus = (UInt16)item;
        s->resultTop = (UInt16)item < s->resultTop
            ? (UInt16)item : (UInt16)item - RESULT_ROWS + 1;
        ShowResults(s, true);
    } else {
        FocusResult(s, (UInt16)item);
    }
}

static void OpenResult(AppState *s, UInt16 item)
{
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
    item = s->resultTop + (UInt16)(y - boundsP->topLeft.y) / RESULT_ROW_HEIGHT;
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
    if (s->task != taskIdle || queryP == 0 || queryP[0] == '\0') return;
    RememberSearch(s, queryP);
    YtSearchInit(s->searchP);
    s->resultTop = 0;
    s->resultFocus = NO_RESULT;
    ShowResults(s, true);
    JsonScanInit(&s->scanner, s->jsonValue, sizeof(s->jsonValue),
        YtSearchValue, YtSearchEnd, s->searchP);
    SetStatus(s, "Searching...");
    StartApiJob(s, taskSearch, YtBuildSearchRequest(&s->client,
        s->prefs.visitor, queryP, s->body, s->request, sizeof(s->request)));
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
}

/* Detail form */

static void UpdateInfoScrollBar(void)
{
    FieldType *fieldP = (FieldType *)GetObject(InfoField);
    UInt16 position, textHeight, fieldHeight, maximum;
    FldGetScrollValues(fieldP, &position, &textHeight, &fieldHeight);
    maximum = textHeight > fieldHeight ? textHeight - fieldHeight : 0;
    SclSetScrollBar((ScrollBarType *)GetObject(InfoScrollBar),
        (Int16)position, 0, (Int16)maximum,
        (Int16)(fieldHeight > 1 ? fieldHeight - 1 : 1));
}

static void ScrollInfo(Int16 lines)
{
    FieldType *fieldP = (FieldType *)GetObject(InfoField);
    if (lines > 0) FldScrollField(fieldP, (UInt16)lines, winDown);
    else if (lines < 0) FldScrollField(fieldP, (UInt16)-lines, winUp);
    UpdateInfoScrollBar();
}

/* Appends textP as a field width wide wraps it, cut with "..." after two
 * lines. */
static void AppendTwoLines(Char *dstP, UInt16 capacity, const Char *textP,
                           Coord width)
{
    UInt16 length = StrLen(dstP), first, second;
    FontID font = FntSetFont(stdFont);
    first = FntWordWrap(textP, (UInt16)width);
    second = StrLen(textP + first);
    if (FntWordWrap(textP + first, (UInt16)width) < second) {
        while (second > 0 && FntCharsWidth(textP + first, (Int16)second) +
                FntCharWidth(chrEllipsis) > width)
            second--;
        while (second > 0 && textP[first + second - 1] == ' ') second--;
        if (length + first + second + 2 <= capacity) {
            MemMove(dstP + length, textP, first + second);
            dstP[length + first + second] = chrEllipsis;
            dstP[length + first + second + 1] = '\0';
        }
    } else {
        Append(dstP, capacity, textP);
    }
    FntSetFont(font);
}

static void BuildInfo(AppState *s)
{
    const YtSearchItem *itemP = &s->searchP->items[s->selected];
    const YtPlayer *playerP = s->playerP;
    Char *infoP = s->info;
    UInt16 capacity = sizeof(s->info);
    RectangleType bounds;
    FormType *formP = FrmGetActiveForm();
    FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, HeadField), &bounds);
    s->head[0] = '\0';
    AppendTwoLines(s->head, sizeof(s->head), playerP->title[0] != '\0'
        ? playerP->title : itemP->title, bounds.extent.x);
    Append(s->head, sizeof(s->head), "\n");
    Append(s->head, sizeof(s->head), playerP->author[0] != '\0'
        ? playerP->author : itemP->channel);
    infoP[0] = '\0';
    Append(infoP, capacity, itemP->length);
    if (itemP->views[0] != '\0') {
        Append(infoP, capacity, " | ");
        Append(infoP, capacity, itemP->views);
    }
    if (itemP->published[0] != '\0') {
        Append(infoP, capacity, " | ");
        Append(infoP, capacity, itemP->published);
    }
    if (playerP->video.found && playerP->audio.found) {
        Append(infoP, capacity, "\n144p video ");
        AppendSize(infoP, capacity, playerP->video.contentLength);
        Append(infoP, capacity, ", audio ");
        AppendSize(infoP, capacity, playerP->audio.contentLength);
    }
}

/* Thumbnail */

static Int16 FeedThumb(void *contextP, const UInt8 *dataP, UInt16 length)
{
    AppState *s = (AppState *)contextP;
    if (length > THUMB_JPEG_CAPACITY - s->thumbLength) return 1;
    MemMove(s->thumbJpegP + s->thumbLength, dataP, length);
    s->thumbLength += length;
    return 0;
}

static void FreeThumb(AppState *s)
{
    if (s->thumbJpegP != 0) MemPtrFree(s->thumbJpegP);
    s->thumbJpegP = 0;
    if (s->thumbP != 0) BmpDelete(s->thumbP);
    s->thumbP = 0;
    if (s->thumbBitsP != 0) BmpDelete(s->thumbBitsP);
    s->thumbBitsP = 0;
}

/* Fetches the selected video's thumbnail, after its stream info. */
static void StartThumb(AppState *s)
{
    UInt16 length;
    FreeThumb(s);
    s->thumbJpegP = (UInt8 *)MemPtrNew(THUMB_JPEG_CAPACITY);
    s->thumbLength = 0;
    length = YtBuildThumbRequest(&s->client,
        s->searchP->items[s->selected].videoId, s->request, sizeof(s->request));
    StrCopy(s->host, YT_THUMB_HOST);
    if (s->thumbJpegP != 0 && length != 0 && NetJobStart(&s->jobP,
            &s->netConfig, s->host, s->request, length, FeedThumb, s,
            &s->jobResult))
        s->task = taskThumb;
    else
        FreeThumb(s);
}

static void StopThumb(AppState *s)
{
    if (s->task != taskThumb) return;
    if (s->jobP != 0) {
        NetJobCancel(s->jobP);
        while (s->jobP != 0) NetJobStep(&s->jobP);
    }
    s->task = taskIdle;
    FreeThumb(s);
}

static void FinishThumb(AppState *s)
{
    void *workP = MemPtrNew(THUMB_WORK_SIZE);
    Coord width, height;
    UInt16 rowBytes, error;
    if (s->jobResult.status == netJobOk && workP != 0 &&
            (s->thumbBitsP = BmpCreate(THUMB_WIDTH, THUMB_HEIGHT, 16, 0,
                &error)) != 0) {
        BmpGetDimensions(s->thumbBitsP, &width, &height, &rowBytes);
        if (rowBytes == THUMB_WIDTH * 2 && ThumbDecode(s->thumbJpegP,
                s->thumbLength, (UInt16 *)BmpGetBits(s->thumbBitsP), workP))
            s->thumbP = (BitmapType *)BmpCreateBitmapV3(s->thumbBitsP,
                kDensityDouble, BmpGetBits(s->thumbBitsP), 0);
    }
    if (workP != 0) MemPtrFree(workP);
    MemPtrFree(s->thumbJpegP);
    s->thumbJpegP = 0;
}

/* A light gray box stands in until the thumbnail arrives, or if it
 * does not. */
static void DrawThumb(const AppState *s)
{
    static const RGBColorType kPlaceholder = { 0, 0xE0, 0xE0, 0xE0 };
    FormType *formP;
    RectangleType bounds;
    if (!OnForm(DetailForm)) return;
    formP = FrmGetActiveForm();
    FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, ThumbGadget), &bounds);
    if (s->thumbP != 0) {
        WinDrawBitmap(s->thumbP, bounds.topLeft.x, bounds.topLeft.y);
        return;
    }
    WinPushDrawState();
    WinSetForeColorRGB(&kPlaceholder, 0);
    WinDrawRectangle(&bounds, 0);
    WinPopDrawState();
}

static void RefreshDetail(AppState *s)
{
    Boolean idle = s->task == taskIdle || s->task == taskThumb;
    if (!OnForm(DetailForm)) return;
    SetFieldText(HeadField, s->head);
    SetFieldText(InfoField, s->info);
    UpdateInfoScrollBar();
    SetFieldText(ProgressField, s->progress);
    ShowObject(DownloadButton, idle && YtPlayerPlayable(s->playerP));
    ShowObject(CancelButton, !idle);
    ShowObject(PlayButton, idle && (s->saved || YtPlayerPlayable(s->playerP)));
    DrawProgressBar(s);
    DrawThumb(s);
}

static void MakeOutPath(AppState *s)
{
    const YtSearchItem *itemP = &s->searchP->items[s->selected];
    Char name[64];
    TextFileName(s->playerP->title, itemP->videoId, ".mp4", name, sizeof(name));
    StoreMakePath(s->outPath, name);
}

static Boolean OutFileExists(AppState *s)
{
    FileRef file;
    UInt32 size = 0;
    if (StoreOpenVolume(&s->volume) != errNone ||
        StoreOpenRead(s->volume, s->outPath, &file, &size) != errNone)
        return false;
    VFSFileClose(file);
    return size != 0;
}

static void StartPlayer(AppState *s)
{
    const YtSearchItem *itemP = &s->searchP->items[s->selected];
    FreeThumb(s);
    YtPlayerInit(s->playerP, kVideoItags, kAudioItags);
    s->percent = 0;
    s->saved = false;
    BuildInfo(s);
    JsonScanInit(&s->scanner, s->jsonValue, sizeof(s->jsonValue),
        YtPlayerValue, YtPlayerEnd, s->playerP);
    StrCopy(s->progress, "Getting stream info...");
    StartApiJob(s, taskPlayer, YtBuildPlayerRequest(&s->client,
        s->prefs.visitor, itemP->videoId, s->body, s->request,
        sizeof(s->request)));
}

static void FinishPlayer(AppState *s)
{
    YtPlayer *playerP = s->playerP;
    if (s->jobResult.status != netJobOk ||
        JsonScanFinish(&s->scanner) != jsonOk) {
        if (s->jobResult.status == netJobOk)
            StrCopy(s->progress, "Unexpected reply from YouTube.");
        else
            DescribeFailure(s, s->progress, sizeof(s->progress));
    } else {
        SaveVisitor(s, playerP->visitor);
        BuildInfo(s);
        if (YtPlayerPlayable(playerP)) {
            MakeOutPath(s);
            s->saved = OutFileExists(s);
            s->progress[0] = '\0';
            if (s->saved) StrCopy(s->progress, "Already saved.");
        } else if (StrCompare(playerP->status, "OK") != 0) {
            StrCopy(s->progress, "YouTube: ");
            Append(s->progress, sizeof(s->progress), playerP->reason[0] != '\0'
                ? playerP->reason : playerP->status);
        } else if (playerP->live) {
            StrCopy(s->progress, "Live streams are not supported.");
        } else {
            StrCopy(s->progress, "No 144p H.264 + AAC streams offered.");
        }
    }
}

/* Download and merge */

static const YtFormat *CurrentFormat(const AppState *s)
{
    return s->stream == 0 ? &s->playerP->video : &s->playerP->audio;
}

static void CloseFile(AppState *s)
{
    if (s->fileOpen) VFSFileClose(s->file);
    s->fileOpen = false;
}

static void ShowTransfer(AppState *s, Boolean force)
{
    const YtFormat *formatP = CurrentFormat(s);
    UInt32 now = TimGetTicks();
    UInt32 elapsed = now - s->sessionStartTicks;
    if (!force && now - s->lastProgressTicks < PROGRESS_INTERVAL_TICKS) return;
    s->lastProgressTicks = now;
    StrCopy(s->progress, s->stream == 0 ? "Video " : "Audio ");
    AppendSize(s->progress, sizeof(s->progress), s->offset);
    Append(s->progress, sizeof(s->progress), " of ");
    AppendSize(s->progress, sizeof(s->progress), formatP->contentLength);
    if (elapsed >= SysTicksPerSecond()) {
        Append(s->progress, sizeof(s->progress), "\n");
        AppendNumber(s->progress, sizeof(s->progress),
            s->sessionBytes / 1024UL * SysTicksPerSecond() / elapsed);
        Append(s->progress, sizeof(s->progress), " KB/s");
    }
    if (s->jobResult.handshakeTicks != 0) {
        Append(s->progress, sizeof(s->progress), ", TLS handshake ");
        AppendSeconds(s->progress, sizeof(s->progress),
            s->jobResult.handshakeTicks);
    }
    s->percent = (UInt16)(formatP->contentLength == 0 ? 0
        : (s->offset / 1024UL) * 100UL / (formatP->contentLength / 1024UL + 1));
    if (OnForm(DetailForm)) {
        SetFieldText(ProgressField, s->progress);
        DrawProgressBar(s);
    }
}

static Int16 WriteMedia(void *contextP, const UInt8 *dataP, UInt16 length)
{
    AppState *s = (AppState *)contextP;
    if (StoreWrite(s->file, dataP, length) != errNone) return 1;
    s->offset += length;
    s->sessionBytes += length;
    ShowTransfer(s, false);
    return 0;
}

static void FailDownload(AppState *s, const Char *messageP)
{
    CloseFile(s);
    s->task = taskIdle;
    s->percent = 0;
    StrCopy(s->progress, messageP);
    Append(s->progress, sizeof(s->progress),
        "\nTap Download to resume.");
}

static void StartMerge(AppState *s);

static void StartChunk(AppState *s)
{
    const YtFormat *formatP = CurrentFormat(s);
    const Char *pathP;
    UInt32 end = s->offset + CHUNK_BYTES - 1;
    UInt16 length;
    if (end >= formatP->contentLength) end = formatP->contentLength - 1;
    length = YtBuildMediaRequest(&s->client, formatP->url, s->offset, end,
        false, s->request, sizeof(s->request));
    if (length == 0 || !YtSplitUrl(formatP->url, s->host, sizeof(s->host),
            &pathP)) {
        FailDownload(s, "Stream URL is not usable.");
        return;
    }
    if (!NetJobStart(&s->jobP, &s->netConfig, s->host, s->request, length,
            WriteMedia, s, &s->jobResult)) {
        FailDownload(s, "Out of memory.");
        return;
    }
    NetJobKeepLocation(s->jobP, s->location, sizeof(s->location));
}

/* Opens the next stream's file and resumes it where it stopped. */
static void StartStream(AppState *s)
{
    while (s->stream < 2) {
        const YtFormat *formatP = CurrentFormat(s);
        const Char *pathP = s->stream == 0 ? s->videoPath : s->audioPath;
        Err error = StoreOpenAppend(s->volume, pathP, &s->file, &s->offset);
        if (error == errNone && s->offset > formatP->contentLength) {
            VFSFileClose(s->file);
            error = StoreCreate(s->volume, pathP, &s->file);
            s->offset = 0;
        }
        if (error != errNone) {
            Char message[64];
            StrCopy(message, "Could not create a file on the card, error ");
            AppendHex(message, sizeof(message), (UInt16)error);
            Append(message, sizeof(message), ".");
            FailDownload(s, message);
            return;
        }
        s->fileOpen = true;
        if (s->offset < formatP->contentLength) {
            ShowTransfer(s, true);
            StartChunk(s);
            return;
        }
        CloseFile(s);
        s->stream++;
    }
    StartMerge(s);
}

static void StartDownload(AppState *s)
{
    const YtPlayer *playerP = s->playerP;
    const YtSearchItem *itemP = &s->searchP->items[s->selected];
    Char name[64];
    UInt32 freeBytes = 0, needed;
    Err error;
    StopThumb(s);
    if (s->task != taskIdle || !YtPlayerPlayable(playerP)) return;
    error = StoreOpenVolume(&s->volume);
    if (error != errNone) {
        SetProgressText(s, "No memory card found.");
        return;
    }
    /* Both streams plus the merged copy. */
    needed = (playerP->video.contentLength + playerP->audio.contentLength) *
        2UL + 65536UL;
    if (StoreFreeBytes(s->volume, &freeBytes) == errNone && freeBytes < needed) {
        StrCopy(s->progress, "Not enough space on the card: need ");
        AppendSize(s->progress, sizeof(s->progress), needed);
        Append(s->progress, sizeof(s->progress), ".");
        RefreshDetail(s);
        return;
    }
    error = EnsureNetwork(s);
    if (error != errNone) {
        SetProgressText(s, "Wi-Fi unavailable.");
        return;
    }
    StrCopy(name, itemP->videoId);
    StrCat(name, ".video.mp4");
    StoreMakePath(s->videoPath, name);
    StrCopy(name, itemP->videoId);
    StrCat(name, ".audio.mp4");
    StoreMakePath(s->audioPath, name);
    MakeOutPath(s);
    s->saved = false;
    s->task = taskDownload;
    s->taskStartTicks = TimGetTicks();
    s->sessionStartTicks = s->taskStartTicks;
    s->sessionBytes = 0;
    s->lastProgressTicks = 0;
    s->redirects = 0;
    s->stream = 0;
    RefreshDetail(s);
    StartStream(s);
    RefreshDetail(s);
}

static void FinishChunk(AppState *s)
{
    Char message[80];
    /* YouTube moves some streams to another server. */
    if (s->jobResult.redirected) {
        YtFormat *formatP = s->stream == 0 ? &s->playerP->video
            : &s->playerP->audio;
        if (++s->redirects <= MAX_REDIRECTS &&
            YtApplyRedirect(formatP->url, sizeof(formatP->url), s->location)) {
            StartChunk(s);
            return;
        }
    }
    s->redirects = 0;
    if (s->jobResult.status != netJobOk) {
        DescribeFailure(s, message, sizeof(message));
        FailDownload(s, message);
        return;
    }
    if (s->jobResult.received == 0) {
        FailDownload(s, "The server sent no data.");
        return;
    }
    if (s->offset < CurrentFormat(s)->contentLength) {
        StartChunk(s);
        return;
    }
    CloseFile(s);
    s->stream++;
    StartStream(s);
}

static Boolean MergeProgress(void *contextP, UInt32 done, UInt32 total)
{
    AppState *s = (AppState *)contextP;
    UInt32 now = TimGetTicks();
    EvtResetAutoOffTimer();
    if (now - s->lastProgressTicks < PROGRESS_INTERVAL_TICKS) return true;
    s->lastProgressTicks = now;
    s->percent = (UInt16)((done / 1024UL) * 100UL / (total / 1024UL + 1));
    DrawProgressBar(s);
    return true;
}

static void StartMerge(AppState *s)
{
    Mp4Io io;
    Mp4MergeResult result;
    FileRef videoFile = 0, audioFile = 0, outFile = 0;
    Err error;
    MemSet(&io, sizeof(io), 0);
    s->percent = 0;
    SetProgressText(s, "Merging video and audio...");
    DrawProgressBar(s);
    error = StoreOpenRead(s->volume, s->videoPath, &videoFile, &io.videoSize);
    if (error == errNone)
        error = StoreOpenRead(s->volume, s->audioPath, &audioFile,
            &io.audioSize);
    if (error == errNone) error = StoreCreate(s->volume, s->outPath, &outFile);
    if (error != errNone) {
        Char message[64];
        if (videoFile != 0) VFSFileClose(videoFile);
        if (audioFile != 0) VFSFileClose(audioFile);
        StrCopy(message, "Could not open the streams for merging, error ");
        AppendHex(message, sizeof(message), (UInt16)error);
        Append(message, sizeof(message), ".");
        FailDownload(s, message);
        return;
    }
    io.readProcP = StoreMp4Read;
    io.writeProcP = StoreMp4Write;
    io.progressProcP = MergeProgress;
    io.contextP = s;
    io.videoP = (void *)videoFile;
    io.audioP = (void *)audioFile;
    io.outP = (void *)outFile;
    Mp4Merge(&io, &result);
    VFSFileClose(videoFile);
    VFSFileClose(audioFile);
    VFSFileClose(outFile);
    s->task = taskIdle;
    if (result.status != mp4MergeOk) {
        VFSFileDelete(s->volume, s->outPath);
        StrCopy(s->progress, "Merge failed: status ");
        AppendNumber(s->progress, sizeof(s->progress), result.status);
        Append(s->progress, sizeof(s->progress), ", line ");
        AppendNumber(s->progress, sizeof(s->progress), result.detail);
        s->percent = 0;
        return;
    }
    VFSFileDelete(s->volume, s->videoPath);
    VFSFileDelete(s->volume, s->audioPath);
    s->percent = 100;
    s->saved = true;
    StrCopy(s->progress, "Saved ");
    Append(s->progress, sizeof(s->progress), s->outPath);
    Append(s->progress, sizeof(s->progress), " in ");
    AppendSeconds(s->progress, sizeof(s->progress),
        TimGetTicks() - s->taskStartTicks);
    Append(s->progress, sizeof(s->progress), ".");
}

/* Saved videos form */

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

static void OpenSavedForm(AppState *s)
{
    ListType *listP = (ListType *)GetObject(SavedList);
    UInt16 volume;
    if (s->savedNamesP == 0)
        s->savedNamesP = (Char *)MemPtrNew((UInt32)MAX_SAVED * SAVED_NAME_CAPACITY);
    s->savedCount = s->savedNamesP != 0 && StoreOpenVolume(&volume) == errNone
        ? StoreListVideos(volume, s->savedNamesP, SAVED_NAME_CAPACITY, MAX_SAVED)
        : 0;
    LstSetDrawFunction(listP, DrawSavedRow);
    LstSetListChoices(listP, (Char **)s->savedNamesP, (Int16)s->savedCount);
    LstSetSelection(listP, noListSelection);
    StrCopy(s->savedStatus, s->savedCount == 0 ? "No saved videos yet."
        : "Tap a video to play it.");
    FldSetTextPtr((FieldType *)GetObject(SavedStatusField), s->savedStatus);
}

static void ReturnFromSaved(AppState *s)
{
    FrmReturnToForm(MainForm);
    FrmDrawForm(FrmGetActiveForm());
    ShowResults(s, true);
    if (s->savedNamesP != 0) MemPtrFree(s->savedNamesP);
    s->savedNamesP = 0;
    s->savedCount = 0;
}

/* Play form */

/* "1:05" or "1:02:05" */
static void AppendClock(Char *dstP, UInt16 capacity, UInt32 ms)
{
    UInt32 seconds = ms / 1000UL;
    if (seconds >= 3600UL) {
        AppendNumber(dstP, capacity, seconds / 3600UL);
        Append(dstP, capacity, seconds % 3600UL < 600UL ? ":0" : ":");
        seconds %= 3600UL;
    }
    AppendNumber(dstP, capacity, seconds / 60UL);
    Append(dstP, capacity, seconds % 60UL < 10UL ? ":0" : ":");
    AppendNumber(dstP, capacity, seconds % 60UL);
}

static void GetVideoBounds(RectangleType *boundsP)
{
    FormType *formP = FrmGetActiveForm();
    FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, PlayVideoGadget), boundsP);
}

static void ClearVideoArea(void)
{
    RectangleType bounds;
    GetVideoBounds(&bounds);
    WinDrawRectangle(&bounds, 0);
}

static void GetSeekBounds(RectangleType *boundsP)
{
    FormType *formP = FrmGetActiveForm();
    FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, PlaySeekGadget), boundsP);
}

/* value / total of width, for times of many hours too. */
static Coord BarX(UInt32 value, UInt32 total, Coord width)
{
    if (total == 0) return 0;
    if (value > total) value = total;
    while (total > 0x003FFFFFUL) {
        total >>= 1;
        value >>= 1;
    }
    return (Coord)(value * (UInt32)width / total);
}

static void FillBar(RectangleType *rP, Coord width, UInt8 gray)
{
    RGBColorType color;
    color.r = gray;
    color.g = gray;
    color.b = gray;
    WinSetForeColorRGB(&color, 0);
    rP->extent.x = width;
    WinDrawRectangle(rP, 0);
}

/* In the color theme's title bar color */
static void FillThemeBar(RectangleType *rP, Coord width)
{
    WinSetForeColor(UIColorGetTableEntryIndex(UIFormFrame));
    rP->extent.x = width;
    WinDrawRectangle(rP, 0);
}

/* The bar under the video: played, downloaded, and not loaded yet. While
 * the pen drags it, it shows where the pen points. */
static void DrawSeekBar(const AppState *s)
{
    const Playback *p = s->playbackP;
    RectangleType bounds, bar;
    UInt32 duration = 0, position = 0, loaded = 0;
    Coord played;
    if (!OnForm(PlayForm)) return;
    GetSeekBounds(&bounds);
    WinEraseRectangle(&bounds, 0);
    if (p == 0 || (duration = PlaybackDurationMs(p)) == 0) return;
    position = s->seekDragging || s->skipPending ? s->seekTargetMs
        : PlaybackPositionMs(p);
    loaded = duration;
    if (s->feedP != 0) {
        UInt32 video = PlaybackBufferedMs(p, STREAM_FEED_VIDEO);
        UInt32 audio = PlaybackBufferedMs(p, STREAM_FEED_AUDIO);
        loaded = PlaybackPositionMs(p) + (video < audio ? video : audio);
    }
    played = BarX(position, duration, bounds.extent.x - 4);
    WinPushDrawState();
    RctSetRectangle(&bar, bounds.topLeft.x, bounds.topLeft.y + 2, 0, 2);
    FillBar(&bar, bounds.extent.x, 0xC8);
    FillBar(&bar, BarX(loaded, duration, bounds.extent.x), 0x80);
    FillThemeBar(&bar, played);
    RctSetRectangle(&bar, bounds.topLeft.x + played, bounds.topLeft.y + 1, 0, 4);
    FillThemeBar(&bar, 4);
    WinPopDrawState();
}

/* The time under the pen's x on the seek bar */
static UInt32 SeekBarMs(const AppState *s, Coord penX)
{
    RectangleType bounds;
    UInt32 duration = PlaybackDurationMs(s->playbackP), ms;
    GetSeekBounds(&bounds);
    if (penX <= bounds.topLeft.x) return 0;
    ms = (UInt32)(penX - bounds.topLeft.x) * (duration / 1000UL) /
        (UInt32)bounds.extent.x * 1000UL;
    return ms < duration ? ms : duration;
}

static void GetVolumeBounds(RectangleType *boundsP)
{
    FormType *formP = FrmGetActiveForm();
    FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, PlayVolumeGadget), boundsP);
}

/* Ten bars rising left to right; the colored ones show the volume. */
static void DrawVolume(const AppState *s)
{
    RectangleType bounds, bar;
    UInt16 coordinates, level;
    Coord step, shortest;
    if (!OnForm(PlayForm)) return;
    GetVolumeBounds(&bounds);
    WinPushDrawState();
    coordinates = WinSetCoordinateSystem(kCoordinatesNative);
    WinScaleRectangle(&bounds);
    WinEraseRectangle(&bounds, 0);
    step = bounds.extent.x / (VOLUME_LEVELS - 1);
    shortest = bounds.extent.y / 4;
    for (level = 1; level < VOLUME_LEVELS; level++) {
        bar.extent.y = shortest + (bounds.extent.y - shortest) * (level - 1) /
            (VOLUME_LEVELS - 2);
        bar.topLeft.x = bounds.topLeft.x + (level - 1) * step;
        bar.topLeft.y = bounds.topLeft.y + bounds.extent.y - bar.extent.y;
        if (level <= s->prefs.volume) FillThemeBar(&bar, step * 2 / 3);
        else FillBar(&bar, step * 2 / 3, 0xC8);
    }
    WinSetCoordinateSystem(coordinates);
    WinPopDrawState();
}

static void SetVolume(AppState *s, UInt16 level)
{
    if (level == s->prefs.volume) return;
    s->prefs.volume = level;
    s->prefsChanged = true;
    if (s->playbackP != 0)
        PlaybackSetVolume(s->playbackP, kVolumeGain[level]);
    DrawVolume(s);
}

/* Sets the volume to the bar under the pen. Left of the bars mutes. */
static Boolean TrackVolume(AppState *s, const EventType *eventP)
{
    RectangleType bounds;
    Coord step;
    UInt16 level;
    GetVolumeBounds(&bounds);
    if (eventP->eType == penDownEvent) {
        /* Take the whole height of the button row. */
        bounds.topLeft.y -= 2;
        bounds.extent.y += 3;
        if (!RctPtInRectangle(eventP->screenX, eventP->screenY, &bounds))
            return false;
        s->volumeDragging = true;
    } else if (!s->volumeDragging) {
        return false;
    }
    if (eventP->eType == penUpEvent) s->volumeDragging = false;
    step = bounds.extent.x / (VOLUME_LEVELS - 1);
    if (eventP->screenX < bounds.topLeft.x) level = 0;
    else level = (UInt16)((eventP->screenX - bounds.topLeft.x) / step) + 1;
    SetVolume(s, level < VOLUME_LEVELS ? level : VOLUME_LEVELS - 1);
    return true;
}

static void UpdatePlayStatus(AppState *s)
{
    static const Char kQuality[PLAYBACK_LEVELS][12] = {
        "Full", "Fast B", "Fast", "Half rate"
    };
    const Playback *p = s->playbackP;
    Char *textP = s->playStatus;
    UInt16 capacity = sizeof(s->playStatus);
    textP[0] = '\0';
    if (PlaybackError(p) != 0) {
        Append(textP, capacity, PlaybackError(p));
        /* What dropped the stream, then how the last retry failed. */
        if (s->feedP != 0 && StreamFeedFailure(s->feedP) != 0) {
            const NetJobResult *dropP = StreamFeedDrop(s->feedP);
            const NetJobResult *lastP = StreamFeedFailure(s->feedP);
            if (dropP != 0 && MemCmp(dropP, lastP, sizeof(*lastP)) != 0) {
                Append(textP, capacity, "\n");
                DescribeResult(dropP, false, textP, capacity);
            }
            Append(textP, capacity, "\n");
            DescribeResult(lastP, false, textP, capacity);
        }
        SetFieldText(PlayStatusField, textP);
        return;
    }
    AppendClock(textP, capacity, s->seekDragging || s->skipPending
        ? s->seekTargetMs : PlaybackPositionMs(p));
    Append(textP, capacity, " / ");
    AppendClock(textP, capacity, PlaybackDurationMs(p));
    if (s->seekDragging || s->skipPending) Append(textP, capacity, "  Seek");
    else if (p->ended) Append(textP, capacity, "  Ended");
    else if (!p->playing) Append(textP, capacity, "  Paused");
    else if (p->buffering || (s->feedP != 0 && !p->started))
        Append(textP, capacity, "  Buffering");
    Append(textP, capacity, "\n");
    AppendNumber(textP, capacity, p->fps10 / 10);
    Append(textP, capacity, ".");
    AppendNumber(textP, capacity, p->fps10 % 10);
    Append(textP, capacity, " fps, CPU ");
    AppendNumber(textP, capacity, p->cpuPercent);
    Append(textP, capacity, "%, skipped ");
    AppendNumber(textP, capacity, p->skipped);
    if (p->soundError != 0) {
        Append(textP, capacity, "\nNo sound: ");
        Append(textP, capacity, p->soundError);
    } else {
        Append(textP, capacity, "\n");
        Append(textP, capacity, kQuality[p->level]);
        Append(textP, capacity, ", ");
        AppendNumber(textP, capacity, p->soundUnderruns);
        Append(textP, capacity, " gaps");
    }
    if (s->feedP != 0) {
        Append(textP, capacity, ", ");
        AppendNumber(textP, capacity, p->netKBps);
        Append(textP, capacity, " KB/s");
    }
    if (p->decodeErrors + p->audioErrors != 0) {
        Append(textP, capacity, ", errors ");
        AppendNumber(textP, capacity, p->decodeErrors + p->audioErrors);
    }
    SetFieldText(PlayStatusField, textP);
}

static void ClosePlayback(AppState *s)
{
    if (s->playbackP != 0) {
        PlaybackClose(s->playbackP);
        MemPtrFree(s->playbackP);
        s->playbackP = 0;
    }
    StreamFeedFree(s->feedP);
    s->feedP = 0;
    s->playWait = evtWaitForever;
    s->resumeOnEnter = false;
}

/* Streams the selected video when playPath is empty. */
static const Char *StartPlayback(AppState *s)
{
    s->playbackP = (Playback *)MemPtrNew(sizeof(Playback));
    if (s->playbackP == 0) return "Out of memory.";
    if (s->playPath[0] != '\0') return PlaybackOpen(s->playbackP, s->playPath);
    if (EnsureNetwork(s) != errNone) {
        MemSet(s->playbackP, sizeof(Playback), 0);
        return "Wi-Fi unavailable.";
    }
    s->feedP = StreamFeedNew(&s->netConfig, &s->client, &s->playerP->video,
        &s->playerP->audio);
    if (s->feedP == 0) {
        MemSet(s->playbackP, sizeof(Playback), 0);
        return "Out of memory.";
    }
    return PlaybackOpenStream(s->playbackP, s->feedP);
}

static void OpenPlayForm(AppState *s)
{
    FormType *formP = FrmGetActiveForm();
    RectangleType bounds, formBounds;
    const Char *nameP = s->playPath, *p, *errorP;
    UInt16 length;
    FontID font;
    for (p = s->playPath; *p != '\0'; p++)
        if (*p == '/') nameP = p + 1;
    if (s->playPath[0] == '\0')
        nameP = s->playerP->title[0] != '\0' ? s->playerP->title
            : s->searchP->items[s->selected].title;
    length = StrLen(nameP);
    if (s->playPath[0] != '\0' && length > 4) length -= 4;    /* ".mp4" */
    if (length >= sizeof(s->playTitle)) length = sizeof(s->playTitle) - 1;
    MemMove(s->playTitle, nameP, length);
    s->playTitle[length] = '\0';
    font = FntSetFont(boldFont);
    if (FntCharsWidth(s->playTitle, (Int16)length) > PLAY_TITLE_WIDTH) {
        while (length > 0 && FntCharsWidth(s->playTitle, (Int16)length) +
                FntCharWidth(chrEllipsis) > PLAY_TITLE_WIDTH)
            length--;
        s->playTitle[length++] = chrEllipsis;
        s->playTitle[length] = '\0';
    }
    FntSetFont(font);
    FrmSetTitle(formP, s->playTitle);
    StrCopy(s->playStatus, s->playPath[0] != '\0' ? "Loading..." : "Connecting...");
    FldSetTextPtr((FieldType *)GetObject(PlayStatusField), s->playStatus);
    s->seekDragging = false;
    s->volumeDragging = false;
    s->skipPending = false;
    FrmDrawForm(formP);
    ClearVideoArea();
    DrawVolume(s);

    errorP = StartPlayback(s);
    if (errorP == 0) {
        PlaybackSetVolume(s->playbackP, kVolumeGain[s->prefs.volume]);
        GetVideoBounds(&bounds);
        WinGetBounds(FrmGetWindowHandle(formP), &formBounds);
        bounds.topLeft.x += formBounds.topLeft.x;
        bounds.topLeft.y += formBounds.topLeft.y;
        errorP = PlaybackAttachScreen(s->playbackP, &bounds);
    }
    if (errorP != 0) {
        ClosePlayback(s);
        StrCopy(s->playStatus, errorP);
        SetFieldText(PlayStatusField, s->playStatus);
        ShowObject(PlayPauseButton, false);
        return;
    }
    PlaybackPlay(s->playbackP);
    s->playWait = 0;
    s->playStatusTicks = TimGetTicks();
    DrawSeekBar(s);
}

static void TogglePause(AppState *s)
{
    Playback *p = s->playbackP;
    if (p == 0) return;
    if (p->playing) {
        PlaybackPause(p);
    } else {
        if (p->ended) ClearVideoArea();
        PlaybackPlay(p);
    }
    CtlSetLabel((ControlType *)GetObject(PlayPauseButton),
        p->playing ? "Pause" : "Play");
    s->playWait = 0;
    UpdatePlayStatus(s);
    DrawSeekBar(s);
}

/* After a seek: an ended video plays again from there. */
static void FinishSeek(AppState *s, Boolean wasEnded)
{
    Playback *p = s->playbackP;
    if (wasEnded) PlaybackPlay(p);
    CtlSetLabel((ControlType *)GetObject(PlayPauseButton),
        p->playing ? "Pause" : "Play");
    s->playWait = 0;
    UpdatePlayStatus(s);
    DrawSeekBar(s);
}

/* Moves the target; StepPlayback seeks once the presses stop. */
static void Skip(AppState *s, Boolean forward)
{
    Playback *p = s->playbackP;
    UInt32 now = TimGetTicks(), from, duration = PlaybackDurationMs(p);
    /* One press may arrive as two events. */
    if (now - s->lastSkip < SysTicksPerSecond() / 4 || !PlaybackCanSeek(p))
        return;
    s->lastSkip = now;
    from = s->skipPending ? s->seekTargetMs : PlaybackPositionMs(p);
    s->seekTargetMs = forward ? (from + SKIP_MS < duration ? from + SKIP_MS : duration)
        : from > SKIP_MS ? from - SKIP_MS : 0;
    s->skipPending = true;
    UpdatePlayStatus(s);
    DrawSeekBar(s);
}

static void FinishSkip(AppState *s)
{
    Playback *p = s->playbackP;
    UInt32 position = PlaybackPositionMs(p);
    Boolean wasEnded = p->ended, forward = s->seekTargetMs > position;
    s->skipPending = false;
    PlaybackSkip(p, forward, forward ? s->seekTargetMs - position
        : position - s->seekTargetMs);
    FinishSeek(s, wasEnded);
}

/* The pen on the seek bar: dragging shows where it points, lifting it
 * seeks there. Returns true if the event was the bar's. */
static Boolean TrackSeekBar(AppState *s, const EventType *eventP)
{
    RectangleType bounds;
    Boolean wasEnded;
    if (eventP->eType == penDownEvent) {
        GetSeekBounds(&bounds);
        /* The bar is thin: take the gap above and below it too. */
        bounds.topLeft.y -= 1;
        bounds.extent.y += 2;
        if (!RctPtInRectangle(eventP->screenX, eventP->screenY, &bounds) ||
            !PlaybackCanSeek(s->playbackP))
            return false;
        s->seekDragging = true;
    } else if (!s->seekDragging) {
        return false;
    }
    s->seekTargetMs = SeekBarMs(s, eventP->screenX);
    if (eventP->eType == penUpEvent) {
        s->seekDragging = false;
        wasEnded = s->playbackP->ended;
        PlaybackSeek(s->playbackP, s->seekTargetMs);
        FinishSeek(s, wasEnded);
        return true;
    }
    UpdatePlayStatus(s);
    DrawSeekBar(s);
    return true;
}

static void ReturnFromPlay(AppState *s)
{
    ClosePlayback(s);
    FrmReturnToForm(0);
    FrmDrawForm(FrmGetActiveForm());
    if (OnForm(DetailForm)) RefreshDetail(s);
}

/* Runs playback after each event while PlayForm is up. */
static void StepPlayback(AppState *s)
{
    Playback *p = s->playbackP;
    Boolean wasPlaying;
    UInt32 now;
    if (p == 0) return;
    /* Another form is on top; winEnterEvent wakes playback up again. */
    if (!OnForm(PlayForm)) {
        s->playWait = evtWaitForever;
        return;
    }
    wasPlaying = p->playing;
    s->playWait = PlaybackStep(p);
    now = TimGetTicks();
    if (s->skipPending) {
        UInt32 waited = now - s->lastSkip;
        if (waited >= SKIP_DELAY_TICKS) {
            FinishSkip(s);
        } else if (s->playWait < 0 ||
                   (UInt32)s->playWait > SKIP_DELAY_TICKS - waited) {
            s->playWait = (Int32)(SKIP_DELAY_TICKS - waited);
        }
    }
    if (wasPlaying && p->ended) {
        CtlSetLabel((ControlType *)GetObject(PlayPauseButton), "Replay");
        UpdatePlayStatus(s);
        DrawSeekBar(s);
    } else if ((p->playing || (s->feedP != 0 && !p->ended)) &&
               now - s->playStatusTicks >= SysTicksPerSecond()) {
        /* Paused streams keep downloading: the bar shows it. */
        s->playStatusTicks = now;
        if (s->feedP != 0) {
            const Char *tagP = !p->playing ? "pause"
                : p->buffering || !p->started ? "wait" : "play";
            UdpLogLine(s->logP, tagP, "at % ms, buffer % + % ms",
                PlaybackPositionMs(p), PlaybackBufferedMs(p, STREAM_FEED_VIDEO),
                PlaybackBufferedMs(p, STREAM_FEED_AUDIO));
            UdpLogLine(s->logP, tagP, "net % KB/s, cpu %, fps x10 %",
                p->netKBps, p->cpuPercent, p->fps10);
            UdpLogLine(s->logP, tagP, "level %, late %, peak net % ms",
                p->level, p->lateTotal,
                p->peakNet * 1000UL / SysTicksPerSecond());
        }
        if (p->playing) EvtResetAutoOffTimer();
        if (!s->seekDragging && !s->skipPending) {
            UpdatePlayStatus(s);
            DrawSeekBar(s);
        }
    }
}

static Boolean IsPlayWindow(WinHandle windowH)
{
    FormType *formP = FrmGetFormPtr(PlayForm);
    return formP != 0 && windowH == FrmGetWindowHandle(formP);
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
        StartThumb(s);
    } else if (task == taskDownload) {
        FinishChunk(s);
        RefreshDetail(s);
    } else if (task == taskThumb) {
        s->task = taskIdle;
        FinishThumb(s);
        DrawThumb(s);
    }
}

/* Runs job steps for a short slice so transfers are not limited to one
 * step per event. */
static void AdvanceJob(AppState *s)
{
    UInt32 start = TimGetTicks();
    while (s->jobP != 0) {
        if (NetJobStep(&s->jobP) == netJobFinished) {
            FinishJob(s);
            return;
        }
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
    FreeThumb(s);
    s->task = taskIdle;
    FrmReturnToForm(MainForm);
    FrmDrawForm(FrmGetActiveForm());
    ShowResults(s, true);
}

/* A 5-way button pressed just now, as palmOne reports it */
static Boolean NavPressed(const EventType *eventP, UInt16 bit, UInt16 change)
{
    return eventP->data.keyDown.chr == vchrNavChange &&
        (eventP->data.keyDown.keyCode & (bit | change)) == (bit | change);
}

static Boolean HandleEvent(EventType *eventP, AppState *s)
{
    FormType *formP;
    switch (eventP->eType) {
        case nilEvent:
            AdvanceJob(s);
            return false;
        case frmLoadEvent:
            formP = FrmInitForm(eventP->data.frmLoad.formID);
            FrmSetActiveForm(formP);
            return true;
        case frmOpenEvent:
            if (OnForm(MainForm)) {
                InitMainForm(s);
                FrmDrawForm(FrmGetActiveForm());
            } else if (OnForm(SavedForm)) {
                OpenSavedForm(s);
                FrmDrawForm(FrmGetActiveForm());
            } else if (OnForm(PlayForm)) {
                OpenPlayForm(s);
            } else if (OnForm(DetailForm)) {
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
            return true;
        case lstSelectEvent:
            if (eventP->data.lstSelect.listID == SavedList &&
                eventP->data.lstSelect.selection >= 0 &&
                (UInt16)eventP->data.lstSelect.selection < s->savedCount) {
                StoreMakePath(s->playPath, s->savedNamesP +
                    (UInt32)eventP->data.lstSelect.selection * SAVED_NAME_CAPACITY);
                LstSetSelection((ListType *)GetObject(SavedList), noListSelection);
                FrmPopupForm(PlayForm);
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
                    if (s->task == taskIdle) FrmPopupForm(SavedForm);
                    return true;
                case SavedBackButton: ReturnFromSaved(s); return true;
                /* With no history, nothing pops up. */
                case HistoryTrigger: return ShowHistory(s) == 0;
                case PlayButton:
                    StopThumb(s);
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
                    TogglePause(s);
                    return true;
                }
            }
            break;
        case winExitEvent:
            /* Something is about to cover the video: pause under it. */
            if (s->playbackP != 0 && s->playbackP->playing &&
                IsPlayWindow(eventP->data.winExit.exitWindow)) {
                PlaybackPause(s->playbackP);
                s->resumeOnEnter = true;
            }
            break;
        case winEnterEvent:
            if (s->playbackP != 0 && s->resumeOnEnter &&
                IsPlayWindow(eventP->data.winEnter.enterWindow)) {
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
            if (OnForm(PlayForm)) {
                WChar c = eventP->data.keyDown.chr;
                if (c == vchrPageUp || c == vchrRockerUp) {
                    if (s->prefs.volume < VOLUME_LEVELS - 1)
                        SetVolume(s, s->prefs.volume + 1);
                    return true;
                }
                if (c == vchrPageDown || c == vchrRockerDown) {
                    if (s->prefs.volume > 0) SetVolume(s, s->prefs.volume - 1);
                    return true;
                }
                if (c == vchrRockerLeft || NavPressed(eventP, navBitLeft, navChangeLeft)) {
                    Skip(s, false);
                    return true;
                }
                if (c == vchrRockerRight ||
                        NavPressed(eventP, navBitRight, navChangeRight)) {
                    Skip(s, true);
                    return true;
                }
                /* One press may arrive as both of these. */
                if (c == vchrRockerCenter ||
                        NavPressed(eventP, navBitSelect, navChangeSelect)) {
                    UInt32 now = TimGetTicks();
                    if (now - s->lastCentre >= SysTicksPerSecond() / 4)
                        TogglePause(s);
                    s->lastCentre = now;
                    return true;
                }
            }
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
    YtSearchInit(s->searchP);
    s->resultFocus = NO_RESULT;
    YtPlayerInit(s->playerP, kVideoItags, kAudioItags);
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
    return errNone;
}

static void StopApplication(AppState *s)
{
    ClosePlayback(s);
    if (s->prefsChanged)
        PrefSetAppPreferences(APP_CREATOR, PREFS_ID, PREFS_VERSION, &s->prefs,
            sizeof(s->prefs), true);
    if (s->jobP != 0) {
        NetJobCancel(s->jobP);
        while (s->jobP != 0) NetJobStep(&s->jobP);
    }
    CloseFile(s);
    FreeThumb(s);
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
