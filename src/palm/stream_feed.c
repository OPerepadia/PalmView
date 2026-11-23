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
/* Failed connections in a row before giving up; the wait before trying
 * again grows by RETRY_SECONDS each time. */
#define MAX_FAILURES 5
#define RETRY_SECONDS 1
/* Redirects in a row before giving up. */
#define MAX_REDIRECTS 5
/* Reads per stream in one StreamFeedStep, while data keeps coming. */
#define READS_PER_STEP 4
/* While the stream being fetched has at least half its ring buffered, its
 * response is read at a pace a little above both streams' bitrate
 * together: PACE_PERCENT of it, and at least PACE_MINIMUM. Each 16 KB TLS
 * record is decrypted in one go, and a frame due meanwhile is late: read
 * faster, and too many frames are. Below half, it is read at twice the
 * pace while video plays, and as fast as it comes while nothing does. */
#define PACE_PERCENT 125
#define PACE_MINIMUM (32UL * 1024UL)

/* A response that sends nothing this long is taken as stalled, and asked
 * for again on a new connection. */
#define IDLE_SECONDS 5

/* Live streams start this far behind the newest segment, as a buffer. With
 * no new segment for LIVE_END_SECONDS, the stream has ended. */
#define LIVE_BEHIND_SECONDS 15
#define LIVE_END_SECONDS 60
#define LIVE_NUMBER_HEADER "X-Head-Seqnum:"

typedef struct FeedTrack {
    StreamFeed *feedP;
    const Char *tagP;       /* in the log */
    YtFormat *formatP;
    Mp4Stream *streamP;
    NetJob *jobP;
    NetJobResult result;
    UInt32 offset;          /* next byte to ask for */
    UInt32 limit;           /* asks stop here until a seek, if not 0 */
    UInt32 retryTicks;      /* when to connect again, if waiting */
    Boolean waiting;
    UInt16 failures;
    UInt16 redirects;
    Char host[64];
    Char request[YT_REQUEST_CAPACITY];
    Char location[YT_URL_CAPACITY];

    /* Live streams */
    UInt32 sequence;        /* the segment being fetched, or next */
    UInt32 segmentLength;   /* its size, once a response says */
    UInt32 segmentReceived; /* its bytes in the stream so far */
    UInt32 largest;         /* the largest segment so far */
    UInt32 liveBytes;       /* all segments so far, for the rate */
    UInt32 liveSegments;
    Boolean probing;        /* asking only for the newest number */
    Boolean whole;          /* asking for a whole segment */
    Boolean ended;          /* no more segments are coming */
} FeedTrack;

struct StreamFeed {
    NetJobConfig config;
    const YtClient *clientP;
    FeedTrack tracks[2];
    FeedTrack *activeP;     /* the track with a request in flight, or 0 */
    UInt32 pace;            /* bytes per second */
    UInt32 allowance;       /* bytes a paced read may take now */
    UInt32 paceTicks;       /* when the allowance last grew */
    UInt32 received;
    const Char *errorP;
    Boolean failed;
    Boolean dropped;
    NetJobResult failure;
    NetJobResult drop;

    Boolean live;
    UInt32 segmentTicks;
    Boolean headKnown;
    UInt32 head;            /* the newest segment YouTube has */
    UInt32 headTicks;       /* when head last moved on */
};

StreamFeed *StreamFeedNew(const NetJobConfig *configP, const YtClient *clientP,
                          YtFormat *videoP, YtFormat *audioP, UInt32 seconds,
                          Boolean live)
{
    StreamFeed *f = (StreamFeed *)MemPtrNew(sizeof(StreamFeed));
    UInt32 rate = 0;
    if (f == 0) return 0;
    MemSet(f, sizeof(*f), 0);
    f->config = *configP;
    f->config.poll = true;
    f->config.idleTimeoutTicks = (Int32)IDLE_SECONDS * SysTicksPerSecond();
    f->config.keepAlive = true;
    f->clientP = clientP;
    f->live = live;
    if (live) {
        UInt16 segment = videoP->segmentSeconds > audioP->segmentSeconds
            ? videoP->segmentSeconds : audioP->segmentSeconds;
        f->segmentTicks = (UInt32)segment * SysTicksPerSecond();
        rate = (videoP->bitrate / 8 + audioP->bitrate / 8) / 100 * PACE_PERCENT;
    } else if (seconds != 0) {
        rate = (videoP->contentLength / seconds + audioP->contentLength / seconds) /
            100 * PACE_PERCENT;
    }
    f->pace = rate > PACE_MINIMUM ? rate : PACE_MINIMUM;
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
    return f->live ? MP4_STREAM_LIVE : f->tracks[track].formatP->contentLength;
}

Boolean StreamFeedLive(const StreamFeed *f)
{
    return f->live;
}

UInt32 StreamFeedRate(const StreamFeed *f, UInt16 track)
{
    const FeedTrack *t = &f->tracks[track];
    UInt32 seconds = f->segmentTicks / SysTicksPerSecond();
    if (t->liveSegments == 0 || seconds == 0) return t->formatP->bitrate / 8;
    return t->liveBytes / t->liveSegments / seconds;
}

void StreamFeedStart(StreamFeed *f, Mp4Stream *videoP, Mp4Stream *audioP,
                     Boolean headersFirst)
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
        t->limit = headersFirst ? t->formatP->indexEnd : 0;
        t->waiting = false;
        t->failures = 0;
        t->redirects = 0;
        t->segmentLength = 0;
        t->segmentReceived = 0;
        t->probing = false;
        t->ended = false;
    }
    f->headKnown = false;
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

void StreamFeedReconnect(StreamFeed *f)
{
    UInt16 i;
    StreamFeedStop(f);
    f->errorP = 0;
    for (i = 0; i < 2; i++) {
        f->tracks[i].waiting = false;
        f->tracks[i].failures = 0;
    }
    UdpLogLine(f->config.logP, "feed", "reconnect", 0, 0, 0);
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
        t->limit = 0;
        t->waiting = false;
        t->failures = 0;
        t->redirects = 0;
        UdpLogLine(f->config.logP, t->tagP, "seek to %", t->offset, 0, 0);
    }
}

Boolean StreamFeedHeld(const StreamFeed *f)
{
    UInt16 i;
    if (f->activeP != 0) return false;
    for (i = 0; i < 2; i++) {
        const FeedTrack *t = &f->tracks[i];
        if (t->limit == 0 || t->offset < t->limit) return false;
    }
    return true;
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
    /* Only the probe's headers matter. */
    if (t->probing) return 0;
    if (length > Mp4StreamSpace(t->streamP)) return 1;
    Mp4StreamWrite(t->streamP, dataP, length);
    t->offset += length;
    t->segmentReceived += length;
    t->feedP->received += length;
    return 0;
}

/* The next range of a video, as much as the ring has room for */
static UInt16 BuildRange(StreamFeed *f, FeedTrack *t)
{
    UInt32 length = t->limit != 0 ? t->limit : t->formatP->contentLength;
    UInt32 room = Mp4StreamSpace(t->streamP);
    UInt32 end;
    if (room > REQUEST_BYTES) room = REQUEST_BYTES;
    end = t->offset + room - 1;
    if (end >= length) end = length - 1;
    UdpLogLine(f->config.logP, t->tagP, "get %-% of %", t->offset, end, length);
    return YtBuildMediaRequest(f->clientP, t->formatP->url, t->offset, end,
        true, t->request, sizeof(t->request));
}

/* Live: one byte of the newest segment, for its number, until that is
 * known. Then the next whole segment, or the rest of one that a dropped
 * connection cut short. */
static UInt16 BuildLive(StreamFeed *f, FeedTrack *t)
{
    UInt32 sequence = t->sequence, start = 0, end = YT_WHOLE_SEGMENT;
    t->probing = !f->headKnown;
    if (t->probing) {
        sequence = YT_NEWEST_SEGMENT;
        end = 0;
    } else if (t->segmentReceived != 0) {
        if (t->segmentLength <= t->segmentReceived) return 0;
        start = t->segmentReceived;
        end = t->segmentLength - 1;
    }
    t->whole = !t->probing && end == YT_WHOLE_SEGMENT;
    if (t->probing)
        UdpLogLine(f->config.logP, t->tagP, "newest segment?", 0, 0, 0);
    else
        UdpLogLine(f->config.logP, t->tagP, "get segment % from %", sequence,
            start, 0);
    return YtBuildSegmentRequest(f->clientP, t->formatP->url, sequence, start,
        end, true, t->request, sizeof(t->request));
}

static void Request(StreamFeed *f, FeedTrack *t)
{
    const Char *pathP;
    UInt16 size = f->live ? BuildLive(f, t) : BuildRange(f, t);
    if (size == 0 || !YtSplitUrl(t->formatP->url, t->host, sizeof(t->host),
            &pathP)) {
        f->errorP = "The stream link is not usable.";
        return;
    }
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
    if (f->live) NetJobKeepNumber(t->jobP, LIVE_NUMBER_HEADER);
}

/* Both streams start LIVE_BEHIND_SECONDS before the newest segment. */
static void StartLive(StreamFeed *f, UInt32 newest)
{
    UInt32 seconds = f->segmentTicks / SysTicksPerSecond();
    UInt32 back = seconds == 0 ? 1 : (LIVE_BEHIND_SECONDS + seconds - 1) / seconds;
    UInt16 i;
    for (i = 0; i < 2; i++)
        f->tracks[i].sequence = newest > back ? newest - back : 0;
    f->headKnown = true;
    UdpLogLine(f->config.logP, "feed", "live: newest %, from %", newest,
        f->tracks[0].sequence, 0);
}

static void NextSegment(FeedTrack *t)
{
    t->liveBytes += t->segmentReceived;
    t->liveSegments++;
    if (t->segmentReceived > t->largest) t->largest = t->segmentReceived;
    t->sequence++;
    t->segmentReceived = 0;
    t->segmentLength = 0;
    t->failures = 0;
    t->redirects = 0;
}

/* The stream ends where its unfinished segment starts. */
static void EndLive(StreamFeed *f, FeedTrack *t)
{
    t->ended = true;
    Mp4StreamFinish(t->streamP, t->offset - t->segmentReceived);
    UdpLogLine(f->config.logP, t->tagP, "live stream ended at segment %",
        t->sequence, 0, 0);
}

/* Live: notes the newest segment, and moves on once a segment is in.
 * False when that settles the response. */
static Boolean FinishedLive(StreamFeed *f, FeedTrack *t)
{
    const NetJobResult *r = &t->result;
    UInt32 now = TimGetTicks();
    if (r->hasNumber && (!f->headKnown || r->number > f->head)) {
        if (!f->headKnown) StartLive(f, r->number);
        f->head = r->number;
        f->headTicks = now;
    }
    if (t->whole && r->httpStatus == 200 && r->contentLength != 0)
        t->segmentLength = r->contentLength;
    if (t->probing) {
        if (r->status != netJobOk) return true;
        t->probing = false;
        t->failures = 0;
        if (!f->headKnown) f->errorP = "YouTube did not say where the live stream is.";
        return false;
    }
    if (r->status == netJobOk && r->received != 0 &&
        (t->segmentLength == 0 || t->segmentReceived >= t->segmentLength)) {
        NextSegment(t);
        return false;
    }
    /* Asked too early: YouTube waits a few seconds, then has nothing. */
    if (r->httpStatus == 204 || (r->httpStatus == 404 && t->sequence > f->head)) {
        if (now - f->headTicks >= LIVE_END_SECONDS * (UInt32)SysTicksPerSecond()) {
            EndLive(f, t);
            return false;
        }
        t->waiting = true;
        t->retryTicks = now + f->segmentTicks / 2;
        UdpLogLine(f->config.logP, t->tagP, "segment % not out yet",
            t->sequence, 0, 0);
        return false;
    }
    return true;
}

static void Finished(StreamFeed *f, FeedTrack *t)
{
    UInt16 http = t->result.httpStatus;
    if (f->live && !FinishedLive(f, t)) return;
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

/* Live: a segment is asked for once YouTube should have it, and only
 * with room for all of it, guessed from the largest so far. Until the
 * newest number is known, only video asks, for that. */
static UInt32 LiveWant(const StreamFeed *f, const FeedTrack *t)
{
    UInt32 space, need, capacity = t->streamP->capacity;
    if (!f->headKnown) return t == &f->tracks[STREAM_FEED_VIDEO] ? 1 : 0;
    if (t->ended || t->sequence > f->head + 1) return 0;
    if (t->sequence == f->head + 1 &&
        TimGetTicks() - f->headTicks < f->segmentTicks)
        return 0;
    space = Mp4StreamSpace(t->streamP);
    need = t->largest != 0 ? t->largest + t->largest / 4
        : t->formatP->bitrate / 8 * (f->segmentTicks / SysTicksPerSecond());
    if (need > capacity / 4 * 3) need = capacity / 4 * 3;
    if (space < need) return 0;
    return (space << 8) / capacity + 1;
}

/* How much a track wants a request now: 0 if it cannot or need not ask,
 * otherwise more the emptier its ring. */
static UInt32 Want(const StreamFeed *f, const FeedTrack *t)
{
    UInt32 end = t->limit != 0 ? t->limit : t->formatP->contentLength;
    UInt32 left, space, need;
    if (t->streamP == 0) return 0;
    if (t->waiting && (Int32)(TimGetTicks() - t->retryTicks) < 0) return 0;
    if (f->live) return LiveWant(f, t);
    if (t->offset >= end) return 0;
    left = end - t->offset;
    space = Mp4StreamSpace(t->streamP);
    need = t->streamP->capacity / STREAM_FEED_REFILL_DIVISOR;
    if (need > left) need = left;
    if (space < need) return 0;
    return (space << 8) / t->streamP->capacity + 1;
}

/* A live segment can be larger than its ring has room for: then its
 * response waits until the player has made room for the next read. */
static Boolean LiveRoom(const FeedTrack *t)
{
    UInt32 need = NET_JOB_IO_CAPACITY, length;
    if (t->probing) return true;
    length = t->whole ? t->result.contentLength : t->segmentLength;
    if (length > t->segmentReceived && length - t->segmentReceived < need)
        need = length - t->segmentReceived;
    return Mp4StreamSpace(t->streamP) >= need;
}

/* While a live response waits for room, the other stream gets nothing.
 * Once that one runs low, the player may stop taking from either, so the
 * response is cut, to be finished later with a range, if the other can
 * ask now. */
static void WaitForRoom(StreamFeed *f, FeedTrack *t)
{
    const FeedTrack *otherP = &f->tracks[t == &f->tracks[STREAM_FEED_VIDEO]
        ? STREAM_FEED_AUDIO : STREAM_FEED_VIDEO];
    const Mp4Stream *s = otherP->streamP;
    if (Mp4StreamBuffered(s) >= s->capacity / 4 || Want(f, otherP) == 0)
        return;
    if (t->whole && t->result.contentLength != 0)
        t->segmentLength = t->result.contentLength;
    if (t->segmentLength <= t->segmentReceived) return;
    UdpLogLine(f->config.logP, t->tagP, "cut segment % at %", t->sequence,
        t->segmentReceived, 0);
    Close(t);
    f->activeP = 0;
}

void StreamFeedStep(StreamFeed *f, Boolean playing)
{
    FeedTrack *t = f->activeP;
    UInt32 now = TimGetTicks(), elapsed = now - f->paceTicks;
    UInt32 rate = f->pace;
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
        UInt32 video = Want(f, &f->tracks[STREAM_FEED_VIDEO]);
        UInt32 audio = Want(f, &f->tracks[STREAM_FEED_AUDIO]);
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
        if (f->live && receiving && !LiveRoom(t)) {
            WaitForRoom(f, t);
            return;
        }
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
