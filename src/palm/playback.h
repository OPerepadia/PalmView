/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_PLAYBACK_H
#define PALMVIEW_PLAYBACK_H

#include <PalmOS.h>
#include "arm_module.h"
#include "mp4_read.h"
#include "mp4_stream.h"
#include "store.h"

/* Plays a saved MP4. The native ARM module decodes a few video frames
 * ahead and converts each one straight into the screen's framebuffer when
 * its time comes. AAC sound goes through a PCM ring that a native Palm OS
 * sound stream callback drains; the frames it has played set the clock.
 * Without sound, the clock is the system tick count.
 *
 * When video falls behind, quality drops a level at a time (see
 * kLevelFlags in playback.c) and comes back once there is time to spare.
 * If it is still far behind, video skips ahead to the next keyframe.
 *
 * It can also play YouTube's two streams while they download (a
 * StreamFeed). The compressed data waits in RAM rings, and the clock stops
 * to buffer whenever either stream runs dry. Streams can seek, to the
 * start of a segment. */

#define PLAYBACK_LEVELS 4

struct StreamFeed;

typedef struct Playback {
    ArmModule module;
    Boolean moduleLoaded;
    UInt8 *arenaP;
    UInt32 arenaSize;
    UInt8 *extraP;              /* more decoder heap for large video */
    UInt32 extraSize;
    UInt32 heapFree;            /* before the arena, for the log */
    UInt32 heapChunk;           /* the largest free chunk then */
    Mp4Reader *readerP;
    UInt8 *sampleP;
    UInt16 volume;
    FileRef file;
    UInt32 fileSize;
    Boolean fileOpen;
    Boolean decoderOpen;
    Boolean large;              /* frames bigger than 144p */

    /* Output rectangle in the framebuffer, native pixels, and the area it
     * is fitted into */
    UInt8 *screenP;
    UInt32 rowBytes;
    UInt16 width;
    UInt16 height;
    UInt32 rgbFlags;
    Boolean turned;
    UInt8 *bitsP;
    UInt32 areaX, areaY, areaW, areaH;

    /* Streaming */
    struct StreamFeed *feedP;   /* 0 when playing a file */
    Mp4Stream streams[2];       /* video, audio */
    UInt32 startMs;             /* where to start, until the streams do */
    UInt8 *ringsP[2];
    UInt32 ringSizes[2];
    Boolean buffering;          /* the clock waits for data */
    Boolean videoWaiting;       /* the next sample has not arrived */
    Boolean audioWaiting;
    UInt32 bufferings;
    UInt32 lastFeed;            /* ticks */
    const Char *errorP;         /* why streaming stopped */

    /* Clock: ticks since clockBase plus clockOffsetMs, or pausedMs when
     * paused. With sound, clockOffsetMs follows the sound played. */
    Boolean playing;
    Boolean ended;
    Boolean started;
    UInt32 clockBase;
    Int32 clockOffsetMs;
    UInt32 pausedMs;
    UInt32 firstPts;
    Boolean firstPtsKnown;
    UInt32 timescale;
    UInt32 seekMs;          /* where the decoders started, 0 or a seek */
    UInt32 audioFrom;       /* about where the sound at seekMs starts */
    UInt32 seeks;

    /* Sound */
    Boolean ringOpen;           /* the decoder and PCM ring exist */
    Boolean soundOn;            /* the stream exists */
    Boolean soundStarted;
    Boolean soundInputEnded;
    SndStreamRef stream;
    UInt16 gain;
    UInt32 sampleRate;
    UInt32 ringFrames;
    UInt32 soundBuffered;       /* frames decoded, not yet played */
    UInt32 soundPlayed;
    UInt32 soundUnderruns;
    UInt32 soundLastRequest;
    UInt32 decoderRate;         /* 0 until the first packet */
    UInt32 latencyFrames;       /* handed to the OS but not heard yet */
    UInt32 lastPlayed;
    UInt32 audioErrors;
    const Char *soundError;     /* why there is no sound */

    /* Decoder state */
    UInt32 queued;          /* decoded frames waiting */
    UInt16 queueDepth;      /* at most */
    UInt32 frontPts;        /* pts of the oldest */
    Boolean draining;
    Boolean inputEnded;
    UInt32 decodeMs;        /* recent decode time per sample */
    Boolean catchingUp;     /* skipping to a keyframe */
    UInt32 catchUpMs;

    UInt16 level;
    UInt16 bestLevel;       /* the best this video may use */
    UInt16 skippedInRow;
    UInt16 calmWindows;
    UInt16 calmNeeded;      /* grows when a level does not hold */
    UInt32 lastLevelDown;   /* ticks, 0 if never */
    Boolean settling;       /* just resumed: this window keeps the level */
    Boolean strained;       /* the last window was late with the CPU busy */

    /* Statistics; the window restarts every second */
    UInt32 windowStart;
    UInt32 windowBusy;      /* ticks spent decoding and drawing */
    UInt16 windowFrames;
    UInt16 windowShown;
    UInt16 windowLate;
    UInt32 shown;
    UInt32 skipped;
    UInt32 catchUps;
    UInt32 decodeErrors;
    UInt16 fps10;           /* frames shown per second, times 10 */
    UInt16 cpuPercent;

    /* For the status line and the network log, per second */
    UInt32 lateTotal;
    UInt32 windowNet;       /* ticks spent on the network */
    UInt32 windowReceived;  /* bytes received when the window began */
    UInt16 netPercent;
    UInt16 netKBps;

    /* The longest network turn since playback started, in ticks */
    UInt32 peakNet;
} Playback;

/* Opens pathP on the card and starts the decoder. Returns 0, or a message
 * saying what went wrong. Call PlaybackClose either way. */
const Char *PlaybackOpen(Playback *p, const Char *pathP);
/* Streams instead, from startMs (rounded down to a segment start). The
 * decoders start once the first seconds are in. large says the frames are
 * bigger than 144p, so the memory they need is set aside first. The feed
 * must outlive the playback. */
const Char *PlaybackOpenStream(Playback *p, struct StreamFeed *feedP,
                               Boolean large, UInt32 startMs);
void PlaybackClose(Playback *p);

/* Fits the video into boundsP (standard coordinates on the display),
 * turned a quarter clockwise if asked, for the TX held in landscape.
 * Returns 0, or a message if the screen cannot be drawn to directly. */
const Char *PlaybackAttachScreen(Playback *p, const RectangleType *boundsP,
                                 Boolean turned);
/* Frames are decoded but not drawn until the next attach. */
void PlaybackDetachScreen(Playback *p);

/* Plays from the start again once the video has ended. */
void PlaybackPlay(Playback *p);
void PlaybackPause(Playback *p);
/* 1024 is unity gain. PlaybackOpen and PlaybackOpenStream set it to 0
 * (silent), so set it after opening. */
void PlaybackSetVolume(Playback *p, UInt16 gain);

/* Does whatever is due: decodes, draws or waits. Returns the ticks until
 * it needs to run again, as an EvtGetEvent timeout. */
Int32 PlaybackStep(Playback *p);

/* True once a stream has played and both segment indexes are in. */
Boolean PlaybackCanSeek(const Playback *p);
/* Moves a stream near ms and returns where it lands: the start of the
 * segment holding ms. Keeps playing, or paused. */
UInt32 PlaybackSeek(Playback *p, UInt32 ms);
/* Seeks about ms from the position, backward or forward. */
UInt32 PlaybackSkip(Playback *p, Boolean forward, UInt32 ms);
UInt32 PlaybackPositionMs(const Playback *p);
UInt32 PlaybackDurationMs(const Playback *p);
/* Playback time in a stream's ring; 0xFFFFFFFF once all of it is in. */
UInt32 PlaybackBufferedMs(const Playback *p, UInt16 track);
/* Why streaming stopped, or 0. */
const Char *PlaybackError(const Playback *p);
/* Arena bytes the decoder uses now, and at most so far. */
void PlaybackArenaUse(Playback *p, UInt32 *usedP, UInt32 *peakP);

#endif
