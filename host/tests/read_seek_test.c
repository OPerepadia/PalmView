/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Checks Mp4ReaderSeek on a merged MP4 against a full read of each track.
 *
 *   read_seek_test MERGED.mp4 SEED
 */

#include <PalmOS.h>
#include "mp4_read.h"

#define MAX_SAMPLES 65536
#define SEEKS 200
#define SAMPLES_AFTER_SEEK 50

static Mp4Sample gAll[MAX_SAMPLES];

static Err ReadFile(void *fileP, UInt32 offset, void *bufferP, UInt32 length)
{
    if (fseek((FILE *)fileP, (long)offset, SEEK_SET) != 0) return 1;
    return fread(bufferP, 1, length, (FILE *)fileP) == length ? errNone : 1;
}

static int Fail(const char *messageP, UInt32 dts, UInt32 minimum)
{
    printf("FAIL: %s, seeking to %u from %u\n", messageP, (unsigned)dts,
        (unsigned)minimum);
    return 1;
}

static Boolean Same(const Mp4Sample *a, const Mp4Sample *b)
{
    return a->offset == b->offset && a->size == b->size && a->dts == b->dts &&
        a->cto == b->cto && a->sync == b->sync;
}

/* Where Mp4ReaderSeek should land, from the full read; -1 for nowhere */
static long Expected(UInt32 count, UInt32 dts, UInt32 minimum)
{
    long found = -1;
    UInt32 i;
    for (i = 0; i < count; i++) {
        if (!gAll[i].sync || gAll[i].dts < minimum) continue;
        if (found >= 0 && gAll[i].dts > dts) break;
        found = (long)i;
    }
    return found;
}

/* Checks the samples Mp4ReaderNext returns from first on. */
static int CheckFrom(Mp4Reader *r, Mp4ReadTrack *t, UInt32 count,
                     UInt32 first, UInt32 dts, UInt32 minimum)
{
    UInt32 i;
    Mp4Sample s;
    for (i = first; i < count && i < first + SAMPLES_AFTER_SEEK; i++)
        if (!Mp4ReaderNext(r, t, &s) || !Same(&s, &gAll[i]))
            return Fail("wrong sample after the seek", dts, minimum);
    return 0;
}

static int TestTrack(Mp4Reader *r, Mp4ReadTrack *t, const char *nameP)
{
    UInt32 count = 0, end, i, seeks = 0, misses = 0;
    Mp4Sample s;
    while (count < MAX_SAMPLES && Mp4ReaderNext(r, t, &s)) gAll[count++] = s;
    if (r->status != mp4MergeOk || count == 0 || count == MAX_SAMPLES) {
        printf("FAIL: could not read the %s track\n", nameP);
        return 1;
    }
    end = gAll[count - 1].dts + 1;
    for (i = 0; i < SEEKS; i++) {
        UInt32 dts = (UInt32)rand() % end, found = 0, at = 0;
        UInt32 minimum = i % 50 == 0 ? end
            : rand() % 3 == 0 ? (UInt32)rand() % end : 0;
        long want = Expected(count, dts, minimum);
        Boolean ok;
        /* Start partway through, so a miss has a place to keep. */
        if (!Mp4ReaderSeek(r, t, 0, 0, &found))
            return Fail("could not seek to the start", dts, minimum);
        if (Mp4ReaderSeek(r, t, gAll[rand() % count].dts, 0, &found))
            while (gAll[at].dts != found || !gAll[at].sync) at++;
        ok = Mp4ReaderSeek(r, t, dts, minimum, &found);
        if (r->status != mp4MergeOk) return Fail("read error", dts, minimum);
        if (want < 0) {
            if (ok) return Fail("found a sample where none fits", dts, minimum);
            if (CheckFrom(r, t, count, at, dts, minimum) != 0) return 1;
            misses++;
            continue;
        }
        if (!ok) return Fail("found nothing", dts, minimum);
        if (found != gAll[want].dts) return Fail("landed on the wrong sample", dts, minimum);
        if (CheckFrom(r, t, count, (UInt32)want, dts, minimum) != 0) return 1;
        seeks++;
    }
    printf("%s: %u samples, %u seeks, %u misses kept their place\n", nameP,
        (unsigned)count, (unsigned)seeks, (unsigned)misses);
    return 0;
}

int main(int argc, char **argv)
{
    static Mp4Reader reader;
    FILE *fileP;
    long size;
    int failed;
    if (argc != 3) {
        fprintf(stderr, "usage: read_seek_test MERGED.mp4 SEED\n");
        return 2;
    }
    srand((unsigned)atoi(argv[2]));
    fileP = fopen(argv[1], "rb");
    if (fileP == 0) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    fseek(fileP, 0, SEEK_END);
    size = ftell(fileP);
    if (Mp4ReaderOpen(&reader, ReadFile, fileP, (UInt32)size) != mp4MergeOk) {
        printf("FAIL: cannot open %s\n", argv[1]);
        return 1;
    }
    failed = TestTrack(&reader, &reader.video, "video") ||
        TestTrack(&reader, &reader.audio, "audio");
    fclose(fileP);
    return failed;
}
