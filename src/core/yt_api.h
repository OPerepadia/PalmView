/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_YT_API_H
#define PALMVIEW_YT_API_H

#include <PalmOS.h>
#include "json_scan.h"

/* YouTube InnerTube API: request building and response parsing.
 * Feed response bodies through a JsonScanner using the Yt*Value/Yt*End
 * callbacks with the matching result struct as context. */

#define YT_API_HOST "www.youtube.com"
#define YT_THUMB_HOST "i.ytimg.com"
#define YT_VIDEO_ID_CAPACITY 12
#define YT_URL_CAPACITY 1536
#define YT_VISITOR_CAPACITY 512
#define YT_TITLE_CAPACITY 96
#define YT_AUTHOR_CAPACITY 40
#define YT_REASON_CAPACITY 96
#define YT_STATUS_CAPACITY 24
#define YT_BODY_CAPACITY 1536
#define YT_REQUEST_CAPACITY 2560
#define YT_SEARCH_MAX 20
#define YT_JSON_VALUE_CAPACITY YT_URL_CAPACITY
#define YT_MAX_ITAG_CHOICES 4

/* The client YouTube sees. Defaults are the visionOS app, which currently
 * returns direct stream URLs without a JavaScript challenge or PO token.
 * When YouTube changes that, override these from a settings file. */
typedef struct YtClient {
    Char name[24];
    Char version[24];
    Char nameId[8];
    Char userAgent[160];
    Char deviceMake[16];
    Char deviceModel[32];
    Char osName[16];
    Char osVersion[24];
} YtClient;

void YtClientDefaults(YtClient *clientP);
/* Applies "key=value" lines (name, version, nameId, userAgent, deviceMake,
 * deviceModel, osName, osVersion). Unknown keys and '#' lines are ignored. */
void YtClientParse(YtClient *clientP, const Char *textP, UInt16 length);

/* Builds a complete HTTP/1.1 request. Returns its length, or 0 if it does
 * not fit. bodyP is scratch space of YT_BODY_CAPACITY bytes. queryP is
 * Palm-encoded text. */
UInt16 YtBuildPlayerRequest(const YtClient *clientP, const Char *visitorP,
                            const Char *videoIdP, Char *bodyP,
                            Char *requestP, UInt16 capacity);
UInt16 YtBuildSearchRequest(const YtClient *clientP, const Char *visitorP,
                            const Char *queryP, Char *bodyP,
                            Char *requestP, UInt16 capacity);

/* Splits an https URL into host and the start of its path. */
Boolean YtSplitUrl(const Char *urlP, Char *hostP, UInt16 hostCapacity,
                   const Char **pathPP);
/* GET for bytes [start, end] of a stream URL, using its range parameter.
 * With keepAlive the server may leave the connection open for the next
 * request. */
UInt16 YtBuildMediaRequest(const YtClient *clientP, const Char *urlP,
                           UInt32 start, UInt32 end, Boolean keepAlive,
                           Char *requestP, UInt16 capacity);
/* GET for a video's smallest thumbnail, default.jpg, from YT_THUMB_HOST. */
UInt16 YtBuildThumbRequest(const YtClient *clientP, const Char *videoIdP,
                           Char *requestP, UInt16 capacity);
/* Replaces a stream URL with where a redirect sent it, without the range
 * parameter the redirect carries. False, leaving urlP alone, if locationP
 * is not https or does not fit. */
Boolean YtApplyRedirect(Char *urlP, UInt16 capacity, const Char *locationP);

typedef struct YtFormat {
    UInt16 itag;
    UInt32 contentLength;
    Boolean found;
    Char url[YT_URL_CAPACITY];
} YtFormat;

typedef struct YtPlayer {
    Char status[YT_STATUS_CAPACITY];
    Char reason[YT_REASON_CAPACITY];
    Char title[YT_TITLE_CAPACITY];
    Char author[YT_AUTHOR_CAPACITY];
    Char visitor[YT_VISITOR_CAPACITY];
    UInt32 lengthSeconds;
    Boolean live;
    UInt16 videoItags[YT_MAX_ITAG_CHOICES];
    UInt16 audioItags[YT_MAX_ITAG_CHOICES];
    UInt16 videoRank;
    UInt16 audioRank;
    YtFormat video;
    YtFormat audio;
    /* The adaptive format being read. */
    UInt16 curItag;
    UInt32 curLength;
    Boolean curHasUrl;
    Boolean curUnusable;
    Char curUrl[YT_URL_CAPACITY];
} YtPlayer;

/* Itag lists are in order of preference and end with 0. */
void YtPlayerInit(YtPlayer *playerP, const UInt16 *videoItagsP,
                  const UInt16 *audioItagsP);
Boolean YtPlayerValue(void *contextP, const JsonScanner *scannerP,
                      UInt16 type, const Char *textP, UInt16 length);
Boolean YtPlayerEnd(void *contextP, const JsonScanner *scannerP);
Boolean YtPlayerPlayable(const YtPlayer *playerP);

typedef struct YtSearchItem {
    Char videoId[YT_VIDEO_ID_CAPACITY];
    Char title[80];
    Char channel[32];
    Char length[10];
    Char published[20];
    Char views[20];
    Boolean live;
} YtSearchItem;

typedef struct YtSearch {
    UInt16 count;
    Char visitor[YT_VISITOR_CAPACITY];
    YtSearchItem cur;
    YtSearchItem items[YT_SEARCH_MAX];
} YtSearch;

void YtSearchInit(YtSearch *searchP);
Boolean YtSearchValue(void *contextP, const JsonScanner *scannerP,
                      UInt16 type, const Char *textP, UInt16 length);
Boolean YtSearchEnd(void *contextP, const JsonScanner *scannerP);

#endif
