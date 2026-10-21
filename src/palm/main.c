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
#include "yt_api.h"

#define CHUNK_BYTES (4UL * 1024UL * 1024UL)
#define ARM_TIMEOUT_SECONDS 30
#define M68K_TIMEOUT_SECONDS 600
#define STEP_SLICE_TICKS 10
#define PROGRESS_INTERVAL_TICKS 50
#define CLIENT_FILE STORE_DIR "/client.txt"
#define CLIENT_FILE_CAPACITY 1024
#define PREFS_ID 0
#define PREFS_VERSION 2
#define MAX_SAVED 32
#define SAVED_NAME_CAPACITY 64
/* Redirects in a row a download follows. */
#define MAX_REDIRECTS 5
/* palmOne's 5-way reports the centre button this way (PalmChars.h). */
#define vchrNavChange 0x0503
#define navBitSelect 0x0010
#define navChangeSelect 0x1000
/* Level 0 is silent. */
#define VOLUME_LEVELS 11
#define VOLUME_DEFAULT 6

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
    taskDownload
} Task;

typedef struct AppPrefs {
    Char visitor[YT_VISITOR_CAPACITY];
    UInt16 volume;              /* level, below VOLUME_LEVELS */
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

    JsonScanner scanner;
    Char jsonValue[YT_JSON_VALUE_CAPACITY];
    Char host[64];
    Char request[YT_REQUEST_CAPACITY];
    Char body[YT_BODY_CAPACITY];
    NetJob *jobP;
    NetJobResult jobResult;
    UInt16 task;
    UInt32 taskStartTicks;

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
    UInt16 playView;            /* tapping the status shows diagnostics */
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
    return errNone;
}

static void LoadSettings(AppState *s)
{
    UInt16 size = sizeof(s->prefs);
    Char *textP;
    UInt16 length = 0;
    MemSet(&s->prefs, sizeof(s->prefs), 0);
    if (PrefGetAppPreferences(APP_CREATOR, PREFS_ID, &s->prefs, &size, true) ==
            noPreferenceFound || size != sizeof(s->prefs)) {
        MemSet(&s->prefs, sizeof(s->prefs), 0);
        s->prefs.volume = VOLUME_DEFAULT;
    }
    s->prefs.visitor[sizeof(s->prefs.visitor) - 1] = '\0';
    if (s->prefs.volume >= VOLUME_LEVELS) s->prefs.volume = VOLUME_DEFAULT;
    YtClientDefaults(&s->client);
    /* A client.txt on the card overrides the client when YouTube changes
     * what it accepts; see README. */
    if (StoreOpenVolume(&s->volume) != errNone) return;
    textP = (Char *)MemPtrNew(CLIENT_FILE_CAPACITY);
    if (textP == 0) return;
    if (StoreReadText(s->volume, CLIENT_FILE, textP, CLIENT_FILE_CAPACITY,
            &length) == errNone)
        YtClientParse(&s->client, textP, length);
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

static void DrawResultRow(Int16 itemNum, RectangleType *boundsP,
                          Char **itemsText)
{
    /* The list's choices pointer carries the YtSearch results. */
    const YtSearch *searchP = (const YtSearch *)itemsText;
    const YtSearchItem *itemP = &searchP->items[itemNum];
    const Char *lengthP = itemP->live ? "live" : itemP->length;
    UInt16 lengthLength = StrLen(lengthP);
    Coord lengthWidth = FntCharsWidth(lengthP, lengthLength);
    Coord titleWidth = boundsP->extent.x - lengthWidth - 6;
    WinDrawTruncChars(itemP->title, StrLen(itemP->title),
        boundsP->topLeft.x + 1, boundsP->topLeft.y, titleWidth);
    WinDrawChars(lengthP, lengthLength,
        boundsP->topLeft.x + boundsP->extent.x - lengthWidth - 1,
        boundsP->topLeft.y);
}

static void ShowResults(AppState *s, Boolean draw)
{
    ListType *listP = (ListType *)GetObject(ResultList);
    LstSetDrawFunction(listP, DrawResultRow);
    LstSetListChoices(listP, (Char **)s->searchP, (Int16)s->searchP->count);
    LstSetSelection(listP, noListSelection);
    if (draw) LstDrawList(listP);
}

static void InitMainForm(AppState *s)
{
    FieldType *fieldP = (FieldType *)GetObject(SearchField);
    FldSetTextHandle(fieldP, s->searchTextH);
    ShowResults(s, false);
    FldSetTextPtr((FieldType *)GetObject(StatusField), s->status);
    FrmSetFocus(FrmGetActiveForm(), FrmGetObjectIndex(FrmGetActiveForm(),
        SearchField));
}

static void StartSearch(AppState *s)
{
    FieldType *fieldP = (FieldType *)GetObject(SearchField);
    const Char *queryP = FldGetTextPtr(fieldP);
    if (s->task != taskIdle || queryP == 0 || queryP[0] == '\0') return;
    YtSearchInit(s->searchP);
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

static void BuildInfo(AppState *s)
{
    const YtSearchItem *itemP = &s->searchP->items[s->selected];
    const YtPlayer *playerP = s->playerP;
    Char *infoP = s->info;
    UInt16 capacity = sizeof(s->info);
    infoP[0] = '\0';
    Append(infoP, capacity, playerP->title[0] != '\0'
        ? playerP->title : itemP->title);
    Append(infoP, capacity, "\n");
    Append(infoP, capacity, playerP->author[0] != '\0'
        ? playerP->author : itemP->channel);
    Append(infoP, capacity, "\n");
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

static void RefreshDetail(AppState *s)
{
    if (!OnForm(DetailForm)) return;
    SetFieldText(InfoField, s->info);
    UpdateInfoScrollBar();
    SetFieldText(ProgressField, s->progress);
    ShowObject(DownloadButton, s->task == taskIdle &&
        YtPlayerPlayable(s->playerP));
    ShowObject(CancelButton, s->task != taskIdle);
    ShowObject(PlayButton, s->task == taskIdle &&
        (s->saved || YtPlayerPlayable(s->playerP)));
    DrawProgressBar(s);
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
            if (s->saved) {
                StrCopy(s->progress, "Already saved. Tap Play to watch it.");
            } else {
                StrCopy(s->progress, "Ready in ");
                AppendSeconds(s->progress, sizeof(s->progress),
                    TimGetTicks() - s->taskStartTicks);
                Append(s->progress, sizeof(s->progress),
                    ". Tap Play to stream it, or Download to save it.");
            }
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
        s->request, sizeof(s->request));
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
    Append(s->progress, sizeof(s->progress), ". Tap Play to watch it.");
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

static void SetVolume(AppState *s, UInt16 level)
{
    if (level == s->prefs.volume) return;
    s->prefs.volume = level;
    s->prefsChanged = true;
    if (s->playbackP != 0)
        PlaybackSetVolume(s->playbackP, kVolumeGain[level]);
}

static void AppendSigned(Char *dstP, UInt16 capacity, Int32 value)
{
    Append(dstP, capacity, value < 0 ? "-" : "+");
    AppendNumber(dstP, capacity, (UInt32)(value < 0 ? -value : value));
}

/* Tap the status to see these: how the sound stream and the clock behave
 * on this device. */
static void BuildPlayDetails(const Playback *p, Char *textP, UInt16 capacity)
{
    Append(textP, capacity, "Sound ");
    AppendNumber(textP, capacity, p->soundLastRequest);
    Append(textP, capacity, "/");
    AppendNumber(textP, capacity, p->latencyFrames);
    Append(textP, capacity, ", ring ");
    AppendNumber(textP, capacity, p->sampleRate != 0
        ? p->soundBuffered * 1000UL / p->sampleRate : 0);
    Append(textP, capacity, " ms\nSync ");
    AppendSigned(textP, capacity, p->diffMin);
    Append(textP, capacity, "..");
    AppendSigned(textP, capacity, p->diffMax);
    Append(textP, capacity, " ms, jumps ");
    AppendNumber(textP, capacity, p->snaps);
    Append(textP, capacity, "\nLoops ");
    AppendNumber(textP, capacity, p->loopsPerSecond);
    Append(textP, capacity, "/s, late ");
    AppendNumber(textP, capacity, p->lateTotal);
    Append(textP, capacity, ", levels ");
    AppendNumber(textP, capacity, p->levelChanges);
}

static void AppendBuffered(Char *dstP, UInt16 capacity, UInt32 ms)
{
    if (ms == 0xFFFFFFFFUL) {
        Append(dstP, capacity, "all");
        return;
    }
    AppendNumber(dstP, capacity, ms / 1000UL);
    Append(dstP, capacity, " s");
}

static void AppendTicks(Char *dstP, UInt16 capacity, UInt32 ticks)
{
    AppendNumber(dstP, capacity, ticks * 1000UL / SysTicksPerSecond());
}

/* The second tap while streaming: the network, the buffers, and the
 * longest stalls since playback started. */
static void BuildStreamDetails(const AppState *s, Char *textP, UInt16 capacity)
{
    const Playback *p = s->playbackP;
    Append(textP, capacity, "Net ");
    AppendNumber(textP, capacity, p->netKBps);
    Append(textP, capacity, " KB/s, CPU ");
    AppendNumber(textP, capacity, p->netPercent);
    Append(textP, capacity, "%, retries ");
    AppendNumber(textP, capacity, StreamFeedRetries(s->feedP));
    Append(textP, capacity, "\nBuffer ");
    AppendBuffered(textP, capacity, PlaybackBufferedMs(p, STREAM_FEED_VIDEO));
    Append(textP, capacity, " + ");
    AppendBuffered(textP, capacity, PlaybackBufferedMs(p, STREAM_FEED_AUDIO));
    Append(textP, capacity, ", waits ");
    AppendNumber(textP, capacity, p->bufferings);
    Append(textP, capacity, "\nMax ms: net ");
    AppendTicks(textP, capacity, p->peakNet);
    Append(textP, capacity, ", dec ");
    AppendTicks(textP, capacity, p->peakDecode);
    Append(textP, capacity, ", away ");
    AppendTicks(textP, capacity, p->peakAway);
}

static void UpdatePlayStatus(AppState *s)
{
    static const Char kQuality[PLAYBACK_LEVELS][12] = {
        "full", "fast B", "fast", "half rate"
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
    if (s->playView != 0) {
        if (s->playView == 1) BuildPlayDetails(p, textP, capacity);
        else BuildStreamDetails(s, textP, capacity);
        SetFieldText(PlayStatusField, textP);
        return;
    }
    AppendClock(textP, capacity, PlaybackPositionMs(p));
    Append(textP, capacity, " / ");
    AppendClock(textP, capacity, PlaybackDurationMs(p));
    if (p->ended) Append(textP, capacity, "  Ended");
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
        Append(textP, capacity, "\nQuality: ");
        Append(textP, capacity, kQuality[p->level]);
        Append(textP, capacity, ", sound gaps ");
        AppendNumber(textP, capacity, p->soundUnderruns);
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
    StrCopy(s->playStatus, s->playPath[0] != '\0' ? "Loading..." : "Connecting...");
    s->playView = 0;
    FldSetTextPtr((FieldType *)GetObject(PlayTitleField), s->playTitle);
    FldSetTextPtr((FieldType *)GetObject(PlayStatusField), s->playStatus);
    FrmDrawForm(formP);
    ClearVideoArea();

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
    if (wasPlaying && p->ended) {
        CtlSetLabel((ControlType *)GetObject(PlayPauseButton), "Replay");
        UpdatePlayStatus(s);
    } else if (p->playing && now - s->playStatusTicks >= SysTicksPerSecond()) {
        s->playStatusTicks = now;
        EvtResetAutoOffTimer();
        UpdatePlayStatus(s);
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
    } else if (task == taskDownload) {
        FinishChunk(s);
        RefreshDetail(s);
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
    s->task = taskIdle;
    FrmReturnToForm(MainForm);
    FrmDrawForm(FrmGetActiveForm());
    ShowResults(s, true);
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
            if (OnForm(PlayForm)) ClearVideoArea();
            return true;
        case lstSelectEvent:
            if (eventP->data.lstSelect.listID == ResultList &&
                s->task == taskIdle && eventP->data.lstSelect.selection >= 0) {
                s->selected = (UInt16)eventP->data.lstSelect.selection;
                if (s->searchP->items[s->selected].live) {
                    SetStatus(s, "Live streams are not supported.");
                    LstSetSelection((ListType *)GetObject(ResultList),
                        noListSelection);
                } else {
                    FrmPopupForm(DetailForm);
                }
                return true;
            }
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
                case PlayButton:
                    if (s->saved) StrCopy(s->playPath, s->outPath);
                    else s->playPath[0] = '\0';
                    FrmPopupForm(PlayForm);
                    return true;
                case PlayPauseButton: TogglePause(s); return true;
                case PlayDoneButton: ReturnFromPlay(s); return true;
                default: break;
            }
            break;
        case penDownEvent:
            if (OnForm(PlayForm) && s->playbackP != 0) {
                FormType *formP = FrmGetActiveForm();
                RectangleType bounds;
                GetVideoBounds(&bounds);
                if (RctPtInRectangle(eventP->screenX, eventP->screenY, &bounds)) {
                    TogglePause(s);
                    return true;
                }
                FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, PlayStatusField),
                    &bounds);
                if (RctPtInRectangle(eventP->screenX, eventP->screenY, &bounds)) {
                    s->playView = (UInt16)((s->playView + 1) %
                        (s->feedP != 0 ? 3 : 2));
                    UpdatePlayStatus(s);
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
            if (OnForm(MainForm) && (eventP->data.keyDown.chr == vchrPageUp ||
                    eventP->data.keyDown.chr == vchrPageDown)) {
                LstScrollList((ListType *)GetObject(ResultList),
                    eventP->data.keyDown.chr == vchrPageUp ? winUp : winDown, 9);
                return true;
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
                /* One press may arrive as both of these. */
                if (c == vchrRockerCenter || (c == vchrNavChange &&
                        (eventP->data.keyDown.keyCode & (navBitSelect |
                            navChangeSelect)) == (navBitSelect | navChangeSelect))) {
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
        } while (event.eType != appStopEvent);
    }
    StopApplication(s);
    MemPtrFree(s);
    return errNone;
}
