/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Runs src/palm/playback.c on the host against a simulated Palm clock.
 * ARM commands go to the host decoder; each advances the clock by what it
 * costs on a Palm TX, measured with PalmView Bench on a 144p video. LOAD
 * scales the video decode cost to stand for harder videos. The sound
 * stream calls SoundCallback as simulated time passes, like the OS would.
 *
 *   playback_sim FILE.mp4 [LOAD] [PAUSE_AT_SECONDS]
 *   playback_sim --stream VIDEO.mp4 AUDIO.mp4 KB_PER_S [OUTAGE_AT SECONDS]
 *       [--seek AT TO]...
 *
 * The second form streams YouTube's two DASH files over a simulated
 * network of the given speed, which can drop out for a while. --seek moves
 * to TO seconds of the video once AT seconds have passed.
 *
 * Checks that every decoded frame is shown or skipped, that all sound is
 * played, that pausing holds the position, and that playback ends. Shows
 * how far each frame was from the sound heard at the time. */

#define _GNU_SOURCE
#include <PalmOS.h>
#include <math.h>
#include <sys/mman.h>

#include "decoder.h"
#include "playback.h"
#include "sound.h"
#include "stream_feed.h"

/* Milliseconds per frame or packet on the device (PalmView Bench). */
#define COST_FULL 24.6
#define COST_DEBLOCK_NONREF 21.8
#define COST_NO_DEBLOCK 18.8
#define COST_SKIPPED_SAMPLE 1.0
#define COST_RGB 3.5
#define COST_READ 0.8
#define COST_AUDIO_PACKET 3.5
#define COST_ARM_CALL 0.05
#define COST_EVENT_LOOP 0.3
/* Guesses for TLS and NetLib, until the TX measures them. */
#define COST_NET_STEP 0.3
#define COST_NET_KB 0.4
/* What the network holds for a connection that is not read: TCP windows. */
#define NET_BACKLOG 32768.0
#define TICKS_PER_SECOND 100

static Playback gPlayback;
static double gNowMs;
static double gLoad = 1.0;
static double gAverageSize;
static UInt8 *gScreenP;

static struct {
    Boolean running;
    UInt32 sampleRate, bufferFrames;
    double nextCallbackMs, lastCallbackMs;
    int16_t buffer[2048 * 2];
} gSound;

static struct {
    UInt32 frames;
    double sum, worst;
} gSync;

static void Advance(double ms)
{
    gNowMs += ms;
    while (gSound.running && gSound.nextCallbackMs <= gNowMs) {
        UInt32 played = SoundGetRing()->played;
        SoundCallback(SoundGetRing(), 0, gSound.buffer, gSound.bufferFrames);
        /* Silence while paused does not move what is heard. */
        if (SoundGetRing()->played != played)
            gSound.lastCallbackMs = gSound.nextCallbackMs;
        gSound.nextCallbackMs += gSound.bufferFrames * 1000.0 / gSound.sampleRate;
    }
}

static void *LowAlloc(size_t size)
{
    void *p = mmap(0, size + 16, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) return 0;
    *(size_t *)p = size + 16;
    return (UInt8 *)p + 16;
}

void SimFree(void *p)
{
    if (p == 0) return;
    if ((uintptr_t)p < 0x80000000UL) {
        UInt8 *base = (UInt8 *)p - 16;
        munmap(base, *(size_t *)base);
    } else {
        free(p);
    }
}

/* Palm OS */

UInt32 TimGetTicks(void) { return (UInt32)(gNowMs * TICKS_PER_SECOND / 1000.0); }
UInt32 SysTicksPerSecond(void) { return TICKS_PER_SECOND; }
Boolean EvtEventAvail(void) { return false; }
Boolean EvtSysEventAvail(Boolean ignorePenUps) { (void)ignorePenUps; return false; }

Err MemHeapFreeBytes(UInt16 heapID, UInt32 *freeP, UInt32 *maxP)
{
    (void)heapID;
    *freeP = 3382UL * 1024;
    *maxP = 1999UL * 1024;
    return errNone;
}

WinHandle WinGetDisplayWindow(void) { return &gScreenP; }
BitmapType *WinGetBitmap(WinHandle windowH) { return (BitmapType *)windowH; }

Err WinScreenGetAttribute(int selector, UInt32 *attrP)
{
    switch (selector) {
        case winScreenDepth: *attrP = 16; return errNone;
        case winScreenDensity: *attrP = 144; return errNone;
        case winScreenPixelFormat: *attrP = pixelFormat565LE; return errNone;
        default: return sysErrParamErr;
    }
}

void *BmpGetBits(BitmapType *bitmapP) { (void)bitmapP; return gScreenP; }

void BmpGetDimensions(const BitmapType *bitmapP, Coord *widthP, Coord *heightP,
                      UInt16 *rowBytesP)
{
    (void)bitmapP;
    *widthP = 320;
    *heightP = 480;
    *rowBytesP = 640;
}

/* The ring and callback are the ones in the ARM module; the stream only
 * schedules the calls. */
Err SndStreamCreate(SndStreamRef *channelP, int mode, UInt32 sampleRate,
                    int type, int width, SndStreamBufferCallback callbackP,
                    void *userdata, UInt32 bufferFrames, Boolean armNative)
{
    (void)mode; (void)type; (void)width; (void)callbackP; (void)userdata;
    (void)armNative;
    *channelP = 1;
    gSound.sampleRate = sampleRate;
    gSound.bufferFrames = bufferFrames;
    gSound.running = false;
    return errNone;
}

Err SndStreamStart(SndStreamRef channel)
{
    (void)channel;
    gSound.running = true;
    gSound.nextCallbackMs = gNowMs;
    Advance(0);
    return errNone;
}

Err SndStreamStop(SndStreamRef channel) { (void)channel; gSound.running = false; return errNone; }
Err SndStreamDelete(SndStreamRef channel) { (void)channel; return errNone; }
Err SndStreamSetVolume(SndStreamRef channel, Int32 volume) { (void)channel; (void)volume; return errNone; }

/* Store */

Err StoreOpenVolume(UInt16 *volumeP) { *volumeP = 1; return errNone; }

Err StoreOpenRead(UInt16 volume, const Char *pathP, FileRef *fileP, UInt32 *sizeP)
{
    (void)volume;
    *fileP = fopen(pathP, "rb");
    if (*fileP == 0) return 1;
    fseek(*fileP, 0, SEEK_END);
    *sizeP = (UInt32)ftell(*fileP);
    return errNone;
}

Err StoreMp4Read(void *fileP, UInt32 offset, void *bufferP, UInt32 length)
{
    Advance(COST_READ);
    if (fseek((FILE *)fileP, (long)offset, SEEK_SET) != 0) return 1;
    return fread(bufferP, 1, length, (FILE *)fileP) == length ? errNone : 1;
}

void VFSFileClose(FileRef file) { fclose(file); }

/* Stream feed: the network brings bytesPerMs, shared by the streams that
 * can take data, except during the outage. */

struct StreamFeed {
    FILE *files[2];
    UInt32 lengths[2];
    UInt32 offsets[2];
    Mp4Stream *streams[2];
    double bytesPerMs;
    double backlog[2];
    double lastMs;
    double outageStart, outageEnd;
    UInt32 received;
};

static StreamFeed gFeed;

UInt32 StreamFeedLength(const StreamFeed *f, UInt16 track) { return f->lengths[track]; }
const Char *StreamFeedError(const StreamFeed *f) { (void)f; return 0; }
UInt32 StreamFeedReceived(const StreamFeed *f) { return f->received; }
void StreamFeedStop(StreamFeed *f) { (void)f; }

void StreamFeedSeek(StreamFeed *f, UInt32 videoOffset, UInt32 audioOffset)
{
    f->offsets[0] = videoOffset;
    f->offsets[1] = audioOffset;
    f->backlog[0] = f->backlog[1] = 0;
}

void StreamFeedStart(StreamFeed *f, Mp4Stream *videoP, Mp4Stream *audioP)
{
    f->streams[0] = videoP;
    f->streams[1] = audioP;
    f->offsets[0] = f->offsets[1] = 0;
    f->backlog[0] = f->backlog[1] = 0;
    f->lastMs = gNowMs;
}

void StreamFeedStep(StreamFeed *f, Boolean playing)
{
    static UInt8 buffer[4096];
    double arrived = 0;
    Boolean taking[2];
    UInt16 i;
    (void)playing;
    if (gNowMs < f->outageStart || gNowMs >= f->outageEnd)
        arrived = (gNowMs - f->lastMs) * f->bytesPerMs;
    f->lastMs = gNowMs;
    Advance(COST_NET_STEP);
    /* A stream that is complete, or not read with its backlog full,
     * leaves its share to the other. */
    for (i = 0; i < 2; i++)
        taking[i] = f->offsets[i] < f->lengths[i] &&
            (f->backlog[i] < NET_BACKLOG || Mp4StreamSpace(f->streams[i]) != 0);
    for (i = 0; i < 2; i++) {
        UInt32 n, space;
        if (taking[i]) f->backlog[i] += taking[1 - i] ? arrived / 2 : arrived;
        if (f->backlog[i] > NET_BACKLOG) f->backlog[i] = NET_BACKLOG;
        n = (UInt32)f->backlog[i];
        space = Mp4StreamSpace(f->streams[i]);
        if (n > space) n = space;
        if (n > f->lengths[i] - f->offsets[i]) n = f->lengths[i] - f->offsets[i];
        while (n != 0) {
            UInt32 chunk = n < sizeof(buffer) ? n : sizeof(buffer);
            if (fseek(f->files[i], (long)f->offsets[i], SEEK_SET) != 0 ||
                fread(buffer, 1, chunk, f->files[i]) != chunk)
                return;
            Mp4StreamWrite(f->streams[i], buffer, chunk);
            Advance(COST_NET_KB * chunk / 1024.0);
            f->offsets[i] += chunk;
            f->received += chunk;
            f->backlog[i] -= chunk;
            n -= chunk;
        }
    }
}

/* ARM module */

void *ArmLargeAlloc(UInt32 size) { return LowAlloc(size); }

Err ArmModuleLoad(ArmModule *m)
{
    m->callP = (ArmCall *)calloc(1, sizeof(ArmCall));
    m->stackP = (UInt8 *)LowAlloc(ARM_STACK_SIZE);
    return m->callP != 0 && m->stackP != 0 ? errNone : memErrNotEnoughSpace;
}

void ArmModuleUnload(ArmModule *m)
{
    free(m->callP);
    SimFree(m->stackP);
    memset(m, 0, sizeof(*m));
}

static double DecodeCost(UInt32 flags, UInt32 size, int frames)
{
    double cost = (flags & DEC_SKIP_DEBLOCK) ? COST_NO_DEBLOCK
        : (flags & DEC_SKIP_DEBLOCK_NONREF) ? COST_DEBLOCK_NONREF : COST_FULL;
    if ((flags & DEC_SKIP_NONREF) && frames == 0) return COST_SKIPPED_SAMPLE;
    gAverageSize = gAverageSize == 0 ? size : gAverageSize * 0.95 + size * 0.05;
    /* Bigger samples (keyframes) take longer. */
    return cost * gLoad * (0.6 + 0.4 * size / gAverageSize);
}

/* How far the frame about to be drawn is from the sound being heard. */
static void MeasureSync(void)
{
    const SoundRing *r = SoundGetRing();
    double frameMs, heardMs, period, error;
    uint32_t pts;
    if (!gPlayback.soundStarted || DecVideoQueued(&pts) == 0) return;
    frameMs = (pts - gPlayback.firstPts) * 1000.0 / gPlayback.timescale;
    period = gSound.bufferFrames * 1000.0 / gSound.sampleRate;
    heardMs = gPlayback.seekMs +
        (r->played > r->lastRequest ? r->played - r->lastRequest : 0) *
        1000.0 / gSound.sampleRate + fmin(gNowMs - gSound.lastCallbackMs, period);
    error = frameMs - heardMs;
    gSync.frames++;
    gSync.sum += error;
    if (fabs(error) > fabs(gSync.worst)) gSync.worst = error;
}

static void SetSoundOut(ArmCall *c)
{
    const SoundRing *r = SoundGetRing();
    DecAudioInfo info;
    DecAudioGetInfo(&info);
    c->out[0] = r->written - r->read;
    c->out[1] = r->played;
    c->out[2] = r->underruns;
    c->out[3] = r->lastRequest;
    c->out[4] = info.sampleRate;
}

Int32 ArmModuleCall(ArmModule *m, UInt32 command, const UInt32 *argsP,
                    UInt16 argCount)
{
    static int16_t pcm[2048 * 2];
    uint32_t pts = 0;
    Int32 result = 0;
    (void)argCount;
    Advance(COST_ARM_CALL);
    switch (command) {
        case armCmdInit:
            m->callP->out[0] = ARM_MODULE_VERSION;
            return 0;
        case armCmdVideoOpen:
            return DecVideoOpen((const uint8_t *)(uintptr_t)argsP[0], argsP[1], argsP[2]);
        case armCmdVideoDecode:
            result = DecVideoDecode((const uint8_t *)(uintptr_t)argsP[0], argsP[1],
                argsP[2], argsP[3]);
            Advance(argsP[0] != 0 ? DecodeCost(argsP[3], argsP[1], result) : 1.0);
            break;
        case armCmdVideoPop:
            DecVideoPop();
            break;
        case armCmdVideoRgb:
            MeasureSync();
            Advance(COST_RGB);
            return DecVideoToRgb565((uint8_t *)(uintptr_t)argsP[0], argsP[1], argsP[2],
                argsP[3], argsP[4]);
        case armCmdAudioOpen:
            return DecAudioOpen((const uint8_t *)(uintptr_t)argsP[0], argsP[1]);
        case armCmdSoundOpen: {
            DecAudioInfo info;
            DecAudioGetInfo(&info);
            result = SoundOpen(argsP[0], info.channels > 2 ? 2 : info.channels);
            m->callP->out[0] = 1;
            m->callP->out[1] = 1;
            m->callP->out[2] = SoundGetRing()->channels;
            m->callP->out[3] = info.sampleRate != 0 ? info.sampleRate : argsP[1];
            return result;
        }
        case armCmdSoundFeed:
            result = DecAudioDecode((const uint8_t *)(uintptr_t)argsP[0], argsP[1], pcm,
                argsP[2]);
            Advance(COST_AUDIO_PACKET);
            if (result > 0) SoundFeed(pcm, (uint32_t)result);
            SetSoundOut(m->callP);
            return result;
        case armCmdSoundState:
            SetSoundOut(m->callP);
            return 0;
        case armCmdSoundEnd:
            SoundGetRing()->ended = 1;
            return 0;
        case armCmdSoundPause:
            SoundGetRing()->paused = argsP[0] != 0;
            return 0;
        case armCmdClose:
            DecVideoClose();
            DecAudioClose();
            SoundClose();
            return 0;
        default:
            return decErrState;
    }
    m->callP->out[0] = DecVideoQueued(&pts);
    m->callP->out[1] = pts;
    return result;
}

/* Simulation */

static void PrintClock(UInt32 ms)
{
    printf("%u:%02u.%u", (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60),
        (unsigned)(ms / 100 % 10));
}

/* Opens the two DASH files for the simulated network. */
static const Char *OpenFeed(char **argv, int argc)
{
    UInt16 i;
    for (i = 0; i < 2; i++) {
        gFeed.files[i] = fopen(argv[2 + i], "rb");
        if (gFeed.files[i] == 0) return "Could not open the stream files.";
        fseek(gFeed.files[i], 0, SEEK_END);
        gFeed.lengths[i] = (UInt32)ftell(gFeed.files[i]);
    }
    gFeed.bytesPerMs = atof(argv[4]) * 1024.0 / 1000.0;
    gFeed.outageStart = argc > 6 ? atof(argv[5]) * 1000.0 : -1;
    gFeed.outageEnd = argc > 6 ? gFeed.outageStart + atof(argv[6]) * 1000.0 : -1;
    return PlaybackOpenStream(&gPlayback, &gFeed);
}

#define MAX_SEEKS 4

int main(int argc, char **argv)
{
    Playback *p = &gPlayback;
    double seekAt[MAX_SEEKS], seekTo[MAX_SEEKS];
    int seeks = 0, seeksDone = 0;
    RectangleType bounds = { { 0, 0 }, { 160, 90 } };
    Boolean streaming = argc > 1 && strcmp(argv[1], "--stream") == 0;
    int pauseArg = streaming ? 7 : 3;
    double pauseAtMs = argc > pauseArg ? atof(argv[pauseArg]) * 1000.0 : -1;
    double pausedFor = 0, nextReport = 1000, waitedMs = 0, seekWaitMs = 0;
    const Char *errorP;
    DecVideoInfo video;
    DecAudioInfo audio;
    UInt32 position, played = 0;
    UInt16 level = 0;
    int failed = 0;
    while (argc > 3 && strcmp(argv[argc - 3], "--seek") == 0 && seeks < MAX_SEEKS) {
        seekAt[seeks] = atof(argv[argc - 2]) * 1000.0;
        seekTo[seeks] = atof(argv[argc - 1]) * 1000.0;
        seeks++;
        argc -= 3;
    }
    if (argc < 2 || (streaming && argc < 5)) {
        fprintf(stderr, "usage: playback_sim FILE.mp4 [LOAD] [PAUSE_AT_SECONDS]\n"
            "       playback_sim --stream VIDEO.mp4 AUDIO.mp4 KB_PER_S "
            "[OUTAGE_AT SECONDS [PAUSE_AT_SECONDS]] [--seek AT TO]...\n");
        return 2;
    }
    if (!streaming && argc > 2) gLoad = atof(argv[2]);
    gScreenP = (UInt8 *)LowAlloc(640 * 480);
    errorP = streaming ? OpenFeed(argv, argc) : PlaybackOpen(p, argv[1]);
    if (errorP == 0) errorP = PlaybackAttachScreen(p, &bounds);
    if (errorP != 0) {
        fprintf(stderr, "playback: %s\n", errorP);
        return 1;
    }
    if (streaming) {
        printf("streaming at %s KB/s, rings %u + %u KB\n", argv[4],
            (unsigned)(p->ringSizes[0] / 1024), (unsigned)(p->ringSizes[1] / 1024));
    } else {
        printf("output %ux%u, duration ", p->width, p->height);
        PrintClock(PlaybackDurationMs(p));
        printf(", load %.2f, sound %s\n", gLoad, p->soundOn ? "on" : p->soundError);
    }
    PlaybackPlay(p);
    for (;;) {
        Int32 wait = PlaybackStep(p);
        double before = gNowMs;
        Advance(COST_EVENT_LOOP);
        if (wait == evtWaitForever) break;
        Advance(wait * 1000.0 / TICKS_PER_SECOND);
        if (streaming && (p->buffering || !p->started)) {
            waitedMs += gNowMs - before;
            seekWaitMs += gNowMs - before;
        } else if (seekWaitMs != 0) {
            printf("  playing again after %.1f s\n", seekWaitMs / 1000);
            seekWaitMs = 0;
        }
        if (pauseAtMs >= 0 && gNowMs >= pauseAtMs) {
            pauseAtMs = -1;
            PlaybackPause(p);
            position = PlaybackPositionMs(p);
            Advance(3000);
            pausedFor = 3000;
            if (PlaybackPositionMs(p) != position) {
                printf("FAIL: position moved while paused\n");
                failed = 1;
            }
            PlaybackPlay(p);
            printf("paused 3 s at ");
            PrintClock(position);
            printf("\n");
        }
        /* In command-line order: they were read from the end. */
        if (seeksDone < seeks && gNowMs >= seekAt[seeks - 1 - seeksDone]) {
            UInt32 from = PlaybackPositionMs(p), to;
            if (!PlaybackCanSeek(p)) {
                printf("FAIL: cannot seek at ");
                PrintClock(from);
                printf("\n");
                failed = 1;
            }
            to = PlaybackSeek(p, (UInt32)seekTo[seeks - 1 - seeksDone]);
            seeksDone++;
            seekWaitMs = 0.001;
            printf("seek from ");
            PrintClock(from);
            printf(" to ");
            PrintClock(to);
            printf("\n");
            if (to > seekTo[seeks - seeksDone] ||
                PlaybackPositionMs(p) != to) {
                printf("FAIL: seek landed after the time asked for\n");
                failed = 1;
            }
        }
        if (p->level != level) {
            printf("level %u -> %u at ", level, p->level);
            PrintClock(PlaybackPositionMs(p));
            printf("\n");
            level = p->level;
        }
        if (gNowMs >= nextReport) {
            nextReport += gLoad <= 1.0 ? 10000 : 5000;
            printf("t=%6.1f s  pos ", gNowMs / 1000);
            PrintClock(PlaybackPositionMs(p));
            printf("  %2u.%u fps  cpu %3u%%  level %u  skipped %u  queued %u  "
                "sound %u ms, %u gaps", p->fps10 / 10, p->fps10 % 10,
                p->cpuPercent, p->level, (unsigned)p->skipped, (unsigned)p->queued,
                p->sampleRate ? (unsigned)(p->soundBuffered * 1000 / p->sampleRate) : 0,
                (unsigned)p->soundUnderruns);
            if (streaming)
                printf("  net %u%%, buffered %d + %d s", p->netPercent,
                    (int)PlaybackBufferedMs(p, 0) / 1000,
                    (int)PlaybackBufferedMs(p, 1) / 1000);
            printf("\n");
        }
        played = SoundGetRing()->played;
    }
    DecVideoGetInfo(&video);
    DecAudioGetInfo(&audio);
    printf("ended %d after %.1f s: %u frames decoded, %u shown, %u skipped, "
        "%u catch-ups, %u errors\n", p->ended, gNowMs / 1000, (unsigned)video.frames,
        (unsigned)p->shown, (unsigned)p->skipped, (unsigned)p->catchUps,
        (unsigned)(p->decodeErrors + p->audioErrors));
    printf("sound: %u of %u frames played, %u gaps; picture vs. sound: "
        "%+.0f ms average, %+.0f ms worst\n", (unsigned)played,
        (unsigned)audio.samples, (unsigned)p->soundUnderruns,
        gSync.frames ? gSync.sum / gSync.frames : 0.0, gSync.worst);
    if (streaming)
        printf("streaming: %u waits, %.1f s waiting, output %ux%u\n",
            (unsigned)p->bufferings, waitedMs / 1000, p->width, p->height);
    printf("clock drift vs. duration: %+.1f s\n",
        (gNowMs - pausedFor - waitedMs) / 1000 - PlaybackDurationMs(p) / 1000.0);
    if (!p->ended || p->decodeErrors != 0 || p->audioErrors != 0 ||
        (p->catchUps == 0 && p->seeks == 0 &&
         p->shown + p->skipped != video.frames)) {
        printf("FAIL: frames not accounted for\n");
        failed = 1;
    }
    if (audio.samples != 0 && played < audio.samples) {
        printf("FAIL: not all sound was played\n");
        failed = 1;
    }
    PlaybackClose(p);
    return failed;
}
