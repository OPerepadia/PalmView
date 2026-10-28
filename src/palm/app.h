/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The state PalmView's screens share, and the helpers in main.c they use. */

#ifndef PALMVIEW_APP_H
#define PALMVIEW_APP_H

#include <PalmOS.h>

#include "json_scan.h"
#include "net_job.h"
#include "playback.h"
#include "store.h"
#include "stream_feed.h"
#include "text.h"
#include "udp_log.h"
#include "yt_api.h"

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
/* Level 0 is silent. */
#define VOLUME_LEVELS 11
#define VOLUME_DEFAULT 6

/* Video qualities, in the order of their itags */
enum { quality144 = 0, quality240 };
static const Char kQualityNames[YT_VIDEO_CHOICES][5] = { "144p", "240p" };

typedef enum Task {
    taskIdle = 0,
    taskSearch,
    taskPlayer,
    taskDownload,
    taskThumbs          /* in the background: anything else stops it */
} Task;

/* A search result's thumbnail */
typedef struct Thumb {
    UInt8 *jpegP;               /* once downloaded */
    UInt16 length;
    UInt8 state;
    BitmapType *bitsP;          /* the list's pixels */
    BitmapType *bitmapP;        /* the same, drawn at double density */
} Thumb;

enum { thumbMissing = 0, thumbLoaded, thumbFailed };

typedef struct AppPrefs {
    Char visitor[YT_VISITOR_CAPACITY];
    UInt16 volume;              /* level, below VOLUME_LEVELS */
    Char history[HISTORY_MAX][HISTORY_CAPACITY];    /* "" when unused */
    UInt16 quality;             /* the one last chosen in the player */
} AppPrefs;

typedef struct AppState {
    UInt16 tlsRefNum;
    Boolean tlsLoaded;
    Boolean tlsOpen;
    UInt32 tlsCapabilities;
    UInt16 netRefNum;
    Boolean netOpen;
    MemHandle trustH;
    TextCodec codec;            /* how the Palm shows text */
    MemHandle jisH;             /* its Japanese table, with CJKOS */
    NetJobConfig netConfig;

    YtClient client;
    AppPrefs prefs;
    YtSearch *searchP;
    YtPlayer *playerP;
    UInt16 selected;
    UInt16 resultTop;           /* result in the first row */
    UInt16 resultRows;          /* rows on screen */
    Coord resultRowHeight;
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
    Thumb thumbs[YT_SEARCH_MAX];
    UInt8 *thumbBufferP;        /* while thumbnails download */
    UInt16 thumbLength;
    UInt16 thumbItem;           /* the one downloading, or NO_RESULT */
    BitmapType *detailBitsP;    /* the selected video's, full size */
    BitmapType *detailThumbP;

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
    UInt32 lastInputTicks;      /* of the last pen, key or scroll event */
    Boolean seekDragging;       /* the pen is on the seek bar */
    Boolean volumeDragging;     /* or on the volume bars */
    Boolean skipPending;        /* 5-way presses not applied yet */
    UInt32 seekTargetMs;        /* where either points */
    Char *historyItems[HISTORY_MAX];    /* the history list's choices */
    Char logDest[24];           /* "a.b.c.d:port" from client.txt, or "" */
    UdpLog *logP;               /* 0 without a log= line */
    Char playPath[STORE_PATH_CAPACITY];     /* empty to stream */
    UInt16 playChoice;          /* the quality streaming */
    Boolean fullScreen;         /* FullForm is up, the picture turned */
    UInt16 inputArea;           /* its state before full screen */
    Char playTitle[64];
    Char playStatus[128];
} AppState;

void *GetObject(UInt16 id);
Boolean OnForm(UInt16 formId);
void Append(Char *dstP, UInt16 capacity, const Char *textP);
void AppendNumber(Char *dstP, UInt16 capacity, UInt32 value);
void SetFieldText(UInt16 fieldId, Char *textP);
void ShowObject(UInt16 id, Boolean visible);
void DescribeResult(const NetJobResult *r, Boolean toCard, Char *dstP,
                    UInt16 capacity);
Boolean NetworkOpen(AppState *s);
Err EnsureNetwork(AppState *s);
UInt32 ThumbBytes(const AppState *s);
void StartThumbs(AppState *s);
Boolean NavPressed(const EventType *eventP, UInt16 bit, UInt16 change);
Boolean HasInputArea(void);
void SetObjectY(FormType *formP, UInt16 index, Coord y, Coord height);
UInt16 VideoChoice(const AppState *s);
void BuildInfo(AppState *s);
void RefreshDetail(AppState *s);
void LayoutDetailForm(void);
void LayoutSavedForm(void);

#endif
