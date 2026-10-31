/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "mp4_read.h"

#define FOURCC(a, b, c, d) (((UInt32)(a) << 24) | ((UInt32)(b) << 16) | \
                            ((UInt32)(c) << 8) | (UInt32)(d))
#define NO_CHUNK 0xFFFFFFFFUL
#define STSD_BUFFER 384

static UInt32 Get32(const UInt8 *p)
{
    return ((UInt32)p[0] << 24) | ((UInt32)p[1] << 16) |
        ((UInt32)p[2] << 8) | (UInt32)p[3];
}

static UInt16 Get16(const UInt8 *p)
{
    return (UInt16)(((UInt16)p[0] << 8) | p[1]);
}

static Boolean Fail(Mp4Reader *r, UInt16 status, UInt32 line)
{
    if (r->status == mp4MergeOk) {
        r->status = status;
        r->detail = line;
    }
    return false;
}

#define FAIL(r, status) Fail((r), (status), __LINE__)

static Boolean ReadAt(Mp4Reader *r, UInt32 offset, void *bufferP, UInt32 length)
{
    if (offset > r->fileSize || length > r->fileSize - offset)
        return FAIL(r, mp4MergeBadInput);
    if (r->readProcP(r->fileP, offset, bufferP, length) != errNone)
        return FAIL(r, mp4MergeReadError);
    return true;
}

/* Box walking in the file. A box spans [*startP, *endP); payload begins at
 * *payloadP. */
static Boolean FindBox(Mp4Reader *r, UInt32 start, UInt32 end, UInt32 type,
                       UInt32 *payloadP, UInt32 *endP)
{
    while (start + 8 <= end) {
        UInt8 header[16];
        UInt32 size, headerSize;
        UInt32 headLength = end - start >= 16 ? 16 : 8;
        if (!ReadAt(r, start, header, headLength)) return false;
        size = Get32(header);
        headerSize = 8;
        if (size == 1) {
            if (headLength < 16 || Get32(header + 8) != 0)
                return FAIL(r, mp4MergeUnsupported);
            size = Get32(header + 12);
            headerSize = 16;
        } else if (size == 0) {
            size = end - start;
        }
        if (size < headerSize || size > end - start)
            return FAIL(r, mp4MergeBadInput);
        if (Get32(header + 4) == type) {
            *payloadP = start + headerSize;
            *endP = start + size;
            return true;
        }
        start += size;
    }
    return false;
}

static Boolean FindPath(Mp4Reader *r, UInt32 start, UInt32 end,
                        const UInt32 *typesP, UInt16 count, UInt32 *payloadP,
                        UInt32 *endP)
{
    UInt16 i;
    for (i = 0; i < count; i++) {
        if (!FindBox(r, start, end, typesP[i], &start, &end)) return false;
    }
    *payloadP = start;
    *endP = end;
    return true;
}

/* Sample tables, read sequentially through a small buffer */

static Boolean SetTable(Mp4Reader *r, Mp4TableReader *t, UInt32 payload,
                        UInt32 end, UInt32 countOffset, UInt16 entrySize)
{
    UInt8 bytes[4];
    MemSet(t, sizeof(*t), 0);
    if (!ReadAt(r, payload + countOffset, bytes, 4)) return false;
    t->count = Get32(bytes);
    t->offset = payload + countOffset + 4;
    t->entrySize = entrySize;
    if (t->count > (end - t->offset) / entrySize)
        return FAIL(r, mp4MergeBadInput);
    return true;
}

static const UInt8 *TableNext(Mp4Reader *r, Mp4TableReader *t)
{
    UInt32 index = t->next;
    if (index >= t->count) {
        FAIL(r, mp4MergeBadInput);
        return 0;
    }
    if (t->bufferCount == 0 || index < t->bufferFirst ||
        index - t->bufferFirst >= t->bufferCount) {
        UInt32 count = MP4_TABLE_BUFFER / t->entrySize;
        if (count > t->count - index) count = t->count - index;
        if (!ReadAt(r, t->offset + index * t->entrySize, t->buffer,
                count * t->entrySize)) return 0;
        t->bufferFirst = index;
        t->bufferCount = (UInt16)count;
    }
    t->next++;
    return t->buffer + (index - t->bufferFirst) * t->entrySize;
}

/* Sample descriptions */

static UInt32 DescriptorLength(const UInt8 **pP, const UInt8 *endP)
{
    UInt32 length = 0;
    UInt16 i;
    for (i = 0; i < 4 && *pP < endP; i++) {
        UInt8 b = *(*pP)++;
        length = (length << 7) | (b & 0x7F);
        if ((b & 0x80) == 0) break;
    }
    return length;
}

/* AudioSpecificConfig from an esds box payload. */
static Boolean ParseEsds(Mp4ReadTrack *t, const UInt8 *p, const UInt8 *endP)
{
    UInt32 length;
    p += 4;
    if (p >= endP || *p++ != 0x03) return false;
    length = DescriptorLength(&p, endP);
    if (p + 3 > endP) return false;
    {
        UInt8 flags = p[2];
        p += 3;
        if (flags & 0x80) p += 2;
        if ((flags & 0x40) && p < endP) p += 1 + *p;
        if (flags & 0x20) p += 2;
    }
    (void)length;
    if (p >= endP || *p++ != 0x04) return false;
    DescriptorLength(&p, endP);
    p += 13;
    if (p >= endP || *p++ != 0x05) return false;
    length = DescriptorLength(&p, endP);
    if (length > MP4_CONFIG_CAPACITY || p + length > endP) return false;
    MemMove(t->config, p, length);
    t->configLength = (UInt16)length;
    return true;
}

static Boolean ParseStsd(Mp4Reader *r, Mp4ReadTrack *t, UInt32 payload,
                         UInt32 end, Boolean video)
{
    UInt8 buffer[STSD_BUFFER];
    UInt32 length = end - payload < STSD_BUFFER ? end - payload : STSD_BUFFER;
    const UInt8 *entryP = buffer + 8, *endP, *p;
    UInt32 entrySize, type, childStart;
    if (length < 16 || !ReadAt(r, payload, buffer, length)) return false;
    entrySize = Get32(entryP);
    type = Get32(entryP + 4);
    endP = entryP + entrySize;
    if (endP > buffer + length) endP = buffer + length;
    if (video) {
        if (type != FOURCC('a', 'v', 'c', '1')) return FAIL(r, mp4MergeUnsupported);
        if (endP < entryP + 86) return FAIL(r, mp4MergeBadInput);
        t->width = Get16(entryP + 32);
        t->height = Get16(entryP + 34);
        childStart = 86;
    } else {
        if (type != FOURCC('m', 'p', '4', 'a')) return FAIL(r, mp4MergeUnsupported);
        if (endP < entryP + 36) return FAIL(r, mp4MergeBadInput);
        t->channels = Get16(entryP + 24);
        t->sampleRate = Get16(entryP + 32);
        childStart = 36;
    }
    for (p = entryP + childStart; p + 8 <= endP; ) {
        UInt32 size = Get32(p);
        if (size < 8 || p + size > endP) break;
        if (video && Get32(p + 4) == FOURCC('a', 'v', 'c', 'C')) {
            if (size - 8 > MP4_CONFIG_CAPACITY) return FAIL(r, mp4MergeUnsupported);
            MemMove(t->config, p + 8, size - 8);
            t->configLength = (UInt16)(size - 8);
            return true;
        }
        if (!video && Get32(p + 4) == FOURCC('e', 's', 'd', 's'))
            return ParseEsds(t, p + 8, p + size) ? true
                : FAIL(r, mp4MergeUnsupported);
        p += size;
    }
    return FAIL(r, mp4MergeUnsupported);
}

static Boolean StartIteration(Mp4Reader *r, Mp4ReadTrack *t)
{
    const UInt8 *e;
    if ((e = TableNext(r, &t->stsc)) == 0) return false;
    t->stscSamples = Get32(e + 4);
    t->nextStscChunk = NO_CHUNK;
    if (t->stsc.next < t->stsc.count) {
        if ((e = TableNext(r, &t->stsc)) == 0) return false;
        t->nextStscChunk = Get32(e);
        t->stscPending = Get32(e + 4);
    }
    if (t->hasStss) {
        if (t->stss.count == 0) t->hasStss = false;
        else if ((e = TableNext(r, &t->stss)) == 0) return false;
        else t->nextSync = Get32(e);
    }
    return true;
}

static Boolean ParseTrack(Mp4Reader *r, UInt32 trak, UInt32 trakEnd)
{
    static const UInt32 kHdlr[] = { FOURCC('m', 'd', 'i', 'a'), FOURCC('h', 'd', 'l', 'r') };
    static const UInt32 kMdhd[] = { FOURCC('m', 'd', 'i', 'a'), FOURCC('m', 'd', 'h', 'd') };
    static const UInt32 kStbl[] = { FOURCC('m', 'd', 'i', 'a'), FOURCC('m', 'i', 'n', 'f'),
        FOURCC('s', 't', 'b', 'l') };
    UInt8 bytes[32];
    UInt32 payload, end, stbl, stblEnd, handler;
    Mp4ReadTrack *t;
    Boolean video;
    if (!FindPath(r, trak, trakEnd, kHdlr, 2, &payload, &end) ||
        !ReadAt(r, payload, bytes, 12)) return r->status == mp4MergeOk;
    handler = Get32(bytes + 8);
    video = handler == FOURCC('v', 'i', 'd', 'e');
    if (!video && handler != FOURCC('s', 'o', 'u', 'n')) return true;
    t = video ? &r->video : &r->audio;
    if (t->present) return true;
    if (!FindPath(r, trak, trakEnd, kMdhd, 2, &payload, &end) ||
        !ReadAt(r, payload, bytes, 24)) return FAIL(r, mp4MergeBadInput);
    if (bytes[0] == 1) {
        if (!ReadAt(r, payload, bytes, 32)) return FAIL(r, mp4MergeBadInput);
        t->timescale = Get32(bytes + 20);
        t->duration = Get32(bytes + 24) == 0 ? Get32(bytes + 28) : 0;
    } else {
        t->timescale = Get32(bytes + 12);
        t->duration = Get32(bytes + 16);
    }
    if (!FindPath(r, trak, trakEnd, kStbl, 3, &stbl, &stblEnd))
        return FAIL(r, mp4MergeBadInput);
    if (!FindBox(r, stbl, stblEnd, FOURCC('s', 't', 's', 'd'), &payload, &end) ||
        !ParseStsd(r, t, payload, end, video)) return FAIL(r, mp4MergeBadInput);
    if (!FindBox(r, stbl, stblEnd, FOURCC('s', 't', 's', 'z'), &payload, &end) ||
        !ReadAt(r, payload, bytes, 12)) return FAIL(r, mp4MergeBadInput);
    t->constantSize = Get32(bytes + 4);
    t->sampleCount = Get32(bytes + 8);
    if (t->constantSize == 0 && !SetTable(r, &t->stsz, payload, end, 8, 4))
        return false;
    if (FindBox(r, stbl, stblEnd, FOURCC('s', 't', 'c', 'o'), &payload, &end)) {
        if (!SetTable(r, &t->stco, payload, end, 4, 4)) return false;
    } else if (FindBox(r, stbl, stblEnd, FOURCC('c', 'o', '6', '4'), &payload, &end)) {
        if (!SetTable(r, &t->stco, payload, end, 4, 8)) return false;
        t->co64 = true;
    } else {
        return FAIL(r, mp4MergeBadInput);
    }
    if (!FindBox(r, stbl, stblEnd, FOURCC('s', 't', 's', 'c'), &payload, &end) ||
        !SetTable(r, &t->stsc, payload, end, 4, 12) ||
        !FindBox(r, stbl, stblEnd, FOURCC('s', 't', 't', 's'), &payload, &end) ||
        !SetTable(r, &t->stts, payload, end, 4, 8))
        return FAIL(r, mp4MergeBadInput);
    if (FindBox(r, stbl, stblEnd, FOURCC('c', 't', 't', 's'), &payload, &end)) {
        if (!SetTable(r, &t->ctts, payload, end, 4, 8)) return false;
        t->hasCtts = true;
    }
    if (FindBox(r, stbl, stblEnd, FOURCC('s', 't', 's', 's'), &payload, &end)) {
        if (!SetTable(r, &t->stss, payload, end, 4, 4)) return false;
        t->hasStss = true;
    }
    if (r->status != mp4MergeOk) return false;
    /* A fragmented MP4's moov describes its track but holds no samples. */
    if (t->sampleCount != 0) {
        if (t->stsc.count == 0 || t->stts.count == 0)
            return FAIL(r, mp4MergeBadInput);
        if (!StartIteration(r, t)) return false;
    }
    t->present = true;
    return true;
}

UInt16 Mp4ReaderOpen(Mp4Reader *readerP, Mp4ReadProc readProcP, void *fileP,
                     UInt32 fileSize)
{
    UInt32 moov, moovEnd, start;
    MemSet(readerP, sizeof(*readerP), 0);
    readerP->readProcP = readProcP;
    readerP->fileP = fileP;
    readerP->fileSize = fileSize;
    if (!FindBox(readerP, 0, fileSize, FOURCC('m', 'o', 'o', 'v'), &moov,
            &moovEnd)) {
        FAIL(readerP, mp4MergeBadInput);
        return readerP->status;
    }
    start = moov;
    for (;;) {
        UInt32 trak, trakEnd;
        if (!FindBox(readerP, start, moovEnd, FOURCC('t', 'r', 'a', 'k'), &trak,
                &trakEnd)) break;
        if (!ParseTrack(readerP, trak, trakEnd)) break;
        start = trakEnd;
    }
    if (readerP->status == mp4MergeOk && !readerP->video.present &&
        !readerP->audio.present)
        FAIL(readerP, mp4MergeBadInput);
    return readerP->status;
}

Boolean Mp4ReaderNext(Mp4Reader *r, Mp4ReadTrack *t, Mp4Sample *s)
{
    const UInt8 *e;
    if (!t->present || r->status != mp4MergeOk || t->sample >= t->sampleCount)
        return false;
    if (t->chunkSamplesLeft == 0) {
        t->chunk++;
        if (t->chunk == t->nextStscChunk) {
            t->stscSamples = t->stscPending;
            t->nextStscChunk = NO_CHUNK;
            if (t->stsc.next < t->stsc.count) {
                if ((e = TableNext(r, &t->stsc)) == 0) return false;
                t->nextStscChunk = Get32(e);
                t->stscPending = Get32(e + 4);
            }
        }
        if ((e = TableNext(r, &t->stco)) == 0) return false;
        if (t->co64) {
            if (Get32(e) != 0) return FAIL(r, mp4MergeUnsupported);
            t->position = Get32(e + 4);
        } else {
            t->position = Get32(e);
        }
        t->chunkSamplesLeft = t->stscSamples;
        if (t->chunkSamplesLeft == 0) return FAIL(r, mp4MergeBadInput);
    }
    if (t->constantSize != 0) {
        s->size = t->constantSize;
    } else {
        if ((e = TableNext(r, &t->stsz)) == 0) return false;
        s->size = Get32(e);
    }
    s->offset = t->position;
    t->position += s->size;
    t->chunkSamplesLeft--;

    while (t->sttsLeft == 0) {
        if ((e = TableNext(r, &t->stts)) == 0) return false;
        t->sttsLeft = Get32(e);
        t->sttsDelta = Get32(e + 4);
    }
    s->dts = t->dts;
    t->dts += t->sttsDelta;
    t->sttsLeft--;

    s->cto = 0;
    if (t->hasCtts) {
        while (t->cttsLeft == 0) {
            if ((e = TableNext(r, &t->ctts)) == 0) return false;
            t->cttsLeft = Get32(e);
            t->cttsOffset = (Int32)Get32(e + 4);
        }
        s->cto = t->cttsOffset;
        t->cttsLeft--;
    }

    t->sample++;
    s->sync = true;
    if (t->hasStss) {
        s->sync = t->sample == t->nextSync;
        if (s->sync) {
            t->nextSync = 0;
            if (t->stss.next < t->stss.count) {
                if ((e = TableNext(r, &t->stss)) == 0) return false;
                t->nextSync = Get32(e);
            }
        }
    }
    if (s->offset + s->size > r->fileSize || s->offset + s->size < s->offset)
        return FAIL(r, mp4MergeBadInput);
    return true;
}

static Boolean Rewind(Mp4Reader *r, Mp4ReadTrack *t)
{
    t->stsz.next = 0;
    t->stco.next = 0;
    t->stsc.next = 0;
    t->stts.next = 0;
    t->ctts.next = 0;
    t->stss.next = 0;
    t->sample = 0;
    t->chunk = 0;
    t->chunkSamplesLeft = 0;
    t->position = 0;
    t->dts = 0;
    t->sttsLeft = 0;
    t->sttsDelta = 0;
    t->cttsLeft = 0;
    t->cttsOffset = 0;
    t->nextSync = 0;
    return StartIteration(r, t);
}

static Boolean MoveTo(Mp4Reader *r, Mp4ReadTrack *t, UInt32 sample)
{
    Mp4Sample s;
    if (!Rewind(r, t)) return false;
    while (t->sample < sample)
        if (!Mp4ReaderNext(r, t, &s)) return false;
    return true;
}

Boolean Mp4ReaderSeek(Mp4Reader *r, Mp4ReadTrack *t, UInt32 dts,
                      UInt32 minimum, UInt32 *foundP)
{
    UInt32 resume = t->sample, target = 0;
    Boolean found = false;
    Mp4Sample s;
    if (!t->present || r->status != mp4MergeOk || !Rewind(r, t)) return false;
    while (Mp4ReaderNext(r, t, &s)) {
        if (!s.sync || s.dts < minimum) continue;
        if (found && s.dts > dts) break;
        found = true;
        target = t->sample - 1;
        *foundP = s.dts;
    }
    if (r->status != mp4MergeOk) return false;
    return MoveTo(r, t, found ? target : resume) && found;
}
