/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Runs src/palm/stream_feed.c against a fake network that serves two DASH
 * files, while a fake player takes samples out and stops to buffer when
 * either stream runs dry. Checks the feed's rules: one request in flight
 * at a time, none asking for more than its ring has room for, ranges in
 * order, reads paced while a ring is at least half full and less hard
 * below half while the player plays, and both streams read to the end. Kept connections are reused,
 * and some are dropped by the "server" to exercise reconnects. Now and
 * then both streams seek to random segments, mid-request or not.
 *
 * With --live, the files are live segments in a row (pview live). The
 * "server" puts out one more every SECONDS, holds a request for the next
 * one until it is out, answers 204 when it does not come, and fails any
 * request out of order or past a segment's end. Some responses drop
 * partway. Once the segments run out the stream must end, with every
 * sample read in order.
 *
 *   feed_test VIDEO.mp4 AUDIO.mp4 VIDEO_RING_KB AUDIO_RING_KB SEED
 *   feed_test --live VIDEO.mp4 AUDIO.mp4 SECONDS VIDEO_RING_KB AUDIO_RING_KB SEED
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
} Source;

enum { jobConnecting, jobSending, jobDelivering, jobIdle, jobDead };

struct NetJob {
    Source *sourceP;
    UInt32 next, end;
    UInt32 readyTicks;      /* live: when the reply starts */
    UInt32 sequence;
    UInt16 http;
    Boolean probe;
    Boolean hold;           /* waiting for the next segment */
    HttpBodyProc bodyProcP;
    void *contextP;
    NetJobResult *resultP;
    int state;
    int wait;
    Boolean cancelled;
};

static Source gSources[2];
static int gActive, gRequests, gReused, gDropped, gClosed, gSeeks;
static Boolean gPlaying;
static UInt32 gPace = PACE_MINIMUM;
static Boolean gLive;
static UInt32 gSegmentTicks, gInitial;
static int gProbes, gHeld, gEarly, gCut, gCancelled;

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

static void BeginLive(NetJob *jobP, const Char *requestP)
{
    Source *s = jobP->sourceP;
    const char *sequenceP = strstr(requestP, "sq="), *rangeP = strstr(requestP, "range=");
    UInt32 start = 0, end, size, newest = Newest();
    char *endP;
    jobP->http = 200;
    jobP->readyTicks = gTicks;
    jobP->probe = sequenceP == 0;
    jobP->hold = false;
    if (jobP->probe) {
        if (rangeP == 0 || strncmp(rangeP, "range=0-0 ", 10) != 0)
            Fail("asked for more than the newest number");
        gProbes++;
        jobP->sequence = newest;
        jobP->next = jobP->end = s->starts[newest - FIRST_SEGMENT];
        return;
    }
    jobP->sequence = (UInt32)strtoul(sequenceP + 3, 0, 10);
    if (jobP->sequence < FIRST_SEGMENT) Fail("asked for a segment before the first");
    if (jobP->sequence > newest + 1) Fail("asked past the next segment");
    if (jobP->sequence != s->sequence) Fail("segments out of order");
    if (jobP->sequence > newest) {
        /* Held until it is out, or answered with nothing. */
        UInt32 due = (jobP->sequence - FIRST_SEGMENT + 1 - gInitial) * gSegmentTicks;
        jobP->hold = true;
        gHeld++;
        if (jobP->sequence - FIRST_SEGMENT < s->count && due <= gTicks + HOLD_TICKS) {
            jobP->readyTicks = due;
        } else {
            jobP->readyTicks = gTicks + HOLD_TICKS;
            jobP->http = 204;
            return;
        }
    }
    size = s->starts[jobP->sequence - FIRST_SEGMENT + 1] -
        s->starts[jobP->sequence - FIRST_SEGMENT];
    end = size - 1;
    if (rangeP != 0) {
        start = (UInt32)strtoul(rangeP + 6, &endP, 10);
        end = (UInt32)strtoul(endP + 1, 0, 10);
        if (start != s->inSegment) Fail("rest of a segment does not continue");
        if (end >= size) Fail("range past the segment's end");
    } else if (s->inSegment != 0) {
        Fail("whole segment asked again");
    }
    jobP->next = s->starts[jobP->sequence - FIRST_SEGMENT] + start;
    jobP->end = s->starts[jobP->sequence - FIRST_SEGMENT] + end;
}

/* Reads the range a request asks for, and checks it continues the file. */
static void Begin(NetJob *jobP, const Char *requestP)
{
    const char *rangeP = strstr(requestP, "range=");
    char *endP;
    if (gLive) {
        if (strstr(requestP, "Connection: keep-alive") == 0)
            Fail("request without keep-alive");
        if (++gActive > 1) Fail("two requests in flight");
        gRequests++;
        BeginLive(jobP, requestP);
        return;
    }
    if (rangeP == 0 || strstr(requestP, "Connection: keep-alive") == 0)
        Fail("request without a range or keep-alive");
    jobP->next = (UInt32)strtoul(rangeP + 6, &endP, 10);
    jobP->end = (UInt32)strtoul(endP + 1, 0, 10);
    if (jobP->next != jobP->sourceP->next) Fail("range does not continue");
    if (jobP->end < jobP->next || jobP->end >= jobP->sourceP->size)
        Fail("range out of bounds");
    if (++gActive > 1) Fail("two requests in flight");
    gRequests++;
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
    UInt32 n;
    if (jobP->cancelled) {
        if (jobP->state != jobIdle) gActive--;
        if (jobP->state == jobDelivering) gCancelled++;
        r->status = netJobCancelled;
        return End(jobPP);
    }
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
    {
        Source *sourceP = jobP->sourceP;
        if (sourceP->pacedSecond != gTicks / 100) {
            sourceP->pacedSecond = gTicks / 100;
            sourceP->pacedBytes = 0;
            sourceP->playingBytes = 0;
        }
        if (Mp4StreamBuffered(sourceP->streamP) >= sourceP->streamP->capacity / 2)
            sourceP->pacedBytes += n;
        else if (gPlaying)
            sourceP->playingBytes += n;
        if (sourceP->pacedBytes > gPace + 2 * NET_JOB_IO_CAPACITY ||
            sourceP->playingBytes > 2 * gPace + 2 * NET_JOB_IO_CAPACITY)
            Fail("paced reads too fast");
    }
    if (jobP->bodyProcP(jobP->contextP, jobP->sourceP->dataP + jobP->next,
            (UInt16)n) != 0)
        Fail("more data than the ring had room for");
    jobP->next += n;
    r->received += n;
    if (!jobP->probe) {
        Source *sourceP = jobP->sourceP;
        sourceP->next += n;
        sourceP->inSegment += n;
        if (gLive &&
            sourceP->next == sourceP->starts[sourceP->sequence - FIRST_SEGMENT + 1]) {
            sourceP->sequence++;
            sourceP->inSegment = 0;
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
void NetJobKeepNumber(NetJob *jobP, const Char *nameP) { (void)jobP; (void)nameP; }
void UdpLogLine(UdpLog *logP, const Char *tagP, const Char *formatP,
                UInt32 a, UInt32 b, UInt32 c)
{
    (void)logP; (void)tagP; (void)formatP; (void)a; (void)b; (void)c;
}

/* Samples of a whole live file, read in one go */
static UInt32 CountSamples(const Source *s)
{
    static UInt8 sample[SAMPLE_CAPACITY];
    Mp4Stream stream;
    Mp4ReadTrack track;
    Mp4Sample out;
    UInt32 count = 0;
    UInt16 result;
    if (Mp4StreamOpen(&stream, s->dataP, s->size, MP4_STREAM_LIVE, &track) !=
            mp4MergeOk)
        Fail("cannot open a stream");
    stream.writeOffset = s->size;
    Mp4StreamFinish(&stream, s->size);
    while ((result = Mp4StreamNext(&stream, &out, sample, sizeof(sample))) ==
           mp4StreamSample)
        count++;
    if (result != mp4StreamEnd) Fail("whole live file unreadable");
    Mp4StreamClose(&stream);
    return count;
}

static int LiveMain(char **argv)
{
    static UInt8 sample[SAMPLE_CAPACITY];
    static YtFormat formats[2];
    static Mp4Stream streams[2];
    static Mp4ReadTrack tracks[2];
    YtClient client;
    NetJobConfig config;
    StreamFeed *f;
    UInt32 loops, taken[2] = { 0, 0 }, want[2], lastDts[2] = { 0, 0 };
    UInt32 seconds = (UInt32)atoi(argv[2]), back;
    Boolean ended[2] = { false, false };
    int i;
    gLive = true;
    gSegmentTicks = seconds * 100;
    srand((unsigned)atoi(argv[5]));
    gSources[0].hostP = "v.example";
    gSources[1].hostP = "a.example";
    for (i = 0; i < 2; i++) {
        UInt32 ringBytes = (UInt32)atoi(argv[3 + i]) * 1024;
        gSources[i].dataP = Load(argv[i], &gSources[i].size);
        if (gSources[i].dataP == 0) Fail("cannot read the inputs");
        FindSegments(&gSources[i]);
        gSources[i].sequence = FIRST_SEGMENT;
        want[i] = CountSamples(&gSources[i]);
        snprintf(formats[i].url, sizeof(formats[i].url),
            "https://%s/videoplayback?id=1", gSources[i].hostP);
        formats[i].bitrate = i == 0 ? 192223 : 144000;
        formats[i].segmentSeconds = (UInt16)seconds;
        gSources[i].streamP = &streams[i];
        if (Mp4StreamOpen(&streams[i], (UInt8 *)malloc(ringBytes), ringBytes,
                MP4_STREAM_LIVE, &tracks[i]) != mp4MergeOk)
            Fail("cannot open a stream");
    }
    /* The feed starts LIVE_BEHIND_SECONDS back: at the first segment. */
    back = (LIVE_BEHIND_SECONDS + seconds - 1) / seconds;
    gInitial = back + 1;
    if (gInitial > gSources[0].count || gInitial > gSources[1].count)
        Fail("too few segments");
    YtClientDefaults(&client);
    MemSet(&config, sizeof(config), 0);
    f = StreamFeedNew(&config, &client, &formats[0], &formats[1], 0, true);
    gPace = f->pace;
    if (StreamFeedLength(f, 0) != MP4_STREAM_LIVE || !StreamFeedLive(f))
        Fail("not live");
    StreamFeedStart(f, &streams[0], &streams[1], false);
    for (loops = 0; !(ended[0] && ended[1]); loops++) {
        Boolean waiting = false;
        if (loops == MAX_LOOPS) Fail("streams never finished");
        gTicks += 2;
        StreamFeedStep(f, gPlaying);
        if (StreamFeedError(f) != 0) Fail(StreamFeedError(f));
        for (i = 0; i < 2; i++)
            if (!ended[i] && (!Mp4StreamReady(&streams[i]) ||
                    (Mp4StreamBuffered(&streams[i]) < 8192 &&
                     !Mp4StreamReceived(&streams[i]))))
                waiting = true;
        gPlaying = !waiting;
        if (waiting) continue;
        for (i = 0; i < 2; i++) {
            Mp4Sample s;
            UInt16 result;
            if (ended[i] || loops % (i == 0 ? 3 : 2) != 0) continue;
            result = Mp4StreamNext(&streams[i], &s, sample, sizeof(sample));
            if (result == mp4StreamSample) {
                if (taken[i] == 0 ? s.dts != 0 : s.dts <= lastDts[i])
                    Fail("times do not start at 0 and grow");
                lastDts[i] = s.dts;
                taken[i]++;
            } else if (result == mp4StreamEnd) {
                ended[i] = true;
            } else if (result == mp4StreamError) {
                Fail("stream error");
            }
        }
    }
    for (i = 0; i < 2; i++) {
        if (gSources[i].next != gSources[i].size) Fail("a stream was cut short");
        if (taken[i] != want[i]) Fail("samples missing");
    }
    if (StreamFeedRate(f, 0) == 0 || StreamFeedRate(f, 1) == 0) Fail("no rate");
    printf("live feed ok: %u + %u samples, %d requests (%d reused, %d dropped, "
        "%d cut, %d cancelled), %d probes, %d held, %d early, rates %u + %u B/s, "
        "%u s\n", (unsigned)taken[0], (unsigned)taken[1], gRequests, gReused,
        gDropped, gCut, gCancelled, gProbes, gHeld, gEarly, (unsigned)StreamFeedRate(f, 0),
        (unsigned)StreamFeedRate(f, 1), (unsigned)(gTicks / 100));
    StreamFeedFree(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 8 && strcmp(argv[1], "--live") == 0) return LiveMain(argv + 2);
    static UInt8 sample[SAMPLE_CAPACITY];
    static YtFormat formats[2];
    static Mp4Stream streams[2];
    static Mp4ReadTrack tracks[2];
    YtClient client;
    NetJobConfig config;
    StreamFeed *f;
    UInt8 *ringsP[2];
    UInt32 loops, taken[2] = { 0, 0 }, ringBytes[2];
    Boolean ended[2] = { false, false };
    int i;
    if (argc != 6) {
        fprintf(stderr, "usage: feed_test VIDEO.mp4 AUDIO.mp4 VIDEO_RING_KB "
            "AUDIO_RING_KB SEED\n");
        return 2;
    }
    srand((unsigned)atoi(argv[5]));
    gSources[0].hostP = "v.example";
    gSources[1].hostP = "a.example";
    for (i = 0; i < 2; i++) {
        gSources[i].dataP = Load(argv[1 + i], &gSources[i].size);
        if (gSources[i].dataP == 0) Fail("cannot read the inputs");
        snprintf(formats[i].url, sizeof(formats[i].url),
            "https://%s/videoplayback?id=1", gSources[i].hostP);
        formats[i].contentLength = gSources[i].size;
        gSources[i].streamP = &streams[i];
        ringBytes[i] = (UInt32)atoi(argv[3 + i]) * 1024;
        ringsP[i] = (UInt8 *)malloc(ringBytes[i]);
        if (Mp4StreamOpen(&streams[i], ringsP[i], ringBytes[i],
                gSources[i].size, &tracks[i]) != mp4MergeOk)
            Fail("cannot open a stream");
    }
    YtClientDefaults(&client);
    MemSet(&config, sizeof(config), 0);
    /* 240p and its sound over 213 s: 36 KB/s, paced a quarter above. */
    {
        YtFormat video = formats[0], audio = formats[1];
        video.contentLength = 4310122UL;
        audio.contentLength = 3449447UL;
        f = StreamFeedNew(&config, &client, &video, &audio, 213, false);
        if (f == 0 || f->pace != 45500UL) Fail("pace from the bitrates");
        StreamFeedFree(f);
    }
    /* Without the length, as here, the pace is the minimum. */
    f = StreamFeedNew(&config, &client, &formats[0], &formats[1], 0, false);
    StreamFeedStart(f, &streams[0], &streams[1], false);
    for (loops = 0; !(ended[0] && ended[1]); loops++) {
        Boolean waiting = false;
        if (loops == MAX_LOOPS) Fail("streams never finished");
        gTicks += 2;
        StreamFeedStep(f, gPlaying);
        if (StreamFeedError(f) != 0) Fail(StreamFeedError(f));
        /* The player waits while either stream has nothing ready. */
        for (i = 0; i < 2; i++)
            if (!ended[i] && (!Mp4StreamReady(&streams[i]) ||
                    (Mp4StreamBuffered(&streams[i]) < 8192 &&
                     !Mp4StreamReceived(&streams[i]))))
                waiting = true;
        if (gSeeks < SEEKS && rand() % 2000 == 0 &&
                streams[0].segmentCount != 0 && streams[1].segmentCount != 0) {
            UInt32 offsets[2];
            for (i = 0; i < 2; i++) {
                UInt16 segment = (UInt16)((UInt32)rand() % streams[i].segmentCount);
                if (!Mp4StreamSeek(&streams[i], segment)) Fail("seek refused");
                offsets[i] = streams[i].segmentsP[segment].offset;
                gSources[i].next = offsets[i];
                ended[i] = false;
            }
            StreamFeedSeek(f, offsets[0], offsets[1]);
            gSeeks++;
            continue;
        }
        gPlaying = !waiting;
        if (waiting) continue;
        for (i = 0; i < 2; i++) {
            Mp4Sample s;
            UInt16 result;
            if (ended[i] || loops % (i == 0 ? 3 : 2) != 0) continue;
            result = Mp4StreamNext(&streams[i], &s, sample, sizeof(sample));
            if (result == mp4StreamSample) taken[i]++;
            else if (result == mp4StreamEnd) ended[i] = true;
            else if (result == mp4StreamError) Fail("stream error");
        }
    }
    for (i = 0; i < 2; i++)
        if (gSources[i].next != gSources[i].size) Fail("a stream was cut short");
    printf("feed ok: %u + %u samples, %d requests (%d reused, %d dropped, "
        "%d closed), %d seeks, rings %u + %u KB\n", (unsigned)taken[0],
        (unsigned)taken[1], gRequests, gReused, gDropped, gClosed, gSeeks,
        (unsigned)(ringBytes[0] / 1024), (unsigned)(ringBytes[1] / 1024));
    StreamFeedFree(f);
    return 0;
}
