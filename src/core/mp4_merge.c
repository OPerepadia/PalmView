/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "mp4_merge.h"
#include "mp4_frag.h"

/* Output layout: ftyp, moov (video trak, audio trak), mdat. Each input
 * fragment becomes one chunk; chunks are interleaved by start time. */

#define MAX_MOOV_SIZE 16384UL
#define MAX_MOOF_SIZE 32768UL
#define COPY_BUFFER_SIZE 8192UL
#define WRITE_BUFFER_SIZE 2048
#define MOVIE_TIMESCALE 1000UL
#define FTYP_SIZE 32UL

#define FOURCC(a, b, c, d) (((UInt32)(a) << 24) | ((UInt32)(b) << 16) | \
                            ((UInt32)(c) << 8) | (UInt32)(d))

typedef struct Fragment {
    UInt32 moofOffset;
    UInt32 moofSize;
    UInt32 dataOffset;
    UInt32 dataSize;
    UInt32 sampleCount;
    UInt32 startTime;
    UInt32 outOffset;
} Fragment;

typedef struct Track {
    void *fileP;
    UInt32 fileSize;
    Boolean isVideo;
    UInt8 *moovP;
    UInt32 moovSize;
    const UInt8 *hdlrP;
    UInt32 hdlrSize;
    const UInt8 *mediaHeaderP;
    UInt32 mediaHeaderSize;
    const UInt8 *dinfP;
    UInt32 dinfSize;
    const UInt8 *stsdP;
    UInt32 stsdSize;
    UInt32 timescale;
    UInt32 language;
    UInt32 width;
    UInt32 height;
    UInt32 mediaTime;
    Boolean hasEdit;
    Mp4FragDefaults defaults;
    Fragment *fragmentsP;
    UInt32 fragmentCount;
    UInt32 sampleCount;
    UInt32 duration;
    UInt32 sttsEntries;
    UInt32 cttsEntries;
    UInt32 stssEntries;
    UInt32 stscEntries;
    Boolean needCtts;
    Boolean signedCtts;
    UInt32 trakSize;
    UInt32 mdiaSize;
    UInt32 minfSize;
    UInt32 stblSize;
    UInt32 trackDuration;
} Track;

typedef struct Rle {
    UInt32 count;
    UInt32 value;
    UInt32 entries;
} Rle;

typedef struct Merge {
    const Mp4Io *ioP;
    Mp4MergeResult *resultP;
    Track tracks[2];
    UInt8 *moofP;
    UInt8 *copyP;
    UInt16 writeLength;
    UInt32 written;
    UInt32 copyTotal;
    UInt32 copyDone;
    UInt8 writeBuffer[WRITE_BUFFER_SIZE];
} Merge;

typedef Boolean (*SampleProc)(Merge *m, Track *t, const Mp4FragSample *s,
                              void *contextP);

static Boolean Fail(Merge *m, UInt16 status, UInt32 line)
{
    if (m->resultP->status == mp4MergeOk) {
        m->resultP->status = status;
        m->resultP->detail = line;
    }
    return false;
}

#define FAIL(m, status) Fail((m), (status), __LINE__)

static UInt32 Get32(const UInt8 *p)
{
    return ((UInt32)p[0] << 24) | ((UInt32)p[1] << 16) |
        ((UInt32)p[2] << 8) | (UInt32)p[3];
}

static UInt16 Get16(const UInt8 *p)
{
    return (UInt16)(((UInt16)p[0] << 8) | p[1]);
}

/* Milliseconds from media ticks without 32-bit overflow. */
static UInt32 ToMovieTime(UInt32 ticks, UInt32 timescale)
{
    return (ticks / timescale) * MOVIE_TIMESCALE +
        (ticks % timescale) * MOVIE_TIMESCALE / timescale;
}

static Boolean ReadAt(Merge *m, void *fileP, UInt32 offset, void *bufferP,
                      UInt32 length)
{
    if (m->ioP->readProcP(fileP, offset, bufferP, length) != errNone)
        return FAIL(m, mp4MergeReadError);
    return true;
}

/* Buffered big-endian output */

static Boolean Flush(Merge *m)
{
    if (m->writeLength == 0) return true;
    if (m->ioP->writeProcP(m->ioP->outP, m->writeBuffer, m->writeLength) !=
            errNone)
        return FAIL(m, mp4MergeWriteError);
    m->writeLength = 0;
    return true;
}

static Boolean PutBytes(Merge *m, const void *dataP, UInt32 length)
{
    const UInt8 *p = (const UInt8 *)dataP;
    m->written += length;
    while (length != 0) {
        UInt32 room = WRITE_BUFFER_SIZE - m->writeLength;
        UInt32 count = length < room ? length : room;
        MemMove(m->writeBuffer + m->writeLength, p, count);
        m->writeLength += (UInt16)count;
        p += count;
        length -= count;
        if (m->writeLength == WRITE_BUFFER_SIZE && !Flush(m)) return false;
    }
    return true;
}

static Boolean Put32(Merge *m, UInt32 value)
{
    UInt8 bytes[4];
    bytes[0] = (UInt8)(value >> 24);
    bytes[1] = (UInt8)(value >> 16);
    bytes[2] = (UInt8)(value >> 8);
    bytes[3] = (UInt8)value;
    return PutBytes(m, bytes, 4);
}

static Boolean Put16(Merge *m, UInt16 value)
{
    UInt8 bytes[2];
    bytes[0] = (UInt8)(value >> 8);
    bytes[1] = (UInt8)value;
    return PutBytes(m, bytes, 2);
}

static Boolean PutZeros(Merge *m, UInt16 count)
{
    static const UInt8 zeros[16] = { 0 };
    while (count != 0) {
        UInt16 step = count < 16 ? count : 16;
        if (!PutBytes(m, zeros, step)) return false;
        count -= step;
    }
    return true;
}

static Boolean PutBox(Merge *m, UInt32 size, UInt32 type)
{
    return Put32(m, size) && Put32(m, type);
}

static Boolean PutFullBox(Merge *m, UInt32 size, UInt32 type, UInt32 versionFlags)
{
    return PutBox(m, size, type) && Put32(m, versionFlags);
}

/* Boxes inside a buffer */

/* Pass 0: top-level boxes of an input file */

static Boolean ScanTopLevel(Merge *m, Track *t, Boolean store,
                            UInt32 *moovOffsetP, UInt32 *moovSizeP)
{
    UInt32 offset = 0;
    UInt32 count = 0;
    while (offset + 8 <= t->fileSize) {
        UInt8 header[16];
        UInt32 size, type, headerSize = 8;
        UInt32 headLength = t->fileSize - offset >= 16 ? 16 : 8;
        if (!ReadAt(m, t->fileP, offset, header, headLength)) return false;
        size = Get32(header);
        type = Get32(header + 4);
        if (size == 1) {
            if (headLength < 16 || Get32(header + 8) != 0)
                return FAIL(m, mp4MergeUnsupported);
            size = Get32(header + 12);
            headerSize = 16;
        } else if (size == 0) {
            size = t->fileSize - offset;
        }
        if (size < headerSize || size > t->fileSize - offset)
            return FAIL(m, mp4MergeBadInput);
        if (type == FOURCC('m', 'o', 'o', 'v')) {
            *moovOffsetP = offset;
            *moovSizeP = size;
        } else if (type == FOURCC('m', 'o', 'o', 'f')) {
            if (size > MAX_MOOF_SIZE) return FAIL(m, mp4MergeUnsupported);
            if (store) {
                t->fragmentsP[count].moofOffset = offset;
                t->fragmentsP[count].moofSize = size;
            }
            count++;
        }
        offset += size;
    }
    t->fragmentCount = count;
    return true;
}

static Boolean IndexFile(Merge *m, Track *t)
{
    UInt32 moovOffset = 0, moovSize = 0;
    if (!ScanTopLevel(m, t, false, &moovOffset, &moovSize)) return false;
    if (moovSize == 0 || t->fragmentCount == 0)
        return FAIL(m, mp4MergeBadInput);
    if (moovSize > MAX_MOOV_SIZE) return FAIL(m, mp4MergeUnsupported);
    t->fragmentsP = (Fragment *)MemPtrNew(t->fragmentCount * sizeof(Fragment));
    t->moovP = (UInt8 *)MemPtrNew(moovSize);
    if (t->fragmentsP == 0 || t->moovP == 0) return FAIL(m, mp4MergeNoMemory);
    MemSet(t->fragmentsP, t->fragmentCount * sizeof(Fragment), 0);
    t->moovSize = moovSize;
    if (!ReadAt(m, t->fileP, moovOffset, t->moovP, moovSize)) return false;
    return ScanTopLevel(m, t, true, &moovOffset, &moovSize);
}

/* Track metadata from the input moov */

static Boolean ParseMoov(Merge *m, Track *t, UInt32 expectedHandler)
{
    static const UInt32 kTrak[] = { FOURCC('m', 'o', 'o', 'v'), FOURCC('t', 'r', 'a', 'k') };
    Mp4Box moov, trak, box, mdia, minf, stbl;
    const UInt8 *p;
    if (!Mp4FindBox(t->moovP, t->moovSize, kTrak[0], &moov) ||
        !Mp4FindBox(moov.payloadP, moov.payloadSize, kTrak[1], &trak))
        return FAIL(m, mp4MergeBadInput);

    if (!Mp4FindBox(trak.payloadP, trak.payloadSize, FOURCC('t', 'k', 'h', 'd'),
            &box) || box.payloadSize < 84) return FAIL(m, mp4MergeBadInput);
    p = box.payloadP;
    t->width = Get32(p + box.payloadSize - 8);
    t->height = Get32(p + box.payloadSize - 4);

    {
        static const UInt32 kElst[] = { FOURCC('e', 'd', 't', 's'), FOURCC('e', 'l', 's', 't') };
        if (Mp4FindPath(trak.payloadP, trak.payloadSize, kElst, 2, &box) &&
            box.payloadSize >= 20 && box.payloadP[0] == 0 &&
            Get32(box.payloadP + 4) >= 1) {
            UInt32 mediaTime = Get32(box.payloadP + 12);
            if (mediaTime != 0xFFFFFFFFUL) {
                t->hasEdit = true;
                t->mediaTime = mediaTime;
            }
        }
    }

    if (!Mp4FindBox(trak.payloadP, trak.payloadSize, FOURCC('m', 'd', 'i', 'a'),
            &mdia)) return FAIL(m, mp4MergeBadInput);
    if (!Mp4FindBox(mdia.payloadP, mdia.payloadSize, FOURCC('m', 'd', 'h', 'd'),
            &box) || box.payloadSize < 24) return FAIL(m, mp4MergeBadInput);
    p = box.payloadP;
    if (p[0] == 1) {
        if (box.payloadSize < 36) return FAIL(m, mp4MergeBadInput);
        t->timescale = Get32(p + 20);
        t->language = Get16(p + 32);
    } else {
        t->timescale = Get32(p + 12);
        t->language = Get16(p + 20);
    }
    if (t->timescale == 0) return FAIL(m, mp4MergeBadInput);

    if (!Mp4FindBox(mdia.payloadP, mdia.payloadSize, FOURCC('h', 'd', 'l', 'r'),
            &box) || box.payloadSize < 12 ||
        Get32(box.payloadP + 8) != expectedHandler)
        return FAIL(m, mp4MergeBadInput);
    t->hdlrP = box.startP;
    t->hdlrSize = box.size;

    if (!Mp4FindBox(mdia.payloadP, mdia.payloadSize, FOURCC('m', 'i', 'n', 'f'),
            &minf)) return FAIL(m, mp4MergeBadInput);
    if (!Mp4FindBox(minf.payloadP, minf.payloadSize, t->isVideo
            ? FOURCC('v', 'm', 'h', 'd') : FOURCC('s', 'm', 'h', 'd'), &box))
        return FAIL(m, mp4MergeBadInput);
    t->mediaHeaderP = box.startP;
    t->mediaHeaderSize = box.size;
    if (!Mp4FindBox(minf.payloadP, minf.payloadSize, FOURCC('d', 'i', 'n', 'f'),
            &box)) return FAIL(m, mp4MergeBadInput);
    t->dinfP = box.startP;
    t->dinfSize = box.size;
    if (!Mp4FindBox(minf.payloadP, minf.payloadSize, FOURCC('s', 't', 'b', 'l'),
            &stbl) ||
        !Mp4FindBox(stbl.payloadP, stbl.payloadSize, FOURCC('s', 't', 's', 'd'),
            &box)) return FAIL(m, mp4MergeBadInput);
    t->stsdP = box.startP;
    t->stsdSize = box.size;

    if (!Mp4FragReadDefaults(moov.payloadP, moov.payloadSize, &t->defaults))
        return FAIL(m, mp4MergeBadInput);
    return true;
}

/* Fragment parsing. Calls procP for every sample of the fragment and
 * records its data range and timing. */

static Boolean ParseFragment(Merge *m, Track *t, Fragment *f, SampleProc procP,
                             void *contextP)
{
    Mp4FragIter iter;
    Mp4FragSample s;
    if (!ReadAt(m, t->fileP, f->moofOffset, m->moofP, f->moofSize))
        return false;
    if (Mp4FragBegin(&iter, m->moofP, f->moofSize, f->moofOffset,
            &t->defaults) != mp4MergeOk)
        return Fail(m, iter.status, iter.line);
    f->startTime = iter.startTime;
    f->sampleCount = 0;
    f->dataSize = 0;
    while (Mp4FragNext(&iter, &s)) {
        /* Chunks must be contiguous: one fragment becomes one chunk. */
        if (f->sampleCount == 0) f->dataOffset = s.offset;
        else if (s.offset != f->dataOffset + f->dataSize)
            return FAIL(m, mp4MergeUnsupported);
        f->dataSize += s.size;
        f->sampleCount++;
        if (procP != 0 && !procP(m, t, &s, contextP)) return false;
    }
    if (iter.status != mp4MergeOk) return Fail(m, iter.status, iter.line);
    if (!iter.anyTrun || f->dataOffset + f->dataSize > t->fileSize ||
        f->dataOffset + f->dataSize < f->dataOffset)
        return FAIL(m, mp4MergeBadInput);
    return true;
}

static Boolean ForEachSample(Merge *m, Track *t, SampleProc procP,
                             void *contextP)
{
    UInt32 i;
    for (i = 0; i < t->fragmentCount; i++)
        if (!ParseFragment(m, t, &t->fragmentsP[i], procP, contextP))
            return false;
    return true;
}

/* Run-length tables. With write false only the entries are counted. */

static Boolean RleAdd(Merge *m, Rle *r, UInt32 value, Boolean write)
{
    if (r->count != 0 && value == r->value) {
        r->count++;
        return true;
    }
    if (r->count != 0) {
        r->entries++;
        if (write && !(Put32(m, r->count) && Put32(m, r->value))) return false;
    }
    r->value = value;
    r->count = 1;
    return true;
}

static void RleCountFinish(Rle *r)
{
    if (r->count == 0) return;
    r->entries++;
    r->count = 0;
}

typedef struct Analysis {
    Rle stts;
    Rle ctts;
    UInt32 syncCount;
    UInt32 decodeTime;
    UInt32 presentationEnd;
    Boolean anyNegative;
    Boolean anyCto;
} Analysis;

static Boolean AnalyzeSample(Merge *m, Track *t, const Mp4FragSample *s,
                             void *contextP)
{
    Analysis *a = (Analysis *)contextP;
    /* Unsigned wrap-around also handles negative signed offsets. */
    UInt32 end = a->decodeTime + s->cto + s->duration;
    if (end > a->presentationEnd) a->presentationEnd = end;
    a->decodeTime += s->duration;
    t->sampleCount++;
    t->duration += s->duration;
    RleAdd(m, &a->stts, s->duration, false);
    RleAdd(m, &a->ctts, s->cto, false);
    if (s->cto != 0) a->anyCto = true;
    if (s->ctoSigned && (s->cto & 0x80000000UL)) a->anyNegative = true;
    if ((s->flags & MP4_SAMPLE_NON_SYNC) == 0) a->syncCount++;
    return true;
}

static Boolean AnalyzeTrack(Merge *m, Track *t)
{
    Analysis a;
    UInt32 i, previous = 0;
    MemSet(&a, sizeof(a), 0);
    for (i = 0; i < t->fragmentCount; i++) {
        Fragment *f = &t->fragmentsP[i];
        UInt32 expectedStart = t->duration;
        if (!ParseFragment(m, t, f, AnalyzeSample, &a)) return false;
        /* Without tfdt, fragments follow each other. */
        if (f->startTime == 0 && i != 0) f->startTime = expectedStart;
        if (f->sampleCount == 0) return FAIL(m, mp4MergeBadInput);
        if (i == 0 || f->sampleCount != previous) t->stscEntries++;
        previous = f->sampleCount;
    }
    RleCountFinish(&a.stts);
    RleCountFinish(&a.ctts);
    t->sttsEntries = a.stts.entries;
    t->needCtts = a.anyCto;
    t->signedCtts = a.anyNegative;
    t->cttsEntries = a.anyCto ? a.ctts.entries : 0;
    t->stssEntries = a.syncCount == t->sampleCount ? 0 : a.syncCount;
    if (a.syncCount == 0) return FAIL(m, mp4MergeBadInput);
    /* The edit list covers presentation times, which B-frames push past
     * the last decode time. */
    if (t->mediaTime >= a.presentationEnd) t->hasEdit = false;
    t->trackDuration = t->hasEdit
        ? ToMovieTime(a.presentationEnd - t->mediaTime, t->timescale)
        : ToMovieTime(t->duration, t->timescale);
    return true;
}

/* Sizes of the output boxes */

static void SizeTrack(Track *t)
{
    t->stblSize = 8 + t->stsdSize + 16 + 8 * t->sttsEntries +
        (t->needCtts ? 16 + 8 * t->cttsEntries : 0) +
        (t->stssEntries != 0 ? 16 + 4 * t->stssEntries : 0) +
        16 + 12 * t->stscEntries + 20 + 4 * t->sampleCount +
        16 + 4 * t->fragmentCount;
    t->minfSize = 8 + t->mediaHeaderSize + t->dinfSize + t->stblSize;
    t->mdiaSize = 8 + 32 + t->hdlrSize + t->minfSize;
    t->trakSize = 8 + 92 + (t->hasEdit ? 36 : 0) + t->mdiaSize;
}

static UInt32 FragmentMillis(const Track *t, UInt32 index)
{
    return ToMovieTime(t->fragmentsP[index].startTime, t->timescale);
}

/* Assigns output offsets with chunks interleaved by start time. */
static UInt32 LayoutChunks(Merge *m, UInt32 offset)
{
    Track *v = &m->tracks[0], *a = &m->tracks[1];
    UInt32 vi = 0, ai = 0;
    while (vi < v->fragmentCount || ai < a->fragmentCount) {
        Fragment *f;
        if (ai >= a->fragmentCount ||
            (vi < v->fragmentCount &&
             FragmentMillis(v, vi) <= FragmentMillis(a, ai)))
            f = &v->fragmentsP[vi++];
        else
            f = &a->fragmentsP[ai++];
        f->outOffset = offset;
        offset += f->dataSize;
    }
    return offset;
}

/* Writing */

static const UInt8 kMatrix[36] = {
    0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0, 0, 0
};

static Boolean WriteFtyp(Merge *m)
{
    return PutBox(m, FTYP_SIZE, FOURCC('f', 't', 'y', 'p')) &&
        Put32(m, FOURCC('i', 's', 'o', 'm')) && Put32(m, 0x200) &&
        Put32(m, FOURCC('i', 's', 'o', 'm')) &&
        Put32(m, FOURCC('i', 's', 'o', '2')) &&
        Put32(m, FOURCC('a', 'v', 'c', '1')) &&
        Put32(m, FOURCC('m', 'p', '4', '1'));
}

static Boolean WriteMvhd(Merge *m, UInt32 duration)
{
    return PutFullBox(m, 108, FOURCC('m', 'v', 'h', 'd'), 0) &&
        Put32(m, 0) && Put32(m, 0) && Put32(m, MOVIE_TIMESCALE) &&
        Put32(m, duration) && Put32(m, 0x00010000UL) && Put16(m, 0x0100) &&
        PutZeros(m, 10) && PutBytes(m, kMatrix, 36) && PutZeros(m, 24) &&
        Put32(m, 3);
}

static Boolean WriteTkhd(Merge *m, const Track *t, UInt32 trackId)
{
    return PutFullBox(m, 92, FOURCC('t', 'k', 'h', 'd'), 3) &&
        Put32(m, 0) && Put32(m, 0) && Put32(m, trackId) && Put32(m, 0) &&
        Put32(m, t->trackDuration) && PutZeros(m, 8) && Put16(m, 0) &&
        Put16(m, 0) && Put16(m, t->isVideo ? 0 : 0x0100) &&
        Put16(m, 0) && PutBytes(m, kMatrix, 36) &&
        Put32(m, t->isVideo ? t->width : 0) &&
        Put32(m, t->isVideo ? t->height : 0);
}

static Boolean WriteEdts(Merge *m, const Track *t)
{
    if (!t->hasEdit) return true;
    return PutBox(m, 36, FOURCC('e', 'd', 't', 's')) &&
        PutFullBox(m, 28, FOURCC('e', 'l', 's', 't'), 0) && Put32(m, 1) &&
        Put32(m, t->trackDuration) && Put32(m, t->mediaTime) &&
        Put32(m, 0x00010000UL);
}

static Boolean WriteMdhd(Merge *m, const Track *t)
{
    return PutFullBox(m, 32, FOURCC('m', 'd', 'h', 'd'), 0) &&
        Put32(m, 0) && Put32(m, 0) && Put32(m, t->timescale) &&
        Put32(m, t->duration) && Put16(m, (UInt16)t->language) &&
        Put16(m, 0);
}

typedef struct TableWriter {
    Rle rle;
    UInt32 sampleNumber;
    UInt16 table;
} TableWriter;

enum { tableStts = 0, tableCtts, tableStss, tableStsz };

static Boolean WriteTableSample(Merge *m, Track *t, const Mp4FragSample *s,
                                void *contextP)
{
    TableWriter *w = (TableWriter *)contextP;
    (void)t;
    w->sampleNumber++;
    switch (w->table) {
        case tableStts: return RleAdd(m, &w->rle, s->duration, true);
        case tableCtts: return RleAdd(m, &w->rle, s->cto, true);
        case tableStss:
            return (s->flags & MP4_SAMPLE_NON_SYNC) != 0 ||
                Put32(m, w->sampleNumber);
        default: return Put32(m, s->size);
    }
}

static Boolean WriteRleFinish(Merge *m, Rle *r)
{
    if (r->count == 0) return true;
    return Put32(m, r->count) && Put32(m, r->value);
}

static Boolean WriteTable(Merge *m, Track *t, UInt16 table)
{
    TableWriter w;
    MemSet(&w, sizeof(w), 0);
    w.table = table;
    if (!ForEachSample(m, t, WriteTableSample, &w)) return false;
    return table == tableStts || table == tableCtts
        ? WriteRleFinish(m, &w.rle) : true;
}

static Boolean WriteStbl(Merge *m, Track *t)
{
    UInt32 i, previous = 0;
    if (!PutBox(m, t->stblSize, FOURCC('s', 't', 'b', 'l')) ||
        !PutBytes(m, t->stsdP, t->stsdSize)) return false;
    if (!PutFullBox(m, 16 + 8 * t->sttsEntries, FOURCC('s', 't', 't', 's'), 0) ||
        !Put32(m, t->sttsEntries) || !WriteTable(m, t, tableStts)) return false;
    if (t->needCtts &&
        (!PutFullBox(m, 16 + 8 * t->cttsEntries, FOURCC('c', 't', 't', 's'),
            t->signedCtts ? 0x01000000UL : 0) ||
         !Put32(m, t->cttsEntries) || !WriteTable(m, t, tableCtts)))
        return false;
    if (t->stssEntries != 0 &&
        (!PutFullBox(m, 16 + 4 * t->stssEntries, FOURCC('s', 't', 's', 's'), 0) ||
         !Put32(m, t->stssEntries) || !WriteTable(m, t, tableStss)))
        return false;
    if (!PutFullBox(m, 16 + 12 * t->stscEntries, FOURCC('s', 't', 's', 'c'), 0) ||
        !Put32(m, t->stscEntries)) return false;
    for (i = 0; i < t->fragmentCount; i++) {
        UInt32 count = t->fragmentsP[i].sampleCount;
        if (i != 0 && count == previous) continue;
        if (!Put32(m, i + 1) || !Put32(m, count) || !Put32(m, 1)) return false;
        previous = count;
    }
    if (!PutFullBox(m, 20 + 4 * t->sampleCount, FOURCC('s', 't', 's', 'z'), 0) ||
        !Put32(m, 0) || !Put32(m, t->sampleCount) ||
        !WriteTable(m, t, tableStsz)) return false;
    if (!PutFullBox(m, 16 + 4 * t->fragmentCount, FOURCC('s', 't', 'c', 'o'), 0) ||
        !Put32(m, t->fragmentCount)) return false;
    for (i = 0; i < t->fragmentCount; i++)
        if (!Put32(m, t->fragmentsP[i].outOffset)) return false;
    return true;
}

static Boolean WriteTrak(Merge *m, Track *t, UInt32 trackId)
{
    UInt32 before = m->written;
    if (!PutBox(m, t->trakSize, FOURCC('t', 'r', 'a', 'k')) ||
        !WriteTkhd(m, t, trackId) || !WriteEdts(m, t) ||
        !PutBox(m, t->mdiaSize, FOURCC('m', 'd', 'i', 'a')) ||
        !WriteMdhd(m, t) || !PutBytes(m, t->hdlrP, t->hdlrSize) ||
        !PutBox(m, t->minfSize, FOURCC('m', 'i', 'n', 'f')) ||
        !PutBytes(m, t->mediaHeaderP, t->mediaHeaderSize) ||
        !PutBytes(m, t->dinfP, t->dinfSize) || !WriteStbl(m, t))
        return false;
    /* The tables were sized in the analysis pass; they must agree. */
    if (m->written - before != t->trakSize) return FAIL(m, mp4MergeBadInput);
    return true;
}

static Boolean CopyChunk(Merge *m, const Track *t, const Fragment *f)
{
    UInt32 offset = f->dataOffset;
    UInt32 left = f->dataSize;
    if (!Flush(m)) return false;
    while (left != 0) {
        UInt32 step = left < COPY_BUFFER_SIZE ? left : COPY_BUFFER_SIZE;
        if (!ReadAt(m, t->fileP, offset, m->copyP, step)) return false;
        if (m->ioP->writeProcP(m->ioP->outP, m->copyP, step) != errNone)
            return FAIL(m, mp4MergeWriteError);
        m->written += step;
        offset += step;
        left -= step;
        m->copyDone += step;
        if (m->ioP->progressProcP != 0 &&
            !m->ioP->progressProcP(m->ioP->contextP, m->copyDone, m->copyTotal))
            return FAIL(m, mp4MergeCancelled);
    }
    return true;
}

static Boolean WriteMdat(Merge *m, UInt32 dataSize)
{
    Track *v = &m->tracks[0], *a = &m->tracks[1];
    UInt32 vi = 0, ai = 0;
    if (!PutBox(m, 8 + dataSize, FOURCC('m', 'd', 'a', 't'))) return false;
    /* Same order as LayoutChunks. */
    while (vi < v->fragmentCount || ai < a->fragmentCount) {
        Boolean takeVideo = ai >= a->fragmentCount ||
            (vi < v->fragmentCount &&
             FragmentMillis(v, vi) <= FragmentMillis(a, ai));
        Track *t = takeVideo ? v : a;
        Fragment *f = takeVideo ? &v->fragmentsP[vi++] : &a->fragmentsP[ai++];
        if (f->outOffset != m->written) return FAIL(m, mp4MergeBadInput);
        if (!CopyChunk(m, t, f)) return false;
    }
    return Flush(m);
}

static Boolean Run(Merge *m)
{
    Track *v = &m->tracks[0], *a = &m->tracks[1];
    UInt32 maxMoof = 0, moovSize, dataStart, end, i, k;
    for (k = 0; k < 2; k++) {
        Track *t = &m->tracks[k];
        if (!IndexFile(m, t) || !ParseMoov(m, t, t->isVideo
                ? FOURCC('v', 'i', 'd', 'e') : FOURCC('s', 'o', 'u', 'n')))
            return false;
        for (i = 0; i < t->fragmentCount; i++)
            if (t->fragmentsP[i].moofSize > maxMoof)
                maxMoof = t->fragmentsP[i].moofSize;
    }
    m->moofP = (UInt8 *)MemPtrNew(maxMoof);
    m->copyP = (UInt8 *)MemPtrNew(COPY_BUFFER_SIZE);
    if (m->moofP == 0 || m->copyP == 0) return FAIL(m, mp4MergeNoMemory);
    if (!AnalyzeTrack(m, v) || !AnalyzeTrack(m, a)) return false;
    a->hasEdit = false;
    SizeTrack(v);
    SizeTrack(a);
    moovSize = 8 + 108 + v->trakSize + a->trakSize;
    dataStart = FTYP_SIZE + moovSize + 8;
    end = LayoutChunks(m, dataStart);
    if (end < dataStart) return FAIL(m, mp4MergeUnsupported);
    m->copyTotal = end - dataStart;
    if (!WriteFtyp(m) || !PutBox(m, moovSize, FOURCC('m', 'o', 'o', 'v')) ||
        !WriteMvhd(m, v->trackDuration > a->trackDuration
            ? v->trackDuration : a->trackDuration) ||
        !WriteTrak(m, v, 1) || !WriteTrak(m, a, 2) ||
        !WriteMdat(m, end - dataStart)) return false;
    m->resultP->videoSamples = v->sampleCount;
    m->resultP->audioSamples = a->sampleCount;
    m->resultP->outputSize = m->written;
    return true;
}

void Mp4Merge(const Mp4Io *ioP, Mp4MergeResult *resultP)
{
    Merge *m;
    UInt16 k;
    MemSet(resultP, sizeof(*resultP), 0);
    m = (Merge *)MemPtrNew(sizeof(Merge));
    if (m == 0) {
        resultP->status = mp4MergeNoMemory;
        return;
    }
    MemSet(m, sizeof(*m), 0);
    m->ioP = ioP;
    m->resultP = resultP;
    m->tracks[0].fileP = ioP->videoP;
    m->tracks[0].fileSize = ioP->videoSize;
    m->tracks[0].isVideo = true;
    m->tracks[1].fileP = ioP->audioP;
    m->tracks[1].fileSize = ioP->audioSize;
    Run(m);
    for (k = 0; k < 2; k++) {
        if (m->tracks[k].fragmentsP != 0) MemPtrFree(m->tracks[k].fragmentsP);
        if (m->tracks[k].moovP != 0) MemPtrFree(m->tracks[k].moovP);
    }
    if (m->moofP != 0) MemPtrFree(m->moofP);
    if (m->copyP != 0) MemPtrFree(m->copyP);
    MemPtrFree(m);
}
