/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "stream_feed.h"

/* Only one request is in flight at a time, and it asks for no more than
 * its ring has room for, so every byte that arrives can be read at once.
 * On the Palm TX, data waiting unread on one connection starves the
 * others: their data and even new handshakes stop getting through. Each
 * stream keeps its connection open between requests, because a new TLS
 * handshake stalls the player for about 2 s. */

/* The most one request asks for. */
#define REQUEST_BYTES (128UL * 1024UL)
/* A stream asks again once this share of its ring is free, or room for the
 * rest of it. The stream with the emptier ring goes first. */
#define REFILL_DIVISOR 4
/* Failed connections in a row before giving up; the wait before trying
 * again grows by RETRY_SECONDS each time. */
#define MAX_FAILURES 5
#define RETRY_SECONDS 1
/* Redirects in a row before giving up. */
#define MAX_REDIRECTS 5
/* Reads per stream in one StreamFeedStep, while data keeps coming. */
#define READS_PER_STEP 4
/* While the stream being fetched has at least half its ring buffered, its
 * response is read at no more than this, a little above both streams'
 * bitrate together. Each 16 KB TLS record is decrypted in one go, and a
 * frame due meanwhile is late: read faster, and too many frames are.
 * Below half, it is read at twice this while video plays, and as fast as
 * it comes while nothing does. */
#define PACE_BYTES_PER_SECOND (32UL * 1024UL)

/* A response that sends nothing this long is taken as stalled, and asked
 * for again on a new connection. */
#define IDLE_SECONDS 5

typedef struct FeedTrack {
    StreamFeed *feedP;
    const Char *tagP;       /* in the log */
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
    FeedTrack *activeP;     /* the track with a request in flight, or 0 */
    UInt32 allowance;       /* bytes a paced read may take now */
    UInt32 paceTicks;       /* when the allowance last grew */
    UInt32 received;
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
    f->config.idleTimeoutTicks = (Int32)IDLE_SECONDS * SysTicksPerSecond();
    f->config.keepAlive = true;
    f->clientP = clientP;
    f->tracks[STREAM_FEED_VIDEO].formatP = videoP;
    f->tracks[STREAM_FEED_AUDIO].formatP = audioP;
    f->tracks[STREAM_FEED_VIDEO].feedP = f;
    f->tracks[STREAM_FEED_AUDIO].feedP = f;
    f->tracks[STREAM_FEED_VIDEO].tagP = "v";
    f->tracks[STREAM_FEED_AUDIO].tagP = "a";
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

static void Close(FeedTrack *t)
{
    NetJobCancel(t->jobP);
    while (NetJobStep(&t->jobP) != netJobFinished) {}
}

void StreamFeedStop(StreamFeed *f)
{
    UInt16 i;
    for (i = 0; i < 2; i++) {
        FeedTrack *t = &f->tracks[i];
        if (t->jobP != 0) Close(t);
    }
    f->activeP = 0;
}

void StreamFeedSeek(StreamFeed *f, UInt32 videoOffset, UInt32 audioOffset)
{
    UInt16 i;
    if (f->activeP != 0 && f->activeP->jobP != 0) Close(f->activeP);
    f->activeP = 0;
    f->errorP = 0;
    for (i = 0; i < 2; i++) {
        FeedTrack *t = &f->tracks[i];
        t->offset = i == STREAM_FEED_VIDEO ? videoOffset : audioOffset;
        t->waiting = false;
        t->failures = 0;
        t->redirects = 0;
        UdpLogLine(f->config.logP, t->tagP, "seek to %", t->offset, 0, 0);
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
    UInt32 room = Mp4StreamSpace(t->streamP);
    UInt32 end;
    const Char *pathP;
    UInt16 size;
    if (room > REQUEST_BYTES) room = REQUEST_BYTES;
    end = t->offset + room - 1;
    if (end >= length) end = length - 1;
    size = YtBuildMediaRequest(f->clientP, t->formatP->url, t->offset, end,
        true, t->request, sizeof(t->request));
    if (size == 0 || !YtSplitUrl(t->formatP->url, t->host, sizeof(t->host),
            &pathP)) {
        f->errorP = "The stream link is not usable.";
        return;
    }
    UdpLogLine(f->config.logP, t->tagP, "get %-% of %", t->offset, end, length);
    f->activeP = t;
    if (t->jobP != 0 && NetJobSend(t->jobP, t->request, size)) return;
    if (t->jobP != 0) Close(t);
    if (!NetJobStart(&t->jobP, &f->config, t->host, t->request, size, Receive,
            t, &t->result)) {
        f->activeP = 0;
        f->errorP = "Out of memory.";
        return;
    }
    NetJobKeepLocation(t->jobP, t->location, sizeof(t->location));
    NetJobSetTag(t->jobP, t->tagP);
}

static void Finished(StreamFeed *f, FeedTrack *t)
{
    UInt16 http = t->result.httpStatus;
    if (t->result.received != 0) {
        t->failures = 0;
        t->redirects = 0;
    }
    if (t->result.status == netJobOk && t->result.received != 0) return;
    /* The server closed a connection kept open: just open a new one. */
    if (t->result.reused && t->result.received == 0 &&
        !t->result.redirected && http < 400) {
        UdpLogLine(f->config.logP, t->tagP, "reconnect", 0, 0, 0);
        return;
    }
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
    t->waiting = true;
    t->retryTicks = TimGetTicks() + (UInt32)t->failures * RETRY_SECONDS *
        SysTicksPerSecond();
    UdpLogLine(f->config.logP, t->tagP, "retry % in % s", t->failures,
        (UInt32)t->failures * RETRY_SECONDS, 0);
}

/* How much a track wants a request now: 0 if it cannot or need not ask,
 * otherwise more the emptier its ring. */
static UInt32 Want(const FeedTrack *t)
{
    UInt32 left, space, need;
    if (t->streamP == 0 || t->offset >= t->formatP->contentLength) return 0;
    if (t->waiting && (Int32)(TimGetTicks() - t->retryTicks) < 0) return 0;
    left = t->formatP->contentLength - t->offset;
    space = Mp4StreamSpace(t->streamP);
    need = t->streamP->capacity / REFILL_DIVISOR;
    if (need > left) need = left;
    if (space < need) return 0;
    return (space << 8) / t->streamP->capacity + 1;
}

void StreamFeedStep(StreamFeed *f, Boolean playing)
{
    FeedTrack *t = f->activeP;
    UInt32 now = TimGetTicks(), elapsed = now - f->paceTicks;
    UInt32 rate = PACE_BYTES_PER_SECOND;
    Boolean paced = playing, low = false;
    UInt16 i;
    if (elapsed > SysTicksPerSecond()) elapsed = SysTicksPerSecond();
    if (t != 0 && Mp4StreamBuffered(t->streamP) < t->streamP->capacity / 2) {
        low = true;
        rate *= 2;
    }
    f->allowance += elapsed * rate / SysTicksPerSecond();
    if (f->allowance > NET_JOB_IO_CAPACITY) f->allowance = NET_JOB_IO_CAPACITY;
    f->paceTicks = now;
    if (f->errorP != 0) return;
    if (t == 0) {
        UInt32 video = Want(&f->tracks[STREAM_FEED_VIDEO]);
        UInt32 audio = Want(&f->tracks[STREAM_FEED_AUDIO]);
        if (video == 0 && audio == 0) return;
        t = &f->tracks[audio > video ? STREAM_FEED_AUDIO : STREAM_FEED_VIDEO];
        t->waiting = false;
        Request(f, t);
        return;
    }
    if (!low) paced = true;
    for (i = 0; i < READS_PER_STEP; i++) {
        UInt32 before = t->offset;
        Boolean receiving = NetJobReceiving(t->jobP);
        if (paced && receiving && f->allowance < NET_JOB_IO_CAPACITY) return;
        if (NetJobStep(&t->jobP) != netJobRunning) {
            f->activeP = 0;
            Finished(f, t);
            return;
        }
        if (paced)
            f->allowance -= t->offset - before < f->allowance
                ? t->offset - before : f->allowance;
        if (!receiving || t->offset == before) return;
    }
}
