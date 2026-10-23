/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_MP4_FRAG_H
#define PALMVIEW_MP4_FRAG_H

#include <PalmOS.h>
#include "mp4_merge.h"

/* Boxes in memory, and the samples of a fragmented MP4 (YouTube's DASH
 * streams). Used by the merger and the streaming reader. Statuses are
 * mp4MergeStatus codes. */

#define MP4_FOURCC(a, b, c, d) (((UInt32)(a) << 24) | ((UInt32)(b) << 16) | \
                                ((UInt32)(c) << 8) | (UInt32)(d))
#define MP4_SAMPLE_NON_SYNC 0x00010000UL

typedef struct Mp4Box {
    UInt32 type;
    const UInt8 *startP;    /* the whole box, header included */
    UInt32 size;
    const UInt8 *payloadP;
    UInt32 payloadSize;
} Mp4Box;

UInt32 Mp4Get32(const UInt8 *p);
/* Finds the first child box of the given type within [p, p + length). */
Boolean Mp4FindBox(const UInt8 *p, UInt32 length, UInt32 type, Mp4Box *boxP);
Boolean Mp4FindPath(const UInt8 *p, UInt32 length, const UInt32 *typesP,
                    UInt16 count, Mp4Box *boxP);

/* What fragments may leave out: the trex values of the first track. */
typedef struct Mp4FragDefaults {
    UInt32 trackId;
    UInt32 duration;
    UInt32 size;
    UInt32 flags;
} Mp4FragDefaults;

/* From a moov box payload. False without a track or mvex. */
Boolean Mp4FragReadDefaults(const UInt8 *moovP, UInt32 length,
                            Mp4FragDefaults *defaultsP);

typedef struct Mp4FragSample {
    UInt32 offset;          /* in the file */
    UInt32 size;
    UInt32 duration;
    UInt32 flags;
    UInt32 cto;
    Boolean ctoSigned;
} Mp4FragSample;

/* Walks the samples of one moof. The moof must stay in memory. */
typedef struct Mp4FragIter {
    UInt16 status;
    UInt32 line;            /* source line of the failure */
    UInt32 startTime;       /* from tfdt; 0 without one */
    Boolean hasStartTime;
    const UInt8 *nextP;     /* where the next trun may be */
    UInt32 left;
    const UInt8 *entryP;
    UInt32 entriesLeft;
    Boolean firstEntry;
    UInt32 trunFlags;
    UInt32 firstFlags;
    Boolean hasFirstFlags;
    Boolean signedCto;
    Boolean anyTrun;
    UInt32 base;
    UInt32 nextData;
    UInt32 defaultDuration;
    UInt32 defaultSize;
    UInt32 defaultFlags;
} Mp4FragIter;

/* moofP holds the whole moof box, which starts at moofOffset in its file.
 * Returns mp4MergeOk or why the fragment cannot be read. */
UInt16 Mp4FragBegin(Mp4FragIter *iterP, const UInt8 *moofP, UInt32 moofSize,
                    UInt32 moofOffset, const Mp4FragDefaults *defaultsP);
/* False after the last sample, or on failure (see iterP->status). */
Boolean Mp4FragNext(Mp4FragIter *iterP, Mp4FragSample *sampleP);

#endif
