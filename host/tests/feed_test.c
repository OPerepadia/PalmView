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
 *   feed_test VIDEO.mp4 AUDIO.mp4 VIDEO_RING_KB AUDIO_RING_KB SEED
 */

#include <PalmOS.h>

static UInt32 gTicks;
UInt32 TimGetTicks(void) { return gTicks; }
UInt16 SysTicksPerSecond(void) { return 100; }

#include "../../src/palm/stream_feed.c"

#define MAX_LOOPS 2000000UL
#define SAMPLE_CAPACITY 65536
#define SEEKS 4

typedef struct Source {
    const char *hostP;
    UInt8 *dataP;
    UInt32 size;
    UInt32 next;            /* the next byte the feed should ask for */
    Mp4Stream *streamP;
    UInt32 pacedSecond;     /* gTicks / 100 of the bytes counted below */
    UInt32 pacedBytes;      /* read in that second with the ring half full */
    UInt32 playingBytes;    /* and with it below half while playing */
} Source;

enum { jobConnecting, jobSending, jobDelivering, jobIdle, jobDead };

struct NetJob {
    Source *sourceP;
    UInt32 next, end;
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

/* Reads the range a request asks for, and checks it continues the file. */
static void Begin(NetJob *jobP, const Char *requestP)
{
    const char *rangeP = strstr(requestP, "range=");
    char *endP;
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
            if (--jobP->wait <= 0) jobP->state = jobDelivering;
            return netJobRunning;
        default:
            break;
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
        if (sourceP->pacedBytes > PACE_BYTES_PER_SECOND + 2 * NET_JOB_IO_CAPACITY ||
            sourceP->playingBytes > 2 * PACE_BYTES_PER_SECOND + 2 * NET_JOB_IO_CAPACITY)
            Fail("paced reads too fast");
    }
    if (jobP->bodyProcP(jobP->contextP, jobP->sourceP->dataP + jobP->next,
            (UInt16)n) != 0)
        Fail("more data than the ring had room for");
    jobP->next += n;
    jobP->sourceP->next += n;
    r->received += n;
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

int main(int argc, char **argv)
{
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
    f = StreamFeedNew(&config, &client, &formats[0], &formats[1]);
    StreamFeedStart(f, &streams[0], &streams[1]);
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
