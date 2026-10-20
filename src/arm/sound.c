/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "sound.h"

#include <stdlib.h>
#include <string.h>
#include "decoder.h"

static SoundRing gRing;

int SoundOpen(uint32_t capacity, uint32_t channels)
{
    uint32_t size = 1;
    SoundClose();
    if (channels < 1 || channels > 2 || capacity < 2) return decErrState;
    while (size * 2 <= capacity) size *= 2;
    gRing.samplesP = (int16_t *)malloc(size * channels * sizeof(int16_t));
    if (gRing.samplesP == 0) return decErrNoMemory;
#ifdef __arm__
    __asm__ volatile ("mov %0, r9" : "=r" (gRing.got));
#endif
    gRing.capacity = size;
    gRing.channels = channels;
    return 0;
}

void SoundClose(void)
{
    free(gRing.samplesP);
    memset(&gRing, 0, sizeof(gRing));
}

SoundRing *SoundGetRing(void)
{
    return &gRing;
}

uint32_t SoundFeed(const int16_t *pcmP, uint32_t frames)
{
    SoundRing *r = &gRing;
    uint32_t written = r->written, space = r->capacity - (written - r->read);
    uint32_t n = frames < space ? frames : space, left = n;
    while (left != 0) {
        uint32_t index = written & (r->capacity - 1);
        uint32_t chunk = r->capacity - index < left ? r->capacity - index : left;
        memcpy(r->samplesP + index * r->channels, pcmP,
            chunk * r->channels * sizeof(int16_t));
        pcmP += chunk * r->channels;
        written += chunk;
        left -= chunk;
    }
    r->written = written;
    return n;
}

int32_t SoundCallback(SoundRing *r, uint32_t channel, int16_t *bufferP,
                      uint32_t frames)
{
    uint32_t read = r->read, available = r->written - read;
    uint32_t n = frames < available ? frames : available, left = n;
    (void)channel;
    r->lastRequest = frames;
    if (r->paused) {
        memset(bufferP, 0, frames * r->channels * sizeof(int16_t));
        return 0;
    }
    while (left != 0) {
        uint32_t index = read & (r->capacity - 1);
        uint32_t chunk = r->capacity - index < left ? r->capacity - index : left;
        memcpy(bufferP, r->samplesP + index * r->channels,
            chunk * r->channels * sizeof(int16_t));
        bufferP += chunk * r->channels;
        read += chunk;
        left -= chunk;
    }
    r->read = read;
    if (n < frames) {
        memset(bufferP, 0, (frames - n) * r->channels * sizeof(int16_t));
        if (r->ended) n = frames;
        else r->underruns++;
    }
    r->played += n;
    return 0;
}
