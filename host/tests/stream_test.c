/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Feeds a YouTube DASH stream to Mp4Stream in random pieces through a
 * small ring, and checks every sample against the same track of the
 * merged MP4 (pview merge output).
 *
 *   stream_test PART.mp4 MERGED.mp4 RING_KB SEED
 */

#include <PalmOS.h>
#include "mp4_read.h"
#include "mp4_stream.h"

#define BUFFER_CAPACITY 65536

static Err ReadFile(void *fileP, UInt32 offset, void *bufferP, UInt32 length)
{
    if (fseek((FILE *)fileP, (long)offset, SEEK_SET) != 0) return 1;
    return fread(bufferP, 1, length, (FILE *)fileP) == length ? errNone : 1;
}

static UInt8 *Load(const char *pathP, UInt32 *sizeP)
{
    FILE *fileP = fopen(pathP, "rb");
    UInt8 *dataP;
    long size;
    if (fileP == 0) return 0;
    fseek(fileP, 0, SEEK_END);
    size = ftell(fileP);
    rewind(fileP);
    dataP = (UInt8 *)malloc((size_t)size);
    if (dataP != 0 && fread(dataP, 1, (size_t)size, fileP) != (size_t)size) {
        free(dataP);
        dataP = 0;
    }
    fclose(fileP);
    *sizeP = (UInt32)size;
    return dataP;
}

static int Fail(const char *messageP, UInt32 sample)
{
    printf("FAIL: %s at sample %u\n", messageP, (unsigned)sample);
    return 1;
}

int main(int argc, char **argv)
{
    static Mp4Reader reader;
    static UInt8 got[BUFFER_CAPACITY], want[BUFFER_CAPACITY];
    Mp4Stream stream;
    Mp4ReadTrack track, *wantTrackP;
    Mp4Sample s, w;
    UInt8 *partP, *ringP;
    UInt32 partSize, mergedSize, sent = 0, samples = 0, ringSize;
    FILE *mergedP;
    if (argc != 5) {
        fprintf(stderr, "usage: stream_test PART.mp4 MERGED.mp4 RING_KB SEED\n");
        return 2;
    }
    ringSize = (UInt32)atoi(argv[3]) * 1024;
    srand((unsigned)atoi(argv[4]));
    partP = Load(argv[1], &partSize);
    mergedP = fopen(argv[2], "rb");
    if (partP == 0 || mergedP == 0) return Fail("cannot read the inputs", 0);
    fseek(mergedP, 0, SEEK_END);
    mergedSize = (UInt32)ftell(mergedP);
    if (Mp4ReaderOpen(&reader, ReadFile, mergedP, mergedSize) != mp4MergeOk)
        return Fail("cannot open the merged file", 0);
    ringP = (UInt8 *)malloc(ringSize);
    if (Mp4StreamOpen(&stream, ringP, ringSize, partSize, &track) != mp4MergeOk)
        return Fail("out of memory", 0);
    wantTrackP = 0;
    for (;;) {
        UInt16 result;
        /* Writes of 1 byte to 16 KB, as network reads would come. */
        UInt32 n = (UInt32)(rand() % 4 == 0 ? rand() % 16 + 1 : rand() % 16384 + 1);
        if (n > partSize - sent) n = partSize - sent;
        if (n > Mp4StreamSpace(&stream)) n = Mp4StreamSpace(&stream);
        Mp4StreamWrite(&stream, partP + sent, n);
        sent += n;
        if (wantTrackP == 0) {
            if (!Mp4StreamReady(&stream)) {
                if (stream.status != mp4MergeOk) return Fail("moov", 0);
                if (sent == partSize) return Fail("no moov", 0);
                continue;
            }
            wantTrackP = track.width != 0 ? &reader.video : &reader.audio;
            if (track.timescale != wantTrackP->timescale ||
                track.duration != wantTrackP->duration ||
                track.configLength != wantTrackP->configLength ||
                memcmp(track.config, wantTrackP->config, track.configLength) != 0 ||
                track.width != wantTrackP->width || track.height != wantTrackP->height ||
                track.channels != wantTrackP->channels ||
                track.sampleRate != wantTrackP->sampleRate)
                return Fail("track description differs", 0);
        }
        while ((result = Mp4StreamNext(&stream, &s, got, sizeof(got))) ==
                mp4StreamSample) {
            if (!Mp4ReaderNext(&reader, wantTrackP, &w))
                return Fail("more samples than the merged file", samples);
            if (s.size != w.size || s.dts != w.dts || s.cto != w.cto ||
                s.sync != w.sync)
                return Fail("sample differs", samples);
            if (ReadFile(mergedP, w.offset, want, w.size) != errNone ||
                memcmp(got, want, s.size) != 0)
                return Fail("sample data differs", samples);
            samples++;
        }
        if (result == mp4StreamEnd) break;
        if (result == mp4StreamError) {
            printf("FAIL: status %u, line %u\n", stream.status,
                (unsigned)stream.detail);
            return 1;
        }
        if (n == 0) return Fail("waiting for bytes that cannot come", samples);
    }
    if (samples != wantTrackP->sampleCount)
        return Fail("fewer samples than the merged file", samples);
    printf("stream ok: %s, %u samples, %u KB ring\n", argv[1], (unsigned)samples,
        (unsigned)(ringSize / 1024));
    Mp4StreamClose(&stream);
    free(ringP);
    free(partP);
    fclose(mergedP);
    return 0;
}
