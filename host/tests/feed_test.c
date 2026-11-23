/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Runs src/palm/stream_feed.c against a fake network that serves two DASH
 * files, while a fake player takes samples out and stops to buffer when
 * either stream runs dry. Checks the feed's rules: one request in flight
 * at a time, none asking for more than its ring has room for, ranges in
 * order, reads paced while a ring is at least half full and less hard
 * below half while the player plays, and both streams read to the end.
 * Kept connections are reused, and some are dropped by the "server" to
 * exercise reconnects. Now and then both streams seek to random segments,
 * mid-request or not.
 *
 * With --live, the files are live segments in a row (pview live). The
 * "server" puts out one more each segment time, holds a request for the
 * next one until it is out, answers 204 when it does not come, and fails any
 * request out of order or past a segment's end. Some responses drop
 * partway, and the network goes quiet for a while. The player plays in
 * real time and buffers by playback.c's rule, so a feed that stops asking
 * too early gets it stuck. Once the segments run out the stream must end,
 * with every sample read in order.
 *
 *   feed_test [--live] VIDEO.mp4 AUDIO.mp4 VIDEO_RING_KB AUDIO_RING_KB SEED
 */

#include <PalmOS.h>

static UInt32 gTicks;
UInt32 TimGetTicks(void) { return gTicks; }
UInt16 SysTicksPerSecond(void) { return 100; }

#include "../../src/palm/stream_feed.c"

#define MAX_LOOPS 2000000UL
#define SAMPLE_CAPACITY 65536
#define SEEKS 4
#define MAX_SEGMENTS 64
#define FIRST_SEGMENT 5000UL
/* YouTube holds a request for the next segment this long. */
#define HOLD_TICKS 750
/* The same as in playback.c */
#define START_BUFFER_MS 3000UL
#define RESUME_BUFFER_MS 5000UL

typedef struct Source {
    const char *hostP;
    UInt8 *dataP;
    UInt32 size;
    UInt32 next;            /* the next byte the feed should ask for */
    Mp4Stream *streamP;
    UInt32 pacedSecond;     /* gTicks / 100 of the bytes counted below */
    UInt32 pacedBytes;      /* read in that second with the ring half full */
    UInt32 playingBytes;    /* and with it below half while playing */
    /* Live */
    UInt32 starts[MAX_SEGMENTS + 1];    /* where each segment starts */
    UInt16 count;
    UInt32 sequence;        /* the segment the feed should ask for */
    UInt32 inSegment;       /* its bytes delivered so far */
    UInt32 samples;         /* in the whole file */
} Source;

enum { jobConnecting, jobSending, jobDelivering, jobIdle, jobDead };

struct NetJob {
    Source *sourceP;
    UInt32 next, end;
    UInt32 readyTicks;      /* live: when the reply starts */
    UInt16 http;
    Boolean probe;          /* asks only for the newest number */
    HttpBodyProc bodyProcP;
    void *contextP;
    NetJobResult *resultP;
    int state;
    int wait;
    Boolean cancelled;
};

static Source gSources[2];
static YtFormat gFormats[2];
static Mp4Stream gStreams[2];
static Mp4ReadTrack gTracks[2];
static UInt32 gTaken[2], gLastDts[2], gRingKb[2];
static Boolean gEnded[2];
static int gActive, gRequests, gReused, gDropped, gClosed, gSeeks;
static Boolean gPlaying;
static UInt32 gPace = PACE_MINIMUM;
static Boolean gLive;
static UInt32 gSegmentTicks, gInitial, gQuietFrom, gQuietTo, gClock;
static int gProbes, gHeld, gEarly, gCut, gCancelled, gWaits;

static int Fail(const char *messageP)
{
    printf("FAIL: %s\n", messageP);
    exit(1);
}

static UInt8 *Load(const char *pathP, UInt32 *sizeP)
{
    FILE *fileP = fopen(pathP, "rb");
    UInt8 *dataP;
    long size;
    if (fileP == 0) return 0;
    fseek(fileP, 0, SEEK_END);
    size = ftell(fileP);
    rewind(fileP);
    dataP = (UInt8 *)malloc((size_t)size);
    if (dataP != 0 && fread(dataP, 1, (size_t)size, fileP) != (size_t)size) {
        free(dataP);
        dataP = 0;
    }
    fclose(fileP);
    *sizeP = (UInt32)size;
    return dataP;
}

/* Where each segment starts: each begins with an ftyp. */
static void FindSegments(Source *s)
{
    UInt32 offset = 0;
    s->count = 0;
    while (offset + 8 <= s->size) {
        UInt32 size = Mp4Get32(s->dataP + offset);
        if (Mp4Get32(s->dataP + offset + 4) == MP4_FOURCC('f', 't', 'y', 'p')) {
            if (s->count == MAX_SEGMENTS) Fail("too many segments");
            s->starts[s->count++] = offset;
        }
        if (size < 8) Fail("bad box");
        offset += size;
    }
    s->starts[s->count] = s->size;
    if (s->count == 0) Fail("no segments");
}

/* The newest segment out: gInitial at first, one more each segment time */
static UInt32 Newest(void)
{
    UInt32 out = gInitial + gTicks / gSegmentTicks, count = gSources[0].count;
    if (gSources[1].count < count) count = gSources[1].count;
    return FIRST_SEGMENT + (out < count ? out : count) - 1;
}

static void BeginLive(NetJob *jobP, const char *sequenceP, const char *rangeP)
{
    Source *s = jobP->sourceP;
    UInt32 start, end, sequence, newest = Newest();
    const UInt32 *boundsP;
    char *endP;
    jobP->http = 200;
    jobP->readyTicks = gTicks;
    jobP->probe = sequenceP == 0;
    if (jobP->probe) {
        if (rangeP == 0 || strncmp(rangeP, "range=0-0 ", 10) != 0)
            Fail("asked for more than the newest number");
        gProbes++;
        jobP->next = jobP->end = 0;
        return;
    }
    sequence = (UInt32)strtoul(sequenceP + 3, 0, 10);
    if (sequence > newest + 1) Fail("asked past the next segment");
    if (sequence != s->sequence) Fail("segments out of order");
    if (sequence > newest) {
        /* Held until it is out, or answered with nothing. */
        UInt32 due = (sequence - FIRST_SEGMENT + 1 - gInitial) * gSegmentTicks;
        gHeld++;
        if (sequence - FIRST_SEGMENT >= s->count || due > gTicks + HOLD_TICKS) {
            jobP->readyTicks = gTicks + HOLD_TICKS;
            jobP->http = 204;
            return;
        }
        jobP->readyTicks = due;
    }
    boundsP = &s->starts[sequence - FIRST_SEGMENT];
    start = 0;
    end = boundsP[1] - boundsP[0] - 1;
    if (rangeP != 0) {
        UInt32 last;
        start = (UInt32)strtoul(rangeP + 6, &endP, 10);
        last = (UInt32)strtoul(endP + 1, 0, 10);
        if (last > end) Fail("range past the segment's end");
        end = last;
    }
    if (start != s->inSegment) Fail("request does not continue the segment");
    jobP->next = boundsP[0] + start;
    jobP->end = boundsP[0] + end;
}

static void BeginRange(NetJob *jobP, const char *rangeP)
{
    char *endP;
    if (rangeP == 0) Fail("request without a range");
    jobP->next = (UInt32)strtoul(rangeP + 6, &endP, 10);
    jobP->end = (UInt32)strtoul(endP + 1, 0, 10);
    if (jobP->next != jobP->sourceP->next) Fail("range does not continue");
    if (jobP->end < jobP->next || jobP->end >= jobP->sourceP->size)
        Fail("range out of bounds");
}

/* Reads what a request asks for, and checks it continues the file. */
static void Begin(NetJob *jobP, const Char *requestP)
{
    const char *rangeP = strstr(requestP, "range=");
    if (strstr(requestP, "Connection: keep-alive") == 0)
        Fail("request without keep-alive");
    if (++gActive > 1) Fail("two requests in flight");
    gRequests++;
    if (gLive) BeginLive(jobP, strstr(requestP, "sq="), rangeP);
    else BeginRange(jobP, rangeP);
}

Boolean NetJobStart(NetJob **jobPP, const NetJobConfig *configP,
                    const Char *hostP, const Char *requestP,
                    UInt16 requestLength, HttpBodyProc bodyProcP,
                    void *bodyContextP, NetJobResult *resultP)
{
    NetJob *jobP = (NetJob *)calloc(1, sizeof(NetJob));
    (void)requestLength;
    if (!configP->keepAlive) Fail("keepAlive not set");
    MemSet(resultP, sizeof(*resultP), 0);
    jobP->sourceP = strcmp(hostP, gSources[0].hostP) == 0 ? &gSources[0]
        : &gSources[1];
    jobP->bodyProcP = bodyProcP;
    jobP->contextP = bodyContextP;
    jobP->resultP = resultP;
    jobP->state = jobConnecting;
    jobP->wait = 20;        /* the connection and handshake take a while */
    Begin(jobP, requestP);
    *jobPP = jobP;
    return true;
}

Boolean NetJobSend(NetJob *jobP, const Char *requestP, UInt16 requestLength)
{
    (void)requestLength;
    if (jobP->state != jobIdle) return false;
    MemSet(jobP->resultP, sizeof(*jobP->resultP), 0);
    jobP->resultP->reused = true;
    gReused++;
    Begin(jobP, requestP);
    /* The server sometimes closed the connection while it sat idle. */
    jobP->state = rand() % 5 == 0 ? jobDead : jobSending;
    jobP->wait = 2;
    return true;
}

static UInt16 End(NetJob **jobPP)
{
    free(*jobPP);
    *jobPP = 0;
    return netJobFinished;
}

UInt16 NetJobStep(NetJob **jobPP)
{
    NetJob *jobP = *jobPP;
    NetJobResult *r = jobP->resultP;
    Source *s = jobP->sourceP;
    UInt32 n;
    if (jobP->cancelled) {
        if (jobP->state != jobIdle) gActive--;
        if (jobP->state == jobDelivering) gCancelled++;
        r->status = netJobCancelled;
        return End(jobPP);
    }
    if (gTicks >= gQuietFrom && gTicks < gQuietTo) return netJobRunning;
    switch (jobP->state) {
        case jobIdle:
            return netJobIdle;
        case jobDead:
            gActive--;
            gDropped++;
            r->status = netJobNetworkError;
            r->netError = 0x1212;
            return End(jobPP);
        case jobConnecting:
        case jobSending:
            if (--jobP->wait > 0 || gTicks < jobP->readyTicks) return netJobRunning;
            jobP->state = jobDelivering;
            if (gLive) {
                /* The headers */
                r->httpStatus = jobP->http;
                r->hasNumber = true;
                r->number = Newest();
                if (jobP->http == 204) {
                    gActive--;
                    gEarly++;
                    r->status = netJobOk;
                    jobP->state = jobIdle;
                    return netJobIdle;
                }
                r->contentLength = jobP->end - jobP->next + 1;
            }
            return netJobRunning;
        default:
            break;
    }
    /* Live connections drop partway now and then. */
    if (gLive && !jobP->probe && rand() % 300 == 0) {
        gActive--;
        gCut++;
        r->status = netJobNetworkError;
        r->netError = 0x1212;
        return End(jobPP);
    }
    n = (UInt32)(rand() % NET_JOB_IO_CAPACITY + 1);
    if (n > jobP->end - jobP->next + 1) n = jobP->end - jobP->next + 1;
    if (s->pacedSecond != gTicks / 100) {
        s->pacedSecond = gTicks / 100;
        s->pacedBytes = 0;
        s->playingBytes = 0;
    }
    if (Mp4StreamBuffered(s->streamP) >= s->streamP->capacity / 2)
        s->pacedBytes += n;
    else if (gPlaying)
        s->playingBytes += n;
    if (s->pacedBytes > gPace + 2 * NET_JOB_IO_CAPACITY ||
        s->playingBytes > 2 * gPace + 2 * NET_JOB_IO_CAPACITY)
        Fail("paced reads too fast");
    if (jobP->bodyProcP(jobP->contextP, s->dataP + jobP->next, (UInt16)n) != 0)
        Fail("more data than the ring had room for");
    jobP->next += n;
    r->received += n;
    if (!jobP->probe) {
        s->next += n;
        s->inSegment += n;
        if (gLive && s->next == s->starts[s->sequence - FIRST_SEGMENT + 1]) {
            s->sequence++;
            s->inSegment = 0;
        }
    }
    if (jobP->next <= jobP->end) return netJobRunning;
    gActive--;
    r->status = netJobOk;
    r->httpStatus = 200;
    /* Now and then the server ends with Connection: close. */
    if (rand() % 7 == 0) {
        gClosed++;
        return End(jobPP);
    }
    jobP->state = jobIdle;
    return netJobIdle;
}

void NetJobCancel(NetJob *jobP) { jobP->cancelled = true; }
Boolean NetJobReceiving(const NetJob *jobP) { return jobP->state == jobDelivering; }
void NetJobKeepLocation(NetJob *jobP, Char *bufferP, UInt16 capacity)
{
    (void)jobP;
    if (capacity != 0) bufferP[0] = '\0';
}
void NetJobSetTag(NetJob *jobP, const Char *tagP) { (void)jobP; (void)tagP; }
void UdpLogLine(UdpLog *logP, const Char *tagP, const Char *formatP,
                UInt32 a, UInt32 b, UInt32 c)
{
    (void)logP; (void)tagP; (void)formatP; (void)a; (void)b; (void)c;
}

/* Counts a whole live file's samples. Returns its segments' length. */
static UInt32 MeasureLive(Source *s)
{
    static UInt8 sample[SAMPLE_CAPACITY];
    Mp4Stream stream;
    Mp4ReadTrack track;
    Mp4Sample out = { 0 };
    UInt32 seconds;
    UInt16 result;
    if (Mp4StreamOpen(&stream, s->dataP, s->size, MP4_STREAM_LIVE, &track) !=
            mp4MergeOk)
        Fail("cannot open a stream");
    stream.writeOffset = s->size;
    Mp4StreamFinish(&stream, s->size);
    while ((result = Mp4StreamNext(&stream, &out, sample, sizeof(sample))) ==
           mp4StreamSample)
        s->samples++;
    if (result != mp4StreamEnd) Fail("whole live file unreadable");
    /* The last sample ends where the file does, about. */
    seconds = (out.dts / s->count + track.timescale / 2) / track.timescale;
    Mp4StreamClose(&stream);
    return seconds;
}

static void OpenSource(int i, const char *pathP, const char *ringKbP)
{
    Source *s = &gSources[i];
    gRingKb[i] = (UInt32)atoi(ringKbP);
    s->hostP = i == 0 ? "v.example" : "a.example";
    s->streamP = &gStreams[i];
    s->sequence = FIRST_SEGMENT;
    s->dataP = Load(pathP, &s->size);
    if (s->dataP == 0) Fail("cannot read the inputs");
    snprintf(gFormats[i].url, sizeof(gFormats[i].url),
        "https://%s/videoplayback?id=1", s->hostP);
    if (gLive) {
        FindSegments(s);
        gFormats[i].segmentSeconds = (UInt16)MeasureLive(s);
        gFormats[i].bitrate = i == 0 ? 192223 : 144000;
    } else {
        gFormats[i].contentLength = s->size;
    }
    if (Mp4StreamOpen(&gStreams[i], (UInt8 *)malloc(gRingKb[i] * 1024),
            gRingKb[i] * 1024, gLive ? MP4_STREAM_LIVE : s->size, &gTracks[i]) !=
            mp4MergeOk)
        Fail("cannot open a stream");
}

/* The feed starts LIVE_BEHIND_SECONDS back: at the first segment. The
 * network goes quiet for 5 to 25 s. */
static void StartServer(void)
{
    UInt32 seconds = gFormats[0].segmentSeconds;
    gSegmentTicks = seconds * 100;
    gInitial = (LIVE_BEHIND_SECONDS + seconds - 1) / seconds + 1;
    if (gInitial > gSources[0].count || gInitial > gSources[1].count)
        Fail("too few segments");
    gQuietFrom = 200 + (UInt32)(rand() % 1000);
    gQuietTo = gQuietFrom + 500 + (UInt32)(rand() % 2000);
}

/* 240p and its sound over 213 s: 36 KB/s, paced a quarter above. */
static void TestPace(const NetJobConfig *configP, const YtClient *clientP)
{
    YtFormat video = gFormats[0], audio = gFormats[1];
    StreamFeed *f;
    video.contentLength = 4310122UL;
    audio.contentLength = 3449447UL;
    f = StreamFeedNew(configP, clientP, &video, &audio, 213, false);
    if (f == 0 || f->pace != 45500UL) Fail("pace from the bitrates");
    StreamFeedFree(f);
}

/* The player waits while either stream has nothing ready. */
static Boolean Waiting(void)
{
    Boolean waiting = false;
    int i;
    for (i = 0; i < 2; i++)
        if (!gEnded[i] && (!Mp4StreamReady(&gStreams[i]) ||
                (Mp4StreamBuffered(&gStreams[i]) < 8192 &&
                 !Mp4StreamReceived(&gStreams[i]))))
            waiting = true;
    return waiting;
}

static Boolean SeekNowAndThen(StreamFeed *f)
{
    UInt32 offsets[2];
    int i;
    if (gSeeks >= SEEKS || rand() % 2000 != 0 ||
        gStreams[0].segmentCount == 0 || gStreams[1].segmentCount == 0)
        return false;
    for (i = 0; i < 2; i++) {
        UInt16 segment = (UInt16)((UInt32)rand() % gStreams[i].segmentCount);
        if (!Mp4StreamSeek(&gStreams[i], segment)) Fail("seek refused");
        offsets[i] = gStreams[i].segmentsP[segment].offset;
        gSources[i].next = offsets[i];
        gEnded[i] = false;
    }
    StreamFeedSeek(f, offsets[0], offsets[1]);
    gSeeks++;
    return true;
}

static UInt16 TakeSample(int i)
{
    static UInt8 sample[SAMPLE_CAPACITY];
    Mp4Sample s;
    UInt16 result = Mp4StreamNext(&gStreams[i], &s, sample, sizeof(sample));
    if (result == mp4StreamError) Fail("stream error");
    if (result == mp4StreamEnd) gEnded[i] = true;
    if (result != mp4StreamSample) return result;
    if (gLive && (gTaken[i] == 0 ? s.dts != 0 : s.dts <= gLastDts[i]))
        Fail("times do not start at 0 and grow");
    gLastDts[i] = s.dts;
    gTaken[i]++;
    return result;
}

static void PlayVideo(StreamFeed *f, UInt32 loops)
{
    Boolean waiting = Waiting();
    int i;
    if (SeekNowAndThen(f)) return;
    gPlaying = !waiting;
    for (i = 0; i < 2; i++)
        if (gPlaying && !gEnded[i] && loops % (i == 0 ? 3 : 2) == 0)
            TakeSample(i);
}

/* Both streams have ms in, or full rings: StreamsBuffered in playback.c. */
static Boolean Buffered(const StreamFeed *f, UInt32 ms)
{
    UInt16 i;
    for (i = 0; i < 2; i++) {
        const Mp4Stream *s = &gStreams[i];
        if (!Mp4StreamReady(&gStreams[i])) return false;
        if (!Mp4StreamReceived(s) && !StreamFeedFull(f, i) &&
            Mp4StreamBuffered(s) * 1000UL / (StreamFeedRate(f, i) + 1) < ms)
            return false;
    }
    return true;
}

static void PlayLive(const StreamFeed *f)
{
    /* Ticks of decoded frames and sound the player keeps ahead */
    static const UInt32 kAhead[2] = { 10, 70 };
    int i;
    if (!gPlaying)
        gPlaying = Buffered(f, gClock == 0 ? START_BUFFER_MS : RESUME_BUFFER_MS);
    if (gPlaying) gClock += 2;
    for (i = 0; i < 2; i++) {
        while (gPlaying && !gEnded[i] &&
               gLastDts[i] / (gTracks[i].timescale / 100) <= gClock + kAhead[i]) {
            if (TakeSample(i) != mp4StreamWait) continue;
            gPlaying = false;
            gWaits++;
        }
    }
}

int main(int argc, char **argv)
{
    YtClient client;
    NetJobConfig config;
    StreamFeed *f;
    UInt32 loops;
    int i;
    gLive = argc > 1 && strcmp(argv[1], "--live") == 0;
    if (gLive) {
        argc--;
        argv++;
    }
    if (argc != 6) {
        fprintf(stderr, "usage: feed_test [--live] VIDEO.mp4 AUDIO.mp4 "
            "VIDEO_RING_KB AUDIO_RING_KB SEED\n");
        return 2;
    }
    srand((unsigned)atoi(argv[5]));
    for (i = 0; i < 2; i++) OpenSource(i, argv[1 + i], argv[3 + i]);
    YtClientDefaults(&client);
    MemSet(&config, sizeof(config), 0);
    if (gLive) StartServer();
    else TestPace(&config, &client);
    /* Without a video's length, as here, the pace is the minimum. */
    f = StreamFeedNew(&config, &client, &gFormats[0], &gFormats[1], 0, gLive);
    if (f == 0 || StreamFeedLive(f) != gLive) Fail("feed not started");
    gPace = f->pace;
    StreamFeedStart(f, &gStreams[0], &gStreams[1], false);
    for (loops = 0; !(gEnded[0] && gEnded[1]); loops++) {
        if (loops == MAX_LOOPS) Fail("streams never finished");
        gTicks += 2;
        StreamFeedStep(f, gPlaying);
        if (StreamFeedError(f) != 0) Fail(StreamFeedError(f));
        if (gLive) PlayLive(f);
        else PlayVideo(f, loops);
    }
    for (i = 0; i < 2; i++) {
        if (gSources[i].next != gSources[i].size) Fail("a stream was cut short");
        if (gLive && gTaken[i] != gSources[i].samples) Fail("samples missing");
        if (gLive && StreamFeedRate(f, (UInt16)i) == 0) Fail("no rate");
    }
    printf("%s ok: %u + %u samples, %d requests (%d reused, %d dropped, "
        "%d closed), %d seeks, rings %u + %u KB\n", gLive ? "live feed" : "feed",
        (unsigned)gTaken[0], (unsigned)gTaken[1], gRequests, gReused, gDropped,
        gClosed, gSeeks, (unsigned)gRingKb[0], (unsigned)gRingKb[1]);
    if (gLive)
        printf("  %d cut, %d cancelled, %d probes, %d held, %d early, %d waits, "
            "rates %u + %u B/s, %u s\n", gCut, gCancelled, gProbes, gHeld, gEarly,
            gWaits, (unsigned)StreamFeedRate(f, 0), (unsigned)StreamFeedRate(f, 1),
            (unsigned)(gTicks / 100));
    StreamFeedFree(f);
    return 0;
}
