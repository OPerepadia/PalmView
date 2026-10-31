/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_MP4_READ_H
#define PALMVIEW_MP4_READ_H

#include <PalmOS.h>
#include "mp4_merge.h"

/* Reads samples from a regular (non-fragmented) MP4 such as the files
 * PalmView saves. The moov box is walked in the file and the sample tables
 * are read through small buffers, so memory use is a few KB regardless of
 * the video length. A fragmented MP4's moov opens too: its tracks are
 * described but have no samples (mp4_stream.h reads those). */

#define MP4_CONFIG_CAPACITY 128
#define MP4_TABLE_BUFFER 512

typedef struct Mp4TableReader {
    UInt32 offset;       /* file offset of the first entry */
    UInt32 count;
    UInt32 next;         /* index of the next entry to return */
    UInt32 bufferFirst;  /* entry index of buffer[0] */
    UInt16 entrySize;
    UInt16 bufferCount;
    UInt8 buffer[MP4_TABLE_BUFFER];
} Mp4TableReader;

typedef struct Mp4Sample {
    UInt32 offset;
    UInt32 size;
    UInt32 dts;          /* in the track timescale */
    Int32 cto;           /* composition offset */
    Boolean sync;
} Mp4Sample;

typedef struct Mp4ReadTrack {
    Boolean present;
    UInt32 timescale;
    UInt32 duration;         /* in the timescale; 0 if unknown or too long */
    UInt32 sampleCount;
    UInt32 constantSize;
    UInt16 width;
    UInt16 height;
    UInt16 channels;
    UInt16 sampleRate;
    UInt8 config[MP4_CONFIG_CAPACITY];   /* avcC or AudioSpecificConfig */
    UInt16 configLength;
    Boolean co64;
    Boolean hasCtts;
    Boolean hasStss;
    Mp4TableReader stsz, stco, stsc, stts, ctts, stss;
    /* iteration */
    UInt32 sample;
    UInt32 chunk;            /* 1-based */
    UInt32 chunkSamplesLeft;
    UInt32 position;         /* file offset of the next sample */
    UInt32 nextStscChunk;    /* first chunk of the next stsc entry */
    UInt32 stscSamples;      /* samples per chunk now */
    UInt32 stscPending;      /* samples per chunk from nextStscChunk on */
    UInt32 dts;
    UInt32 sttsLeft, sttsDelta;
    UInt32 cttsLeft;
    Int32 cttsOffset;
    UInt32 nextSync;         /* 1-based sample number, 0 if none left */
} Mp4ReadTrack;

typedef struct Mp4Reader {
    Mp4ReadProc readProcP;
    void *fileP;
    UInt32 fileSize;
    UInt16 status;
    UInt32 detail;
    Mp4ReadTrack video;
    Mp4ReadTrack audio;
} Mp4Reader;

/* Uses mp4MergeStatus codes. */
UInt16 Mp4ReaderOpen(Mp4Reader *readerP, Mp4ReadProc readProcP, void *fileP,
                     UInt32 fileSize);
/* Returns false at the end of the track or on error (see readerP->status). */
Boolean Mp4ReaderNext(Mp4Reader *readerP, Mp4ReadTrack *trackP,
                      Mp4Sample *sampleP);
/* Moves to the last sync sample at or before dts, not before minimum (or
 * the first after minimum). Returns false, staying put, if there is none. */
Boolean Mp4ReaderSeek(Mp4Reader *readerP, Mp4ReadTrack *trackP, UInt32 dts,
                      UInt32 minimum, UInt32 *foundP);

#endif
