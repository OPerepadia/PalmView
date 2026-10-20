/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "stream_feed.h"

/* Bytes per request, as the downloader asks for. */
#define CHUNK_BYTES (4UL * 1024UL * 1024UL)
/* Failed connections in a row before giving up; the wait before trying
 * again grows by RETRY_SECONDS each time. */
#define MAX_FAILURES 5
#define RETRY_SECONDS 1
/* Redirects in a row before giving up. */
#define MAX_REDIRECTS 5
/* Reads per stream in one StreamFeedStep, while data keeps coming. */
#define READS_PER_STEP 4

typedef struct FeedTrack {
    StreamFeed *feedP;
    YtFormat *formatP;
    Mp4Stream *streamP;
    NetJob *jobP;
    NetJobResult result;
    UInt32 offset;          /* next byte to ask for */
    UInt32 retryTicks;      /* when to connect again, if waiting */
    Boolean waiting;
    UInt16 failures;
    UInt16 redirects;
    Char host[64];
    Char request[YT_REQUEST_CAPACITY];
    Char location[YT_URL_CAPACITY];
} FeedTrack;

struct StreamFeed {
    NetJobConfig config;
    const YtClient *clientP;
    FeedTrack tracks[2];
    UInt32 received;
    UInt16 retries;
    const Char *errorP;
    Boolean failed;
    Boolean dropped;
    NetJobResult failure;
    NetJobResult drop;
};

StreamFeed *StreamFeedNew(const NetJobConfig *configP, const YtClient *clientP,
                          YtFormat *videoP, YtFormat *audioP)
{
    StreamFeed *f = (StreamFeed *)MemPtrNew(sizeof(StreamFeed));
    if (f == 0) return 0;
    MemSet(f, sizeof(*f), 0);
    f->config = *configP;
    f->config.poll = true;
    f->clientP = clientP;
    f->tracks[STREAM_FEED_VIDEO].formatP = videoP;
    f->tracks[STREAM_FEED_AUDIO].formatP = audioP;
    f->tracks[STREAM_FEED_VIDEO].feedP = f;
    f->tracks[STREAM_FEED_AUDIO].feedP = f;
    return f;
}

void StreamFeedFree(StreamFeed *f)
{
    if (f == 0) return;
    StreamFeedStop(f);
    MemPtrFree(f);
}

UInt32 StreamFeedLength(const StreamFeed *f, UInt16 track)
{
    return f->tracks[track].formatP->contentLength;
}

void StreamFeedStart(StreamFeed *f, Mp4Stream *videoP, Mp4Stream *audioP)
{
    UInt16 i;
    StreamFeedStop(f);
    f->errorP = 0;
    f->failed = false;
    f->dropped = false;
    for (i = 0; i < 2; i++) {
        FeedTrack *t = &f->tracks[i];
        t->streamP = i == STREAM_FEED_VIDEO ? videoP : audioP;
        t->offset = 0;
        t->waiting = false;
        t->failures = 0;
        t->redirects = 0;
    }
}

void StreamFeedStop(StreamFeed *f)
{
    UInt16 i;
    for (i = 0; i < 2; i++) {
        FeedTrack *t = &f->tracks[i];
        if (t->jobP == 0) continue;
        NetJobCancel(t->jobP);
        while (NetJobStep(&t->jobP) != netJobFinished) {}
    }
}

const Char *StreamFeedError(const StreamFeed *f)
{
    return f->errorP;
}

UInt32 StreamFeedReceived(const StreamFeed *f)
{
    return f->received;
}

UInt16 StreamFeedRetries(const StreamFeed *f)
{
    return f->retries;
}

const NetJobResult *StreamFeedFailure(const StreamFeed *f)
{
    return f->failed ? &f->failure : 0;
}

const NetJobResult *StreamFeedDrop(const StreamFeed *f)
{
    return f->dropped ? &f->drop : 0;
}

static Int16 Receive(void *contextP, const UInt8 *dataP, UInt16 length)
{
    FeedTrack *t = (FeedTrack *)contextP;
    if (length > Mp4StreamSpace(t->streamP)) return 1;
    Mp4StreamWrite(t->streamP, dataP, length);
    t->offset += length;
    t->feedP->received += length;
    return 0;
}

static void Request(StreamFeed *f, FeedTrack *t)
{
    UInt32 length = t->formatP->contentLength;
    UInt32 end = t->offset + CHUNK_BYTES - 1;
    const Char *pathP;
    UInt16 size;
    if (end >= length) end = length - 1;
    size = YtBuildMediaRequest(f->clientP, t->formatP->url, t->offset, end,
        t->request, sizeof(t->request));
    if (size == 0 || !YtSplitUrl(t->formatP->url, t->host, sizeof(t->host),
            &pathP)) {
        f->errorP = "The stream link is not usable.";
        return;
    }
    if (!NetJobStart(&t->jobP, &f->config, t->host, t->request, size, Receive,
            t, &t->result)) {
        f->errorP = "Out of memory.";
        return;
    }
    NetJobKeepLocation(t->jobP, t->location, sizeof(t->location));
}

static void Finished(StreamFeed *f, FeedTrack *t)
{
    UInt16 http = t->result.httpStatus;
    if (t->result.received != 0) {
        t->failures = 0;
        t->redirects = 0;
    }
    if (t->result.status == netJobOk && t->result.received != 0) return;
    f->failed = true;
    f->failure = t->result;
    /* YouTube moves some streams to another server. Ask there at once. */
    if (t->result.redirected) {
        if (++t->redirects > MAX_REDIRECTS ||
            !YtApplyRedirect(t->formatP->url, sizeof(t->formatP->url),
                t->location))
            f->errorP = "Could not follow YouTube's redirect.";
        return;
    }
    if (t->result.status == netJobHttpError && http >= 400 && http < 500) {
        f->errorP = http == 403
            ? "The stream link has expired. Go back and open the video again."
            : "YouTube refused the stream.";
        return;
    }
    if (t->failures == 0) {
        f->dropped = true;
        f->drop = t->result;
    }
    if (++t->failures > MAX_FAILURES) {
        f->errorP = "Lost the connection to YouTube.";
        return;
    }
    f->retries++;
    t->waiting = true;
    t->retryTicks = TimGetTicks() + (UInt32)t->failures * RETRY_SECONDS *
        SysTicksPerSecond();
}

static void StepTrack(StreamFeed *f, FeedTrack *t)
{
    UInt16 i;
    if (t->streamP == 0 || f->errorP != 0) return;
    if (t->jobP == 0) {
        if (t->offset >= t->formatP->contentLength) return;
        if (t->waiting && (Int32)(TimGetTicks() - t->retryTicks) < 0) return;
        t->waiting = false;
        Request(f, t);
        return;
    }
    for (i = 0; i < READS_PER_STEP; i++) {
        UInt32 before = t->offset;
        Boolean receiving = NetJobReceiving(t->jobP);
        if (receiving && Mp4StreamSpace(t->streamP) < NET_JOB_IO_CAPACITY) return;
        if (NetJobStep(&t->jobP) == netJobFinished) {
            Finished(f, t);
            return;
        }
        if (!receiving || t->offset == before) return;
    }
}

void StreamFeedStep(StreamFeed *f)
{
    StepTrack(f, &f->tracks[STREAM_FEED_VIDEO]);
    StepTrack(f, &f->tracks[STREAM_FEED_AUDIO]);
}
