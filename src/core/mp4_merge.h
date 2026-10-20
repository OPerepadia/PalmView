/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_MP4_MERGE_H
#define PALMVIEW_MP4_MERGE_H

#include <PalmOS.h>

/* Merges one fragmented-MP4 video stream and one fragmented-MP4 audio
 * stream (YouTube's DASH files) into a single regular MP4 with the moov box
 * first. Sample tables are streamed from the inputs' moof boxes, so memory
 * use does not grow with the video length. */

typedef Err (*Mp4ReadProc)(void *fileP, UInt32 offset, void *bufferP,
                           UInt32 length);
typedef Err (*Mp4WriteProc)(void *fileP, const void *bufferP, UInt32 length);
/* Return false to cancel. */
typedef Boolean (*Mp4ProgressProc)(void *contextP, UInt32 done, UInt32 total);

typedef struct Mp4Io {
    Mp4ReadProc readProcP;
    Mp4WriteProc writeProcP;
    Mp4ProgressProc progressProcP;
    void *contextP;
    void *videoP;
    void *audioP;
    void *outP;
    UInt32 videoSize;
    UInt32 audioSize;
} Mp4Io;

typedef enum Mp4MergeStatus {
    mp4MergeOk = 0,
    mp4MergeNoMemory,
    mp4MergeReadError,
    mp4MergeWriteError,
    mp4MergeBadInput,
    mp4MergeUnsupported,
    mp4MergeCancelled
} Mp4MergeStatus;

typedef struct Mp4MergeResult {
    UInt16 status;
    /* Source line of the failure, for bug reports. */
    UInt32 detail;
    UInt32 videoSamples;
    UInt32 audioSamples;
    UInt32 outputSize;
} Mp4MergeResult;

void Mp4Merge(const Mp4Io *ioP, Mp4MergeResult *resultP);

#endif
