/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_MP4_STREAM_H
#define PALMVIEW_MP4_STREAM_H

#include <PalmOS.h>
#include "mp4_frag.h"
#include "mp4_read.h"

/* Reads one fragmented MP4 track (a YouTube DASH stream) while it
 * downloads. Received bytes go into a ring. Each sample comes out once all
 * of it has arrived, and its bytes are then dropped. Besides the ring,
 * only the current moof is kept, so memory use does not grow with the
 * video length.
 *
 *   if (Mp4StreamSpace(&s) >= length) Mp4StreamWrite(&s, dataP, length);
 *   if (Mp4StreamNext(&s, &sample, bufferP, capacity) == mp4StreamSample)
 *       decode bufferP...
 *
 * The segment index (sidx) after the moov, when there is one, lets it
 * start again at any segment. */

/* Largest moov or moof kept. YouTube's are under 2 KB. */
#define MP4_STREAM_BOX_CAPACITY 16384UL

typedef enum Mp4StreamResult {
    mp4StreamSample = 0,    /* a sample was copied out */
    mp4StreamWait,          /* more bytes are needed */
    mp4StreamEnd,
    mp4StreamError          /* see status and detail */
} Mp4StreamResult;

/* A segment from the sidx: where its first moof is, and its earliest
 * presentation time in the track's timescale. */
typedef struct Mp4Segment {
    UInt32 offset;
    UInt32 time;
} Mp4Segment;

typedef struct Mp4Stream {
    UInt8 *ringP;
    UInt32 capacity;
    UInt32 head;            /* ring index of readOffset */
    UInt32 readOffset;      /* stream offset of the oldest byte kept */
    UInt32 writeOffset;     /* stream offset after the newest byte */
    UInt32 totalSize;
    UInt16 status;          /* mp4MergeStatus */
    UInt32 detail;          /* source line of the failure */
    Mp4ReadTrack *trackP;
    Boolean ready;          /* the moov has been read */

    UInt16 state;
    UInt32 next;            /* stream offset of the next top-level box */
    UInt32 skipEnd;
    UInt32 mdatEnd;
    UInt8 *boxP;            /* the moov, then the current moof */
    Mp4FragDefaults defaults;
    Mp4FragIter iter;
    Boolean inFragment;
    Boolean havePending;
    Mp4FragSample pending;
    UInt32 dts;
    Mp4Segment *segmentsP;  /* 0 until the sidx is read */
    UInt16 segmentCount;
} Mp4Stream;

/* totalSize is the stream's length in bytes. Once the moov has arrived,
 * *trackP describes the track as Mp4ReaderOpen would. Returns mp4MergeOk
 * or mp4MergeNoMemory. */
UInt16 Mp4StreamOpen(Mp4Stream *s, UInt8 *ringP, UInt32 capacity,
                     UInt32 totalSize, Mp4ReadTrack *trackP);
void Mp4StreamClose(Mp4Stream *s);

/* Bytes the ring can take now. */
UInt32 Mp4StreamSpace(const Mp4Stream *s);
/* Appends the next bytes of the stream, at most Mp4StreamSpace. */
void Mp4StreamWrite(Mp4Stream *s, const UInt8 *dataP, UInt32 length);
/* Bytes received and not read out yet. */
UInt32 Mp4StreamBuffered(const Mp4Stream *s);
Boolean Mp4StreamReceived(const Mp4Stream *s);

/* Reads what has arrived. True once *trackP is filled in. */
Boolean Mp4StreamReady(Mp4Stream *s);
/* Reads on past the moov to the segment index, if one follows, so the
 * stream can start at any segment before its first sample. True once
 * there is nothing more to wait for; segmentCount says if there is one. */
Boolean Mp4StreamIndexed(Mp4Stream *s);
/* Copies the next sample into bufferP; sampleP->offset is its offset in
 * the stream. */
UInt16 Mp4StreamNext(Mp4Stream *s, Mp4Sample *sampleP, UInt8 *bufferP,
                     UInt32 capacity);

/* The last segment starting at or before time (the track's timescale), or
 * the first one. Needs segmentCount != 0. */
UInt16 Mp4StreamFindSegment(const Mp4Stream *s, UInt32 time);
/* Drops what the ring holds and goes on from the segment's start, where
 * the next byte written must come from. False without that segment. */
Boolean Mp4StreamSeek(Mp4Stream *s, UInt16 segment);

#endif
