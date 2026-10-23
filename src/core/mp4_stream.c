/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "mp4_stream.h"

enum {
    stateBox = 0,       /* at the header of the box at next */
    stateSkip,          /* dropping bytes up to skipEnd */
    stateSamples        /* in the mdat of the current fragment */
};

/* Internal result: parsing can go on. */
#define CONTINUE 0xFFFF

static UInt16 Fail(Mp4Stream *s, UInt16 status, UInt32 line)
{
    if (s->status == mp4MergeOk) {
        s->status = status;
        s->detail = line;
    }
    return mp4StreamError;
}

#define FAIL(s, status) Fail((s), (status), __LINE__)

UInt16 Mp4StreamOpen(Mp4Stream *s, UInt8 *ringP, UInt32 capacity,
                     UInt32 totalSize, Mp4ReadTrack *trackP)
{
    MemSet(s, sizeof(*s), 0);
    s->ringP = ringP;
    s->capacity = capacity;
    s->totalSize = totalSize;
    s->trackP = trackP;
    s->boxP = (UInt8 *)MemPtrNew(MP4_STREAM_BOX_CAPACITY);
    return s->boxP != 0 ? mp4MergeOk : mp4MergeNoMemory;
}

void Mp4StreamClose(Mp4Stream *s)
{
    if (s->boxP != 0) MemPtrFree(s->boxP);
    if (s->segmentsP != 0) MemPtrFree(s->segmentsP);
    s->boxP = 0;
    s->segmentsP = 0;
    s->segmentCount = 0;
}

/* The ring */

UInt32 Mp4StreamBuffered(const Mp4Stream *s)
{
    return s->writeOffset - s->readOffset;
}

UInt32 Mp4StreamSpace(const Mp4Stream *s)
{
    return s->capacity - Mp4StreamBuffered(s);
}

Boolean Mp4StreamReceived(const Mp4Stream *s)
{
    return s->writeOffset >= s->totalSize;
}

static UInt32 RingIndex(const Mp4Stream *s, UInt32 offset)
{
    UInt32 index = s->head + (offset - s->readOffset);
    return index >= s->capacity ? index - s->capacity : index;
}

void Mp4StreamWrite(Mp4Stream *s, const UInt8 *dataP, UInt32 length)
{
    UInt32 index = RingIndex(s, s->writeOffset);
    UInt32 first = s->capacity - index;
    if (length > Mp4StreamSpace(s)) length = Mp4StreamSpace(s);
    if (first > length) first = length;
    MemMove(s->ringP + index, dataP, (Int32)first);
    if (length > first) MemMove(s->ringP, dataP + first, (Int32)(length - first));
    s->writeOffset += length;
}

/* Copies [offset, offset + length), which must be in the ring. */
static void Copy(const Mp4Stream *s, UInt32 offset, UInt8 *dstP, UInt32 length)
{
    UInt32 index = RingIndex(s, offset);
    UInt32 first = s->capacity - index;
    if (first > length) first = length;
    MemMove(dstP, s->ringP + index, (Int32)first);
    if (length > first) MemMove(dstP + first, s->ringP, (Int32)(length - first));
}

/* Drops the bytes before offset, or all of them if it has not arrived. */
static void Drop(Mp4Stream *s, UInt32 offset)
{
    UInt32 count;
    if (offset > s->writeOffset) offset = s->writeOffset;
    if (offset <= s->readOffset) return;
    count = offset - s->readOffset;
    s->head += count;
    if (s->head >= s->capacity) s->head -= s->capacity;
    s->readOffset = offset;
}

/* Boxes */

static Err ReadMemory(void *fileP, UInt32 offset, void *bufferP, UInt32 length)
{
    MemMove(bufferP, (const UInt8 *)fileP + offset, (Int32)length);
    return errNone;
}

static UInt16 ReadMoov(Mp4Stream *s, UInt32 size, UInt32 headerSize)
{
    Mp4Reader *readerP = (Mp4Reader *)MemPtrNew(sizeof(Mp4Reader));
    UInt16 status;
    if (readerP == 0) return FAIL(s, mp4MergeNoMemory);
    status = Mp4ReaderOpen(readerP, ReadMemory, s->boxP, size);
    if (status == mp4MergeOk)
        *s->trackP = readerP->video.present ? readerP->video : readerP->audio;
    else
        Fail(s, status, readerP->detail);
    MemPtrFree(readerP);
    if (status != mp4MergeOk) return mp4StreamError;
    if (!Mp4FragReadDefaults(s->boxP + headerSize, size - headerSize,
            &s->defaults))
        return FAIL(s, mp4MergeBadInput);
    s->ready = true;
    return CONTINUE;
}

/* value * to / from, without overflow for 32-bit times. */
static UInt32 Rescale(UInt32 value, UInt32 from, UInt32 to)
{
    return from == to ? value : value / from * to + value % from * to / from;
}

/* Reads the sidx at next, all of which has arrived. Without memory, or in
 * a form it does not handle, the stream just plays without one. */
static void ReadSidx(Mp4Stream *s, UInt32 size, UInt32 headerSize)
{
    UInt8 field[12];
    UInt32 p = s->next + headerSize, end = s->next + size, scale, time, offset;
    UInt16 count, i;
    Boolean wide;
    if (size < headerSize + 24 || s->trackP->timescale == 0) return;
    Copy(s, p, field, 12);
    wide = field[0] != 0;
    if (wide && size < headerSize + 32) return;
    scale = Mp4Get32(field + 8);
    p += 12;
    Copy(s, p, field, 8);
    if (wide) {
        if (Mp4Get32(field) != 0) return;
        time = Mp4Get32(field + 4);
        Copy(s, p + 8, field, 8);
        if (Mp4Get32(field) != 0) return;
        offset = Mp4Get32(field + 4);
        p += 16;
    } else {
        time = Mp4Get32(field);
        offset = Mp4Get32(field + 4);
        p += 8;
    }
    Copy(s, p, field, 4);
    count = (UInt16)Mp4Get32(field);
    p += 4;
    if (scale == 0 || count == 0 || p + 12UL * count > end) return;
    s->segmentsP = (Mp4Segment *)MemPtrNew(sizeof(Mp4Segment) * count);
    if (s->segmentsP == 0) return;
    /* Offsets count from the first byte after the sidx. */
    offset += end;
    for (i = 0; i < count; i++, p += 12) {
        UInt32 reference;
        Copy(s, p, field, 12);
        reference = Mp4Get32(field);
        /* A reference to another sidx */
        if ((reference & 0x80000000UL) != 0) break;
        s->segmentsP[i].offset = offset;
        s->segmentsP[i].time = Rescale(time, scale, s->trackP->timescale);
        offset += reference;
        time += Mp4Get32(field + 4);
    }
    if (i < count || offset > s->totalSize) {
        MemPtrFree(s->segmentsP);
        s->segmentsP = 0;
        return;
    }
    s->segmentCount = count;
}

/* Handles the box at next. */
static UInt16 ReadBox(Mp4Stream *s)
{
    UInt8 header[16];
    UInt32 size, type, headerSize = 8, available;
    if (s->next >= s->totalSize) return mp4StreamEnd;
    available = s->writeOffset - s->next;
    if (available < 8) return mp4StreamWait;
    Copy(s, s->next, header, 8);
    size = Mp4Get32(header);
    type = Mp4Get32(header + 4);
    if (size == 1) {
        if (available < 16) return mp4StreamWait;
        Copy(s, s->next, header, 16);
        if (Mp4Get32(header + 8) != 0) return FAIL(s, mp4MergeUnsupported);
        size = Mp4Get32(header + 12);
        headerSize = 16;
    } else if (size == 0) {
        size = s->totalSize - s->next;
    }
    if (size < headerSize || size > s->totalSize - s->next)
        return FAIL(s, mp4MergeBadInput);

    if (type == MP4_FOURCC('m', 'o', 'o', 'v') ||
        type == MP4_FOURCC('m', 'o', 'o', 'f')) {
        Boolean moov = type == MP4_FOURCC('m', 'o', 'o', 'v');
        UInt32 start = s->next;
        if (!moov && !s->ready) return FAIL(s, mp4MergeBadInput);
        if (size > MP4_STREAM_BOX_CAPACITY || size > s->capacity)
            return FAIL(s, mp4MergeUnsupported);
        if (available < size) return mp4StreamWait;
        Copy(s, start, s->boxP, size);
        s->next += size;
        Drop(s, s->next);
        if (moov) return ReadMoov(s, size, headerSize);
        if (Mp4FragBegin(&s->iter, s->boxP, size, start, &s->defaults) !=
                mp4MergeOk)
            return Fail(s, s->iter.status, s->iter.line);
        /* Without tfdt, fragments follow each other. */
        if (s->iter.hasStartTime) s->dts = s->iter.startTime;
        s->inFragment = true;
        s->havePending = false;
        return CONTINUE;
    }
    if (type == MP4_FOURCC('m', 'd', 'a', 't') && s->inFragment) {
        s->mdatEnd = s->next + size;
        s->next += headerSize;
        Drop(s, s->next);
        s->state = stateSamples;
        return CONTINUE;
    }
    if (type == MP4_FOURCC('s', 'i', 'd', 'x') && s->ready &&
        s->segmentsP == 0 && size <= s->capacity) {
        if (available < size) return mp4StreamWait;
        ReadSidx(s, size, headerSize);
    }
    s->skipEnd = s->next + size;
    s->state = stateSkip;
    return CONTINUE;
}

/* Copies out the fragment's next sample once it has arrived. */
static UInt16 ReadSample(Mp4Stream *s, Mp4Sample *sampleP, UInt8 *bufferP,
                         UInt32 capacity)
{
    Mp4FragSample *f = &s->pending;
    if (!s->havePending) {
        if (!Mp4FragNext(&s->iter, f)) {
            if (s->iter.status != mp4MergeOk)
                return Fail(s, s->iter.status, s->iter.line);
            s->inFragment = false;
            s->skipEnd = s->mdatEnd;
            s->state = stateSkip;
            return CONTINUE;
        }
        if (f->offset < s->readOffset || f->offset + f->size > s->mdatEnd ||
            f->offset + f->size < f->offset)
            return FAIL(s, mp4MergeUnsupported);
        if (f->size > capacity || f->size > s->capacity)
            return FAIL(s, mp4MergeUnsupported);
        s->havePending = true;
    }
    Drop(s, f->offset);
    if (s->writeOffset < f->offset + f->size) return mp4StreamWait;
    Copy(s, f->offset, bufferP, f->size);
    Drop(s, f->offset + f->size);
    sampleP->offset = f->offset;
    sampleP->size = f->size;
    sampleP->dts = s->dts;
    sampleP->cto = (Int32)f->cto;
    sampleP->sync = (f->flags & MP4_SAMPLE_NON_SYNC) == 0;
    s->dts += f->duration;
    s->havePending = false;
    return mp4StreamSample;
}

/* Parses until a sample is out, or more bytes are needed. Without bufferP,
 * stops once the moov has been read. */
static UInt16 Advance(Mp4Stream *s, Mp4Sample *sampleP, UInt8 *bufferP,
                      UInt32 capacity)
{
    for (;;) {
        UInt16 result;
        if (s->status != mp4MergeOk) return mp4StreamError;
        if (bufferP == 0 && s->ready) return mp4StreamWait;
        switch (s->state) {
            case stateSkip:
                Drop(s, s->skipEnd);
                if (s->readOffset < s->skipEnd) return mp4StreamWait;
                s->next = s->skipEnd;
                s->state = stateBox;
                continue;
            case stateBox:
                result = ReadBox(s);
                break;
            default:
                result = ReadSample(s, sampleP, bufferP, capacity);
                break;
        }
        if (result != CONTINUE) return result;
    }
}

Boolean Mp4StreamReady(Mp4Stream *s)
{
    if (!s->ready) Advance(s, 0, 0, 0);
    return s->ready;
}

UInt16 Mp4StreamNext(Mp4Stream *s, Mp4Sample *sampleP, UInt8 *bufferP,
                     UInt32 capacity)
{
    return Advance(s, sampleP, bufferP, capacity);
}

UInt16 Mp4StreamFindSegment(const Mp4Stream *s, UInt32 time)
{
    UInt16 low = 0, high = s->segmentCount;
    while (high - low > 1) {
        UInt16 middle = (UInt16)((low + high) / 2);
        if (s->segmentsP[middle].time <= time) low = middle;
        else high = middle;
    }
    return low;
}

Boolean Mp4StreamSeek(Mp4Stream *s, UInt16 segment)
{
    const Mp4Segment *g;
    if (!s->ready || s->status != mp4MergeOk || segment >= s->segmentCount)
        return false;
    g = &s->segmentsP[segment];
    s->head = 0;
    s->readOffset = g->offset;
    s->writeOffset = g->offset;
    s->next = g->offset;
    s->state = stateBox;
    s->inFragment = false;
    s->havePending = false;
    /* Close enough when there is no tfdt to say. */
    s->dts = g->time;
    return true;
}
