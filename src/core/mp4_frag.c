/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "mp4_frag.h"

/* tfhd flags */
#define TFHD_BASE_DATA_OFFSET 0x000001UL
#define TFHD_SAMPLE_DESCRIPTION 0x000002UL
#define TFHD_DEFAULT_DURATION 0x000008UL
#define TFHD_DEFAULT_SIZE 0x000010UL
#define TFHD_DEFAULT_FLAGS 0x000020UL

/* trun flags */
#define TRUN_DATA_OFFSET 0x000001UL
#define TRUN_FIRST_FLAGS 0x000004UL
#define TRUN_DURATION 0x000100UL
#define TRUN_SIZE 0x000200UL
#define TRUN_FLAGS 0x000400UL
#define TRUN_CTO 0x000800UL

UInt32 Mp4Get32(const UInt8 *p)
{
    return ((UInt32)p[0] << 24) | ((UInt32)p[1] << 16) |
        ((UInt32)p[2] << 8) | (UInt32)p[3];
}

Boolean Mp4FindBox(const UInt8 *p, UInt32 length, UInt32 type, Mp4Box *boxP)
{
    while (length >= 8) {
        UInt32 size = Mp4Get32(p);
        UInt32 header = 8;
        if (size == 1) {
            if (length < 16 || Mp4Get32(p + 8) != 0) return false;
            size = Mp4Get32(p + 12);
            header = 16;
        } else if (size == 0) {
            size = length;
        }
        if (size < header || size > length) return false;
        if (Mp4Get32(p + 4) == type) {
            boxP->type = type;
            boxP->startP = p;
            boxP->size = size;
            boxP->payloadP = p + header;
            boxP->payloadSize = size - header;
            return true;
        }
        p += size;
        length -= size;
    }
    return false;
}

Boolean Mp4FindPath(const UInt8 *p, UInt32 length, const UInt32 *typesP,
                    UInt16 count, Mp4Box *boxP)
{
    UInt16 i;
    for (i = 0; i < count; i++) {
        if (!Mp4FindBox(p, length, typesP[i], boxP)) return false;
        p = boxP->payloadP;
        length = boxP->payloadSize;
    }
    return true;
}

Boolean Mp4FragReadDefaults(const UInt8 *moovP, UInt32 length,
                            Mp4FragDefaults *defaultsP)
{
    static const UInt32 kTkhd[] = { MP4_FOURCC('t', 'r', 'a', 'k'),
        MP4_FOURCC('t', 'k', 'h', 'd') };
    Mp4Box box, mvex;
    const UInt8 *p;
    UInt32 left;
    MemSet(defaultsP, sizeof(*defaultsP), 0);
    if (!Mp4FindPath(moovP, length, kTkhd, 2, &box) || box.payloadSize < 84)
        return false;
    defaultsP->trackId = Mp4Get32(box.payloadP + (box.payloadP[0] == 1 ? 20 : 12));
    if (!Mp4FindBox(moovP, length, MP4_FOURCC('m', 'v', 'e', 'x'), &mvex))
        return false;
    p = mvex.payloadP;
    left = mvex.payloadSize;
    while (Mp4FindBox(p, left, MP4_FOURCC('t', 'r', 'e', 'x'), &box)) {
        if (box.payloadSize >= 24 &&
            Mp4Get32(box.payloadP + 4) == defaultsP->trackId) {
            defaultsP->duration = Mp4Get32(box.payloadP + 12);
            defaultsP->size = Mp4Get32(box.payloadP + 16);
            defaultsP->flags = Mp4Get32(box.payloadP + 20);
            break;
        }
        left -= (UInt32)(box.startP + box.size - p);
        p = box.startP + box.size;
    }
    return true;
}

static UInt16 Fail(Mp4FragIter *iterP, UInt16 status, UInt32 line)
{
    if (iterP->status == mp4MergeOk) {
        iterP->status = status;
        iterP->line = line;
    }
    return iterP->status;
}

#define FAIL(iterP, status) Fail((iterP), (status), __LINE__)

UInt16 Mp4FragBegin(Mp4FragIter *iterP, const UInt8 *moofP, UInt32 moofSize,
                    UInt32 moofOffset, const Mp4FragDefaults *defaultsP)
{
    Mp4Box traf, box;
    const UInt8 *p;
    UInt32 flags;
    MemSet(iterP, sizeof(*iterP), 0);
    if (moofSize < 8 ||
        !Mp4FindBox(moofP + 8, moofSize - 8, MP4_FOURCC('t', 'r', 'a', 'f'),
            &traf) ||
        !Mp4FindBox(traf.payloadP, traf.payloadSize,
            MP4_FOURCC('t', 'f', 'h', 'd'), &box) || box.payloadSize < 8)
        return FAIL(iterP, mp4MergeBadInput);
    p = box.payloadP;
    flags = Mp4Get32(p) & 0x00FFFFFFUL;
    if (box.payloadSize < 8UL +
            ((flags & TFHD_BASE_DATA_OFFSET) ? 8 : 0) +
            ((flags & TFHD_SAMPLE_DESCRIPTION) ? 4 : 0) +
            ((flags & TFHD_DEFAULT_DURATION) ? 4 : 0) +
            ((flags & TFHD_DEFAULT_SIZE) ? 4 : 0) +
            ((flags & TFHD_DEFAULT_FLAGS) ? 4 : 0))
        return FAIL(iterP, mp4MergeBadInput);
    if (Mp4Get32(p + 4) != defaultsP->trackId)
        return FAIL(iterP, mp4MergeUnsupported);
    p += 8;
    iterP->base = moofOffset;
    if (flags & TFHD_BASE_DATA_OFFSET) {
        if (Mp4Get32(p) != 0) return FAIL(iterP, mp4MergeUnsupported);
        iterP->base = Mp4Get32(p + 4);
        p += 8;
    }
    if (flags & TFHD_SAMPLE_DESCRIPTION) {
        if (Mp4Get32(p) != 1) return FAIL(iterP, mp4MergeUnsupported);
        p += 4;
    }
    iterP->defaultDuration = defaultsP->duration;
    iterP->defaultSize = defaultsP->size;
    iterP->defaultFlags = defaultsP->flags;
    if (flags & TFHD_DEFAULT_DURATION) { iterP->defaultDuration = Mp4Get32(p); p += 4; }
    if (flags & TFHD_DEFAULT_SIZE) { iterP->defaultSize = Mp4Get32(p); p += 4; }
    if (flags & TFHD_DEFAULT_FLAGS) { iterP->defaultFlags = Mp4Get32(p); p += 4; }

    if (Mp4FindBox(traf.payloadP, traf.payloadSize,
            MP4_FOURCC('t', 'f', 'd', 't'), &box) && box.payloadSize >= 8) {
        if (box.payloadP[0] == 1) {
            if (box.payloadSize < 12 || Mp4Get32(box.payloadP + 4) != 0)
                return FAIL(iterP, mp4MergeUnsupported);
            iterP->startTime = Mp4Get32(box.payloadP + 8);
        } else {
            iterP->startTime = Mp4Get32(box.payloadP + 4);
        }
    }
    iterP->nextP = traf.payloadP;
    iterP->left = traf.payloadSize;
    return mp4MergeOk;
}

/* Moves to the next trun. False when there is none. */
static Boolean NextTrun(Mp4FragIter *iterP)
{
    Mp4Box box;
    const UInt8 *q, *endP;
    UInt16 entrySize;
    UInt32 count;
    if (!Mp4FindBox(iterP->nextP, iterP->left, MP4_FOURCC('t', 'r', 'u', 'n'),
            &box))
        return false;
    iterP->left -= (UInt32)(box.startP + box.size - iterP->nextP);
    iterP->nextP = box.startP + box.size;
    q = box.payloadP;
    endP = box.payloadP + box.payloadSize;
    if (box.payloadSize < 8) {
        FAIL(iterP, mp4MergeBadInput);
        return false;
    }
    iterP->signedCto = q[0] != 0;
    iterP->trunFlags = Mp4Get32(q) & 0x00FFFFFFUL;
    count = Mp4Get32(q + 4);
    if (box.payloadSize < 8UL + ((iterP->trunFlags & TRUN_DATA_OFFSET) ? 4 : 0) +
            ((iterP->trunFlags & TRUN_FIRST_FLAGS) ? 4 : 0)) {
        FAIL(iterP, mp4MergeBadInput);
        return false;
    }
    q += 8;
    if (iterP->trunFlags & TRUN_DATA_OFFSET) {
        iterP->nextData = iterP->base + Mp4Get32(q);
        q += 4;
    } else if (!iterP->anyTrun) {
        iterP->nextData = iterP->base;
    }
    iterP->hasFirstFlags = (iterP->trunFlags & TRUN_FIRST_FLAGS) != 0;
    iterP->firstFlags = iterP->defaultFlags;
    if (iterP->hasFirstFlags) {
        iterP->firstFlags = Mp4Get32(q);
        q += 4;
    }
    entrySize = (UInt16)(((iterP->trunFlags & TRUN_DURATION) ? 4 : 0) +
        ((iterP->trunFlags & TRUN_SIZE) ? 4 : 0) +
        ((iterP->trunFlags & TRUN_FLAGS) ? 4 : 0) +
        ((iterP->trunFlags & TRUN_CTO) ? 4 : 0));
    if (entrySize != 0 && (UInt32)(endP - q) / entrySize < count) {
        FAIL(iterP, mp4MergeBadInput);
        return false;
    }
    iterP->entryP = q;
    iterP->entriesLeft = count;
    iterP->firstEntry = true;
    iterP->anyTrun = true;
    return true;
}

Boolean Mp4FragNext(Mp4FragIter *iterP, Mp4FragSample *sampleP)
{
    const UInt8 *q;
    UInt32 flags = iterP->trunFlags;
    Boolean first;
    if (iterP->status != mp4MergeOk) return false;
    while (iterP->entriesLeft == 0) {
        if (!NextTrun(iterP)) return false;
        flags = iterP->trunFlags;
    }
    q = iterP->entryP;
    first = iterP->firstEntry;
    sampleP->duration = iterP->defaultDuration;
    sampleP->size = iterP->defaultSize;
    sampleP->flags = first && iterP->hasFirstFlags ? iterP->firstFlags
        : iterP->defaultFlags;
    sampleP->cto = 0;
    sampleP->ctoSigned = iterP->signedCto;
    if (flags & TRUN_DURATION) { sampleP->duration = Mp4Get32(q); q += 4; }
    if (flags & TRUN_SIZE) { sampleP->size = Mp4Get32(q); q += 4; }
    if (flags & TRUN_FLAGS) {
        if (!(first && iterP->hasFirstFlags)) sampleP->flags = Mp4Get32(q);
        q += 4;
    }
    if (flags & TRUN_CTO) { sampleP->cto = Mp4Get32(q); q += 4; }
    sampleP->offset = iterP->nextData;
    iterP->nextData += sampleP->size;
    iterP->entryP = q;
    iterP->entriesLeft--;
    iterP->firstEntry = false;
    return true;
}
