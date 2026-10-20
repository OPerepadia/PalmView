/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "playback.h"

#include "decoder.h"
#include "stream_feed.h"

#define SAMPLE_CAPACITY 65536UL
/* Left in the dynamic heap for the OS and the UI. */
#define HEAP_RESERVE (256UL * 1024)
/* 144p video with three queued frames, AAC and the sound ring peak near
 * 1.55 MB. Without sound, 1.1 MB is enough. */
#define ARENA_LIMIT (1792UL * 1024)
#define ARENA_MINIMUM (1152UL * 1024)
#define QUEUE_DEPTH 3

/* 0.74 s at 44.1 kHz, enough to cover a slow keyframe; half that if memory
 * is short. The callback asks for SOUND_BUFFER_FRAMES at a time; one AAC
 * packet is at most MAX_PACKET_FRAMES. */
#define SOUND_RING_FRAMES 32768UL
#define SOUND_RING_MINIMUM 16384UL
#define SOUND_BUFFER_FRAMES 2048UL
#define MAX_PACKET_FRAMES 2048UL
#define SOUND_PREFILL_FRAMES 8192UL
/* 1024 is unity gain. */
#define SOUND_VOLUME 256

/* A frame shown later than LATE_MS counts as late. Frames later than
 * SKIP_MS are not drawn, but never more than MAX_SKIPPED in a row. */
#define LATE_MS 50
#define SKIP_MS 120
#define MAX_SKIPPED 4
/* Without sound, the clock waits for video further behind than this. With
 * sound, video skips to the next keyframe instead. */
#define RESYNC_MS 1000
#define CATCH_UP_MS 1500
#define CATCH_UP_SAMPLES_PER_STEP 64
/* The clock follows the sound gently, and jumps when far off. */
#define CLOCK_SNAP_MS 300
/* Quality drops a level when over a tenth of a second's frames are late
 * and decoding kept the CPU BUSY_CPU_PERCENT busy. Late frames with time
 * to spare come from stalls that lower quality would not fix. Quality goes
 * up a level after CALM_WINDOWS calm seconds in a row: below
 * CALM_CPU_PERCENT, and at most a tenth late. If it drops again within
 * LEVEL_HOLD_SECONDS, twice as many are needed. */
#define BUSY_CPU_PERCENT 80
#define CALM_WINDOWS 3
#define MAX_CALM_WINDOWS 48
#define CALM_CPU_PERCENT 70
#define LEVEL_HOLD_SECONDS 10
/* PlaybackStep keeps working this long unless an event is waiting: each
 * turn of the event loop costs time under PACE. */
#define STEP_SLICE_MS 50

/* Streaming. The rings share what memory is left, by bitrate, keeping
 * STREAM_RESERVE for TLS and the OS. A ring holds at least a keyframe. */
#define STREAM_BUFFER_LIMIT (512UL * 1024)
#define STREAM_RESERVE (320UL * 1024)
#define STREAM_RING_MINIMUM (48UL * 1024)
/* Playback starts with this much of both streams in. When either runs
 * dry, the clock waits until both have RESUME_BUFFER_MS, or full rings. */
#define START_BUFFER_MS 3000UL
#define RESUME_BUFFER_MS 5000UL
#define SOUND_LOW_FRAMES 8192UL
/* How often the network is served while playing, and while paused. */
#define FEED_INTERVAL_MS 20
#define PAUSED_WAIT_MS 100

/* Quality levels: no deblocking in frames nothing references, then none
 * at all, then those frames are not decoded at all (half the frame rate
 * on most YouTube videos). */
static const UInt32 kLevelFlags[PLAYBACK_LEVELS] = {
    0,
    DEC_SKIP_DEBLOCK_NONREF,
    DEC_SKIP_DEBLOCK,
    DEC_SKIP_DEBLOCK | DEC_SKIP_NONREF
};

/* Time */

static UInt32 TicksToMs(UInt32 ticks)
{
    UInt32 perSecond = SysTicksPerSecond();
    return ticks / perSecond * 1000UL + ticks % perSecond * 1000UL / perSecond;
}

static UInt32 MsToTicks(UInt32 ms)
{
    UInt32 perSecond = SysTicksPerSecond();
    return ms / 1000UL * perSecond + ms % 1000UL * perSecond / 1000UL;
}

static UInt32 ScaleToMs(UInt32 value, UInt32 scale)
{
    return value / scale * 1000UL + value % scale * 1000UL / scale;
}

static UInt32 FrameMs(const Playback *p, UInt32 pts)
{
    return pts > p->firstPts ? ScaleToMs(pts - p->firstPts, p->timescale) : 0;
}

static UInt32 RawClockMs(const Playback *p)
{
    return TicksToMs(TimGetTicks() - p->clockBase);
}

static UInt32 VideoMs(const Playback *p)
{
    Int32 ms;
    if (!p->playing || !p->started || p->buffering) return p->pausedMs;
    ms = (Int32)RawClockMs(p) + p->clockOffsetMs;
    return ms > 0 ? (UInt32)ms : 0;
}

static void SetClock(Playback *p, UInt32 ms)
{
    p->clockBase = TimGetTicks();
    p->clockOffsetMs = (Int32)ms;
}

/* ARM calls */

static Int32 Call(Playback *p, UInt32 command, UInt32 a0, UInt32 a1, UInt32 a2,
                  UInt32 a3)
{
    UInt32 args[4];
    args[0] = a0;
    args[1] = a1;
    args[2] = a2;
    args[3] = a3;
    return ArmModuleCall(&p->module, command, args, 4);
}

static void ReadQueue(Playback *p)
{
    p->queued = p->module.callP->out[0];
    p->frontPts = p->module.callP->out[1];
}

static void ReadSound(Playback *p)
{
    p->soundBuffered = p->module.callP->out[0];
    p->soundPlayed = p->module.callP->out[1];
    p->soundUnderruns = p->module.callP->out[2];
    p->soundLastRequest = p->module.callP->out[3];
    p->decoderRate = p->module.callP->out[4];
}

static Boolean ReadSample(Playback *p, const Mp4Sample *s)
{
    if (s->size + DEC_PADDING > SAMPLE_CAPACITY ||
        StoreMp4Read((void *)p->file, s->offset, p->sampleP, s->size) != errNone)
        return false;
    MemSet(p->sampleP + s->size, DEC_PADDING, 0);
    return true;
}

enum { sampleReady = 0, sampleWait, sampleEnd, sampleFailed };

/* The next sample of a track, in sampleP. Files skip reading it without
 * data; streams copy every sample out. */
static UInt16 NextSample(Playback *p, UInt16 track, Mp4Sample *sP, Boolean data)
{
    UInt16 result;
    if (p->feedP == 0) {
        if (!Mp4ReaderNext(p->readerP, track == STREAM_FEED_VIDEO
                ? &p->readerP->video : &p->readerP->audio, sP))
            return sampleEnd;
        return !data || ReadSample(p, sP) ? sampleReady : sampleFailed;
    }
    result = Mp4StreamNext(&p->streams[track], sP, p->sampleP,
        SAMPLE_CAPACITY - DEC_PADDING);
    if (result == mp4StreamSample) {
        MemSet(p->sampleP + sP->size, DEC_PADDING, 0);
        return sampleReady;
    }
    if (result == mp4StreamWait) return sampleWait;
    if (result == mp4StreamEnd) return sampleEnd;
    p->errorP = "The stream could not be read.";
    return sampleFailed;
}

/* Sound */

static Boolean FeedSound(Playback *p);

/* Failing here leaves the video playing without sound. */
static void OpenSound(Playback *p)
{
    Mp4ReadTrack *t = &p->readerP->audio;
    SndStreamBufferCallback callbackP;
    void *ringP;
    UInt32 channels;
    p->soundError = 0;
    if (!t->present || t->configLength == 0) {
        p->soundError = "the file has no AAC audio.";
        return;
    }
    MemMove(p->sampleP, t->config, t->configLength);
    if (Call(p, armCmdAudioOpen, (UInt32)p->sampleP, t->configLength, 0, 0) != 0) {
        p->soundError = "the audio decoder did not start.";
        return;
    }
    p->ringFrames = SOUND_RING_FRAMES;
    while (Call(p, armCmdSoundOpen, p->ringFrames, t->sampleRate, 0, 0) != 0) {
        p->ringFrames /= 2;
        if (p->ringFrames < SOUND_RING_MINIMUM) {
            p->soundError = "not enough memory.";
            return;
        }
    }
    callbackP = (SndStreamBufferCallback)p->module.callP->out[0];
    ringP = (void *)p->module.callP->out[1];
    channels = p->module.callP->out[2];
    p->sampleRate = p->module.callP->out[3];
    p->ringOpen = true;
    /* The file's rate can differ from the decoder's (HE-AAC without SBR
     * plays at half rate), so decode a packet first. */
    FeedSound(p);
    if (p->decoderRate != 0) p->sampleRate = p->decoderRate;
    if (p->sampleRate == 0 ||
        SndStreamCreate(&p->stream, sndOutput, p->sampleRate, sndInt16Little,
            channels == 1 ? sndMono : sndStereo, callbackP, ringP,
            SOUND_BUFFER_FRAMES, true) != errNone) {
        p->soundError = "the sound output did not open.";
        p->ringOpen = false;
        return;
    }
    SndStreamSetVolume(p->stream, SOUND_VOLUME);
    p->soundOn = true;
}

static void CloseSound(Playback *p)
{
    p->ringOpen = false;
    if (!p->soundOn) return;
    SndStreamStop(p->stream);
    SndStreamDelete(p->stream);
    p->soundOn = false;
    p->soundStarted = false;
}

/* Decodes one AAC packet when the ring has room. Returns true if it did
 * any work. */
static Boolean FeedSound(Playback *p)
{
    UInt32 start;
    UInt16 result;
    Mp4Sample s;
    if (!p->ringOpen || p->soundInputEnded ||
        p->soundBuffered + MAX_PACKET_FRAMES > p->ringFrames)
        return false;
    start = TimGetTicks();
    result = NextSample(p, STREAM_FEED_AUDIO, &s, true);
    p->audioWaiting = result == sampleWait;
    if (result == sampleWait) return false;
    if (result == sampleEnd) {
        p->soundInputEnded = true;
        Call(p, armCmdSoundEnd, 0, 0, 0, 0);
        return true;
    }
    if (result != sampleReady ||
        Call(p, armCmdSoundFeed, (UInt32)p->sampleP, s.size, 0, 0) < 0)
        p->audioErrors++;
    else
        ReadSound(p);
    p->windowBusy += TimGetTicks() - start;
    return true;
}

/* Moves the clock toward the sound actually played. The OS asks for the
 * next buffer as it starts playing the last one, so that one is not heard
 * yet. */
static void FollowSound(Playback *p)
{
    UInt32 played, audioMs;
    Int32 diff;
    Call(p, armCmdSoundState, 0, 0, 0, 0);
    ReadSound(p);
    if (p->soundPlayed == p->lastPlayed) return;
    p->lastPlayed = p->soundPlayed;
    if (p->soundLastRequest > p->latencyFrames) p->latencyFrames = p->soundLastRequest;
    played = p->soundPlayed > p->latencyFrames ? p->soundPlayed - p->latencyFrames : 0;
    audioMs = ScaleToMs(played, p->sampleRate);
    diff = (Int32)audioMs - (Int32)VideoMs(p);
    if (diff < p->windowDiffMin) p->windowDiffMin = diff;
    if (diff > p->windowDiffMax) p->windowDiffMax = diff;
    if (diff > CLOCK_SNAP_MS || diff < -CLOCK_SNAP_MS) {
        p->clockOffsetMs += diff;
        p->snaps++;
    } else {
        p->clockOffsetMs += diff / 4;
    }
}

/* Decoders */

/* Streams fill in the reader's tracks themselves. */
static const Char *StartDecoders(Playback *p)
{
    Mp4ReadTrack *t = &p->readerP->video;
    if (p->feedP == 0 &&
        Mp4ReaderOpen(p->readerP, StoreMp4Read, (void *)p->file, p->fileSize) !=
            mp4MergeOk)
        return "This is not an MP4 file PalmView can read.";
    if (!t->present || t->configLength == 0 || t->timescale == 0)
        return "The file has no H.264 video.";
    MemMove(p->sampleP, t->config, t->configLength);
    if (Call(p, armCmdVideoOpen, (UInt32)p->sampleP, t->configLength,
            QUEUE_DEPTH, 0) != 0)
        return "The video decoder did not start.";
    p->decoderOpen = true;
    p->timescale = t->timescale;
    p->started = false;
    p->ended = false;
    p->pausedMs = 0;
    p->queued = 0;
    p->draining = false;
    p->inputEnded = false;
    p->catchingUp = false;
    p->decodeMs = 0;
    p->level = 0;
    p->skippedInRow = 0;
    p->calmWindows = 0;
    p->calmNeeded = CALM_WINDOWS;
    p->lastLevelDown = 0;
    p->settling = false;
    p->soundInputEnded = false;
    p->soundBuffered = 0;
    p->soundPlayed = 0;
    p->lastPlayed = 0;
    p->latencyFrames = 0;
    p->decoderRate = 0;
    p->ringOpen = false;
    OpenSound(p);
    return 0;
}

static void StopDecoders(Playback *p)
{
    /* The callback reads the ring that armCmdClose frees. */
    CloseSound(p);
    if (p->decoderOpen) Call(p, armCmdClose, 0, 0, 0, 0);
    p->decoderOpen = false;
    p->ringOpen = false;
}

static const Char *LoadDecoder(Playback *p)
{
    UInt32 freeBytes = 0, maxChunk = 0, arenaSize;
    p->readerP = (Mp4Reader *)MemPtrNew(sizeof(Mp4Reader));
    p->sampleP = (UInt8 *)ArmLargeAlloc(SAMPLE_CAPACITY);
    if (p->readerP == 0 || p->sampleP == 0) return "Out of memory.";
    MemSet(p->readerP, sizeof(Mp4Reader), 0);
    if (ArmModuleLoad(&p->module) != errNone) return "Could not load the decoder.";
    p->moduleLoaded = true;
    MemHeapFreeBytes(0, &freeBytes, &maxChunk);
    arenaSize = maxChunk > HEAP_RESERVE ? maxChunk - HEAP_RESERVE : 0;
    if (arenaSize > ARENA_LIMIT) arenaSize = ARENA_LIMIT;
    if (arenaSize < ARENA_MINIMUM)
        return "Not enough free memory. Close other apps and try again.";
    p->arenaP = (UInt8 *)ArmLargeAlloc(arenaSize);
    if (p->arenaP == 0) return "Not enough free memory.";
    if (Call(p, armCmdInit, (UInt32)p->arenaP, arenaSize,
            (UInt32)p->module.stackP, ARM_STACK_SIZE) != 0 ||
        p->module.callP->out[0] != ARM_MODULE_VERSION)
        return "The decoder did not start.";
    return 0;
}

const Char *PlaybackOpen(Playback *p, const Char *pathP)
{
    const Char *errorP;
    MemSet(p, sizeof(*p), 0);
    if (StoreOpenVolume(&p->volume) != errNone) return "No memory card found.";
    if (StoreOpenRead(p->volume, pathP, &p->file, &p->fileSize) != errNone)
        return "Could not open the file.";
    p->fileOpen = true;
    errorP = LoadDecoder(p);
    return errorP != 0 ? errorP : StartDecoders(p);
}

/* Streams */

static void OpenStreams(Playback *p)
{
    UInt16 i;
    for (i = 0; i < 2; i++) {
        Mp4StreamClose(&p->streams[i]);
        if (Mp4StreamOpen(&p->streams[i], p->ringsP[i],
                p->ringSizes[i], StreamFeedLength(p->feedP, i),
                i == STREAM_FEED_VIDEO ? &p->readerP->video
                    : &p->readerP->audio) != mp4MergeOk)
            p->errorP = "Out of memory.";
    }
    p->buffering = false;
    p->videoWaiting = false;
    p->audioWaiting = false;
    StreamFeedStart(p->feedP, &p->streams[STREAM_FEED_VIDEO],
        &p->streams[STREAM_FEED_AUDIO]);
}

/* Shares the memory left between the two rings, by bitrate. */
static const Char *AllocateRings(Playback *p)
{
    UInt32 freeBytes = 0, maxChunk = 0, budget, total;
    UInt16 i;
    MemHeapFreeBytes(0, &freeBytes, &maxChunk);
    budget = freeBytes > STREAM_RESERVE ? freeBytes - STREAM_RESERVE : 0;
    if (budget > STREAM_BUFFER_LIMIT) budget = STREAM_BUFFER_LIMIT;
    total = StreamFeedLength(p->feedP, STREAM_FEED_VIDEO) / 1024 +
        StreamFeedLength(p->feedP, STREAM_FEED_AUDIO) / 1024 + 1;
    for (i = 0; i < 2; i++) {
        UInt32 size = budget / 1024 * (StreamFeedLength(p->feedP, i) / 1024) /
            total * 1024;
        if (size < STREAM_RING_MINIMUM) size = STREAM_RING_MINIMUM;
        while ((p->ringsP[i] = (UInt8 *)ArmLargeAlloc(size)) == 0) {
            size -= size / 4;
            if (size < STREAM_RING_MINIMUM)
                return "Not enough free memory to stream. Close other apps "
                    "and try again.";
        }
        p->ringSizes[i] = size;
    }
    return 0;
}

const Char *PlaybackOpenStream(Playback *p, struct StreamFeed *feedP)
{
    const Char *errorP;
    MemSet(p, sizeof(*p), 0);
    p->feedP = feedP;
    errorP = LoadDecoder(p);
    if (errorP == 0) errorP = AllocateRings(p);
    if (errorP == 0) OpenStreams(p);
    return errorP != 0 ? errorP : p->errorP;
}

static void CloseStreams(Playback *p)
{
    UInt16 i;
    if (p->feedP == 0) return;
    StreamFeedStop(p->feedP);
    for (i = 0; i < 2; i++) {
        Mp4StreamClose(&p->streams[i]);
        if (p->ringsP[i] != 0) MemPtrFree(p->ringsP[i]);
    }
}

UInt32 PlaybackBufferedMs(const Playback *p, UInt16 track)
{
    const Mp4Stream *s = &p->streams[track];
    UInt32 durationMs, perSecond;
    if (p->feedP == 0 || Mp4StreamReceived(s)) return 0xFFFFFFFFUL;
    if (!s->ready || s->trackP->timescale == 0) return 0;
    durationMs = ScaleToMs(s->trackP->duration, s->trackP->timescale);
    perSecond = s->totalSize / (durationMs / 1000UL + 1) + 1;
    return Mp4StreamBuffered(s) / perSecond * 1000UL +
        Mp4StreamBuffered(s) % perSecond * 1000UL / perSecond;
}

/* Both streams have ms of playback in, or as much as their rings hold. */
static Boolean StreamsBuffered(const Playback *p, UInt32 ms)
{
    UInt16 i;
    for (i = 0; i < 2; i++) {
        const Mp4Stream *s = &p->streams[i];
        if (!s->ready) return false;
        if (PlaybackBufferedMs(p, i) < ms && Mp4StreamSpace(s) >= s->capacity / 8)
            return false;
    }
    return true;
}

void PlaybackClose(Playback *p)
{
    CloseStreams(p);
    if (p->moduleLoaded) {
        StopDecoders(p);
        ArmModuleUnload(&p->module);
    }
    if (p->arenaP != 0) MemPtrFree(p->arenaP);
    if (p->fileOpen) VFSFileClose(p->file);
    if (p->readerP != 0) MemPtrFree(p->readerP);
    if (p->sampleP != 0) MemPtrFree(p->sampleP);
    MemSet(p, sizeof(*p), 0);
}

/* Screen */

/* Fits the picture into the area, keeping its shape. Streams only know
 * its size once they start. */
static void FitVideo(Playback *p)
{
    UInt32 x = p->areaX, y = p->areaY, w = p->areaW, h = p->areaH;
    UInt32 videoW = p->readerP->video.width, videoH = p->readerP->video.height;
    if (p->bitsP == 0) return;
    if (videoW != 0 && videoH != 0) {
        if (w * videoH <= h * videoW) {
            UInt32 fitH = w * videoH / videoW;
            y += (h - fitH) / 2;
            h = fitH;
        } else {
            UInt32 fitW = h * videoW / videoH;
            x += (w - fitW) / 2;
            w = fitW;
        }
    }
    p->screenP = p->bitsP + y * p->rowBytes + x * 2;
    p->width = (UInt16)w;
    p->height = (UInt16)h;
}

const Char *PlaybackAttachScreen(Playback *p, const RectangleType *boundsP)
{
    BitmapType *bitmapP = WinGetBitmap(WinGetDisplayWindow());
    UInt32 depth = 0, density = kDensityLow, format = pixelFormatIndexed;
    UInt32 screenW, screenH, x, y, w, h;
    Coord bitmapW = 0, bitmapH = 0;
    UInt16 rowBytes = 0;
    UInt8 *bitsP;
    p->screenP = 0;
    p->bitsP = 0;
    if (WinScreenGetAttribute(winScreenDepth, &depth) != errNone || depth != 16)
        return "PalmView needs a 16-bit color screen.";
    WinScreenGetAttribute(winScreenDensity, &density);
    WinScreenGetAttribute(winScreenPixelFormat, &format);
    if (format != pixelFormat565 && format != pixelFormat565LE)
        return "This screen's pixel format is not supported.";
    if (bitmapP == 0 || (bitsP = (UInt8 *)BmpGetBits(bitmapP)) == 0)
        return "Cannot draw to the screen directly.";
    BmpGetDimensions(bitmapP, &bitmapW, &bitmapH, &rowBytes);
    if (density < kDensityLow) density = kDensityLow;
    /* The dimensions may be in native pixels or standard coordinates. */
    screenW = rowBytes / 2;
    screenH = (UInt32)bitmapW == screenW ? (UInt32)bitmapH
        : (UInt32)bitmapH * density / kDensityLow;
    x = (UInt32)boundsP->topLeft.x * density / kDensityLow;
    y = (UInt32)boundsP->topLeft.y * density / kDensityLow;
    w = (UInt32)boundsP->extent.x * density / kDensityLow;
    h = (UInt32)boundsP->extent.y * density / kDensityLow;
    if (w == 0 || h == 0 || x + w > screenW || y + h > screenH)
        return "The video area is off the screen.";
    p->bitsP = bitsP;
    p->rowBytes = rowBytes;
    p->areaX = x;
    p->areaY = y;
    p->areaW = w;
    p->areaH = h;
    p->rgbFlags = format == pixelFormat565 ? DEC_RGB_BIG_ENDIAN : 0;
    FitVideo(p);
    return 0;
}

/* Video */

/* Skips samples up to the first keyframe at or after catchUpMs, a few at
 * a time. Returns true while still looking. */
static Boolean CatchUp(Playback *p, Mp4Sample *sP)
{
    UInt16 i;
    p->videoWaiting = false;
    for (i = 0; i < CATCH_UP_SAMPLES_PER_STEP; i++) {
        UInt16 result = NextSample(p, STREAM_FEED_VIDEO, sP, false);
        if (result == sampleWait) {
            p->videoWaiting = true;
            return true;
        }
        if (result != sampleReady) {
            p->catchingUp = false;
            p->draining = true;
            return true;
        }
        if (sP->sync && FrameMs(p, sP->dts + (UInt32)sP->cto) >= p->catchUpMs) {
            p->catchingUp = false;
            return false;
        }
    }
    return true;
}

static void DecodeOne(Playback *p)
{
    UInt32 start = TimGetTicks(), elapsed;
    Mp4Sample s;
    Int32 result;
    if (p->draining) {
        result = Call(p, armCmdVideoDecode, 0, 0, 0, kLevelFlags[p->level]);
        if (result <= 0) p->inputEnded = true;
    } else {
        UInt16 next;
        if (p->catchingUp) {
            if (CatchUp(p, &s)) return;
            /* The stream copied the keyframe out already. */
            next = p->feedP != 0 || ReadSample(p, &s) ? sampleReady : sampleFailed;
        } else {
            next = NextSample(p, STREAM_FEED_VIDEO, &s, true);
        }
        p->videoWaiting = next == sampleWait;
        if (next == sampleWait) return;
        if (next == sampleEnd) {
            p->draining = true;
            return;
        }
        if (next != sampleReady) {
            p->decodeErrors++;
            return;
        }
        result = Call(p, armCmdVideoDecode, (UInt32)p->sampleP, s.size,
            s.dts + (UInt32)s.cto, kLevelFlags[p->level]);
        if (result < 0) p->decodeErrors++;
    }
    ReadQueue(p);
    elapsed = TimGetTicks() - start;
    p->windowBusy += elapsed;
    p->decodeMs = (p->decodeMs * 3 + TicksToMs(elapsed)) / 4;
    if (p->started && elapsed > p->peakDecode) p->peakDecode = elapsed;
}

/* Once a second: statistics, and the quality level. */
static void EndWindow(Playback *p)
{
    UInt32 now = TimGetTicks(), elapsed = now - p->windowStart;
    UInt32 perSecond = SysTicksPerSecond();
    Boolean late;
    if (elapsed < perSecond) return;
    p->fps10 = (UInt16)((UInt32)p->windowShown * 10UL * perSecond / elapsed);
    p->cpuPercent = (UInt16)(p->windowBusy >= elapsed ? 100
        : p->windowBusy * 100UL / elapsed);
    p->loopsPerSecond = (UInt16)((UInt32)p->windowLoops * perSecond / elapsed);
    p->stepsPerSecond = (UInt16)((UInt32)p->windowSteps * perSecond / elapsed);
    if (p->feedP != 0) {
        UInt32 received = StreamFeedReceived(p->feedP);
        p->netPercent = (UInt16)(p->windowNet >= elapsed ? 100
            : p->windowNet * 100UL / elapsed);
        p->netKBps = (UInt16)((received - p->windowReceived) / 1024UL *
            perSecond / elapsed);
        p->windowReceived = received;
    }
    p->diffMin = p->windowDiffMin;
    p->diffMax = p->windowDiffMax;
    p->lateTotal += p->windowLate;
    late = (UInt32)p->windowLate * 10UL > p->windowFrames;
    if (p->settling) {
        p->settling = false;
    } else if (late && p->cpuPercent >= BUSY_CPU_PERCENT &&
               p->level + 1 < PLAYBACK_LEVELS) {
        if (p->lastLevelDown != 0 &&
            now - p->lastLevelDown < LEVEL_HOLD_SECONDS * perSecond &&
            p->calmNeeded < MAX_CALM_WINDOWS)
            p->calmNeeded *= 2;
        p->level++;
        p->levelChanges++;
        p->calmWindows = 0;
    } else if (!late && p->cpuPercent < CALM_CPU_PERCENT && p->level > 0) {
        if (++p->calmWindows >= p->calmNeeded) {
            p->level--;
            p->levelChanges++;
            p->lastLevelDown = now;
            p->calmWindows = 0;
        }
    } else {
        p->calmWindows = 0;
    }
    p->windowStart = now;
    p->windowBusy = 0;
    p->windowFrames = 0;
    p->windowShown = 0;
    p->windowLate = 0;
    p->windowLoops = 0;
    p->windowSteps = 0;
    p->windowDiffMin = 0;
    p->windowDiffMax = 0;
    p->windowNet = 0;
}

static void ShowFront(Playback *p, UInt32 lateMs)
{
    p->windowFrames++;
    if (lateMs > SKIP_MS && p->skippedInRow < MAX_SKIPPED) {
        p->skippedInRow++;
        p->skipped++;
        p->windowLate++;
    } else {
        if (p->screenP != 0) {
            UInt32 start = TimGetTicks(), args[5];
            args[0] = (UInt32)p->screenP;
            args[1] = p->rowBytes;
            args[2] = p->width;
            args[3] = p->height;
            args[4] = p->rgbFlags;
            ArmModuleCall(&p->module, armCmdVideoRgb, args, 5);
            p->windowBusy += TimGetTicks() - start;
        }
        p->skippedInRow = 0;
        p->shown++;
        p->windowShown++;
        if (lateMs > LATE_MS) p->windowLate++;
    }
    Call(p, armCmdVideoPop, 0, 0, 0, 0);
    ReadQueue(p);
    if (p->soundStarted) {
        if (lateMs > CATCH_UP_MS && !p->catchingUp && !p->draining) {
            p->catchingUp = true;
            p->catchUpMs = VideoMs(p) + CATCH_UP_MS;
            p->catchUps++;
        }
    } else if (lateMs > RESYNC_MS) {
        p->clockOffsetMs -= (Int32)lateMs;
    }
    EndWindow(p);
}

/* Starts the clock, and the sound, once the first frame and some sound
 * are ready. */
static Boolean ReadyToStart(Playback *p)
{
    if (p->queued == 0 && !p->inputEnded) return false;
    if (p->soundOn && !p->soundInputEnded && p->soundBuffered < SOUND_PREFILL_FRAMES)
        return false;
    p->started = true;
    p->firstPts = p->frontPts;
    p->peakNet = 0;
    p->peakDecode = 0;
    p->peakAway = 0;
    SetClock(p, 0);
    p->windowStart = p->clockBase;
    if (p->soundOn && SndStreamStart(p->stream) == errNone) {
        p->soundStarted = true;
        /* What the OS took at once is its queue: not heard yet. */
        Call(p, armCmdSoundState, 0, 0, 0, 0);
        ReadSound(p);
        p->latencyFrames = p->soundPlayed;
    } else if (p->soundOn) {
        CloseSound(p);
        p->soundError = "the sound output did not start.";
    }
    return true;
}

static void ResetWindow(Playback *p)
{
    p->windowStart = TimGetTicks();
    p->windowBusy = 0;
    p->windowFrames = 0;
    p->windowShown = 0;
    p->windowLate = 0;
    p->windowNet = 0;
    if (p->feedP != 0) p->windowReceived = StreamFeedReceived(p->feedP);
}

/* Streams: the decoders start once both moov boxes and the first seconds
 * are in. */
static Boolean StreamsStarted(Playback *p)
{
    const Char *errorP;
    Boolean videoReady, audioReady;
    if (p->decoderOpen) return true;
    videoReady = Mp4StreamReady(&p->streams[STREAM_FEED_VIDEO]);
    audioReady = Mp4StreamReady(&p->streams[STREAM_FEED_AUDIO]);
    if (!videoReady || !audioReady) {
        if (p->streams[STREAM_FEED_VIDEO].status != mp4MergeOk ||
            p->streams[STREAM_FEED_AUDIO].status != mp4MergeOk)
            p->errorP = "The stream could not be read.";
        return false;
    }
    if (!StreamsBuffered(p, START_BUFFER_MS)) return false;
    errorP = StartDecoders(p);
    if (errorP != 0) {
        p->errorP = errorP;
        return false;
    }
    FitVideo(p);
    ResetWindow(p);
    return true;
}

static void SetSoundPaused(Playback *p, Boolean paused)
{
    if (p->soundOn) Call(p, armCmdSoundPause, paused ? 1 : 0, 0, 0, 0);
}

/* Streams: holds the clock while either stream has run dry, until both
 * have caught up. Returns true while waiting. */
static Boolean Buffering(Playback *p)
{
    if (!p->buffering) {
        if (!(p->videoWaiting && p->queued == 0) &&
            !(p->audioWaiting && p->soundStarted &&
              p->soundBuffered < SOUND_LOW_FRAMES))
            return false;
        p->pausedMs = VideoMs(p);
        p->buffering = true;
        p->bufferings++;
        SetSoundPaused(p, true);
        return true;
    }
    if (!StreamsBuffered(p, RESUME_BUFFER_MS)) return true;
    p->buffering = false;
    SetClock(p, p->pausedMs);
    SetSoundPaused(p, false);
    p->settling = true;
    ResetWindow(p);
    return false;
}

/* After work that found a stream not ready: a short wait, so the network
 * gets served instead of a busy loop. */
static Int32 WaitIf(Boolean waiting)
{
    return waiting ? (Int32)MsToTicks(FEED_INTERVAL_MS) : 0;
}

static Int32 StepOnce(Playback *p)
{
    UInt32 nowMs, dueMs, ticks;
    if (p->feedP != 0 && p->errorP == 0) p->errorP = StreamFeedError(p->feedP);
    if (p->errorP != 0 && p->playing) {
        CloseSound(p);
        if (p->feedP != 0) StreamFeedStop(p->feedP);
        p->playing = false;
        p->ended = true;
    }
    if (!p->playing)
        return p->feedP != 0 && !p->ended ? (Int32)MsToTicks(PAUSED_WAIT_MS)
            : evtWaitForever;
    if (p->feedP != 0 && !StreamsStarted(p)) return WaitIf(true);
    if (FeedSound(p)) return 0;
    if (!p->started) {
        if (!ReadyToStart(p)) {
            if (p->queued == 0 && !p->inputEnded) DecodeOne(p);
            return WaitIf(p->videoWaiting || p->audioWaiting);
        }
    }
    if (p->feedP != 0 && Buffering(p)) {
        /* Decode ahead while the clock waits. */
        if (p->queued < QUEUE_DEPTH && !p->inputEnded) {
            DecodeOne(p);
            if (!p->videoWaiting) return 0;
        }
        return WaitIf(true);
    }
    if (p->soundStarted) FollowSound(p);
    if (p->queued == 0) {
        if (!p->inputEnded) {
            DecodeOne(p);
            return WaitIf(p->videoWaiting);
        }
        /* Video is done; let the sound finish. */
        if (p->soundStarted && !(p->soundInputEnded && p->soundBuffered == 0))
            return (Int32)MsToTicks(100);
        CloseSound(p);
        p->pausedMs = PlaybackDurationMs(p);
        p->playing = false;
        p->ended = true;
        return evtWaitForever;
    }
    nowMs = VideoMs(p);
    dueMs = FrameMs(p, p->frontPts);
    if (dueMs <= nowMs) {
        ShowFront(p, nowMs - dueMs);
        return 0;
    }
    /* Decode ahead while there is time before the next frame is due. */
    if (p->queued < QUEUE_DEPTH && !p->inputEnded && dueMs - nowMs > p->decodeMs) {
        DecodeOne(p);
        if (!p->videoWaiting) return 0;
    }
    ticks = MsToTicks(dueMs - nowMs);
    if (ticks == 0) {
        ShowFront(p, 0);
        return 0;
    }
    if (p->feedP != 0 && ticks > MsToTicks(FEED_INTERVAL_MS))
        ticks = MsToTicks(FEED_INTERVAL_MS);
    return (Int32)ticks;
}

static void ServeNetwork(Playback *p)
{
    UInt32 start = TimGetTicks();
    StreamFeedStep(p->feedP);
    p->lastFeed = TimGetTicks();
    p->windowNet += p->lastFeed - start;
    if (p->started && p->lastFeed - start > p->peakNet)
        p->peakNet = p->lastFeed - start;
}

Int32 PlaybackStep(Playback *p)
{
    UInt32 start = TimGetTicks(), slice = MsToTicks(STEP_SLICE_MS);
    UInt32 feedInterval = MsToTicks(FEED_INTERVAL_MS);
    Int32 wait;
    if (p->started && p->playing && p->stepWait >= 0 &&
        start - p->stepEnd > (UInt32)p->stepWait &&
        start - p->stepEnd - (UInt32)p->stepWait > p->peakAway)
        p->peakAway = start - p->stepEnd - (UInt32)p->stepWait;
    p->windowLoops++;
    do {
        if (p->feedP != 0 && !p->ended && TimGetTicks() - p->lastFeed >= feedInterval)
            ServeNetwork(p);
        wait = StepOnce(p);
        p->windowSteps++;
    } while (wait == 0 && TimGetTicks() - start < slice && !EvtEventAvail() &&
             !EvtSysEventAvail(true));
    p->stepEnd = TimGetTicks();
    p->stepWait = wait;
    return wait;
}

/* Streams play again by downloading again. */
void PlaybackPlay(Playback *p)
{
    if (p->playing || (!p->decoderOpen && p->feedP == 0)) return;
    if (p->ended) {
        StopDecoders(p);
        if (p->feedP != 0) {
            p->ended = false;
            p->started = false;
            p->errorP = 0;
            OpenStreams(p);
        } else if (StartDecoders(p) != 0) {
            return;
        }
    }
    if (p->started) {
        SetClock(p, p->pausedMs);
        p->settling = true;
    }
    if (!p->buffering) SetSoundPaused(p, false);
    ResetWindow(p);
    p->stepWait = -1;   /* the pause is not a stall */
    p->playing = true;
}

/* The stream keeps running on silence instead of SndStreamPause, so the
 * frames played, and the clock, carry on exactly where they stopped. */
void PlaybackPause(Playback *p)
{
    if (!p->playing) return;
    p->pausedMs = VideoMs(p);
    p->playing = false;
    SetSoundPaused(p, true);
}

UInt32 PlaybackPositionMs(const Playback *p)
{
    UInt32 position = VideoMs(p), duration = PlaybackDurationMs(p);
    return duration != 0 && position > duration ? duration : position;
}

UInt32 PlaybackDurationMs(const Playback *p)
{
    const Mp4ReadTrack *t = &p->readerP->video;
    return t->timescale != 0 ? ScaleToMs(t->duration, t->timescale) : 0;
}

const Char *PlaybackError(const Playback *p)
{
    return p->errorP;
}
