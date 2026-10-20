/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Host version of the Palm decoder benchmark: same MP4 reader, same decoder
 * wrapper, same checksums. Use it to get the values the Palm must match.
 *
 *   bench FILE.mp4 [SECONDS] [SKIP_FLAGS]
 */

#include <PalmOS.h>
#include <time.h>

#include "decoder.h"
#include "mp4_read.h"

static Err HostRead(void *fileP, UInt32 offset, void *bufferP, UInt32 length)
{
    if (fseek((FILE *)fileP, (long)offset, SEEK_SET) != 0) return 1;
    return fread(bufferP, 1, length, (FILE *)fileP) == length ? errNone : 1;
}

static double Now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static UInt8 *ReadSample(FILE *fileP, const Mp4Sample *s)
{
    static UInt8 *bufferP;
    static UInt32 capacity;
    if (s->size + DEC_PADDING > capacity) {
        capacity = s->size + DEC_PADDING;
        bufferP = realloc(bufferP, capacity);
    }
    if (HostRead(fileP, s->offset, bufferP, s->size) != errNone) return 0;
    memset(bufferP + s->size, 0, DEC_PADDING);
    return bufferP;
}

int main(int argc, char **argv)
{
    static Mp4Reader reader;
    static int16_t pcm[2048 * 2];
    FILE *fileP;
    Mp4Sample s;
    DecVideoInfo video;
    DecAudioInfo audio;
    double seconds = argc > 2 ? atof(argv[2]) : 1e9, start;
    uint32_t skip = argc > 3 ? (uint32_t)atoi(argv[3]) : 0;
    UInt32 limit, samples = 0, pts, lastPts = 0, reordered = 0, outputs = 0;
    long size;
    int result;

    if (argc < 2 || (fileP = fopen(argv[1], "rb")) == 0) {
        fprintf(stderr, "usage: bench FILE.mp4 [SECONDS] [SKIP_FLAGS]\n");
        return 2;
    }
    fseek(fileP, 0, SEEK_END);
    size = ftell(fileP);
    if (Mp4ReaderOpen(&reader, HostRead, fileP, (UInt32)size) != mp4MergeOk) {
        fprintf(stderr, "mp4 read failed: %u line %u\n", reader.status,
            (unsigned)reader.detail);
        return 1;
    }
    printf("video: %ux%u, %u samples, timescale %u, config %u bytes\n",
        reader.video.width, reader.video.height,
        (unsigned)reader.video.sampleCount, (unsigned)reader.video.timescale,
        reader.video.configLength);
    printf("audio: %u ch, %u Hz, %u samples, config %u bytes\n",
        reader.audio.channels, reader.audio.sampleRate,
        (unsigned)reader.audio.sampleCount, reader.audio.configLength);

    result = DecVideoOpen(reader.video.config, reader.video.configLength, 1);
    if (result != 0) { fprintf(stderr, "video open %d\n", result); return 1; }
    limit = (UInt32)(seconds * reader.video.timescale);
    start = Now();
    while (Mp4ReaderNext(&reader, &reader.video, &s) && s.dts < limit) {
        const UInt8 *dataP = ReadSample(fileP, &s);
        if (dataP == 0 || (result = DecVideoDecode(dataP, s.size,
                (uint32_t)(s.dts + s.cto), skip | DEC_CHECKSUM)) < 0) {
            fprintf(stderr, "video decode failed at sample %u\n", (unsigned)samples);
            return 1;
        }
        /* Output frames must come in presentation order. */
        if (result > 0) {
            DecVideoQueued(&pts);
            if (outputs++ != 0 && pts <= lastPts) reordered++;
            lastPts = pts;
        }
        samples++;
    }
    while (DecVideoDecode(0, 0, 0, skip | DEC_CHECKSUM) > 0) {
        DecVideoQueued(&pts);
        if (pts <= lastPts) reordered++;
        lastPts = pts;
    }
    DecVideoGetInfo(&video);
    printf("video: %u samples in, %u frames out, checksum %08x, %.1f fps, "
        "%u out of order\n", (unsigned)samples, (unsigned)video.frames,
        (unsigned)video.checksum, video.frames / (Now() - start),
        (unsigned)reordered);

    result = DecAudioOpen(reader.audio.config, reader.audio.configLength);
    if (result != 0) { fprintf(stderr, "audio open %d\n", result); return 1; }
    limit = (UInt32)(seconds * reader.audio.timescale);
    samples = 0;
    start = Now();
    while (Mp4ReaderNext(&reader, &reader.audio, &s) && s.dts < limit) {
        const UInt8 *dataP = ReadSample(fileP, &s);
        if (dataP == 0 || DecAudioDecode(dataP, s.size, pcm, DEC_CHECKSUM) < 0) {
            fprintf(stderr, "audio decode failed at sample %u\n", (unsigned)samples);
            return 1;
        }
        samples++;
    }
    DecAudioGetInfo(&audio);
    printf("audio: %u packets in, %u samples out, %u ch, %u Hz, checksum %08x, "
        "%.0fx real time\n", (unsigned)samples, (unsigned)audio.samples,
        audio.channels, audio.sampleRate, (unsigned)audio.checksum,
        audio.sampleRate ? (audio.samples / (double)audio.sampleRate) /
            (Now() - start) : 0.0);
    DecVideoClose();
    DecAudioClose();
    fclose(fileP);
    return 0;
}
