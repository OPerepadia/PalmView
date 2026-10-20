/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_SOUND_H
#define PALMVIEW_SOUND_H

#include <stdint.h>

/* 16-bit PCM between the decoder, on the app's thread, and the Palm OS
 * sound stream callback, on the sound thread. Each side writes only its
 * own counters, so no locking is needed. Counters are in frames (one
 * sample per channel) and only grow. */

typedef struct SoundRing {
    uint32_t got;                 /* must stay first: see entry.S */
    int16_t *samplesP;
    uint32_t capacity;            /* frames, a power of two */
    uint32_t channels;            /* 1 or 2 */
    volatile uint32_t written;    /* by the decoder */
    volatile uint32_t ended;      /* by the decoder: no more input */
    volatile uint32_t paused;     /* by the decoder */
    volatile uint32_t read;       /* by the callback */
    volatile uint32_t played;     /* by the callback: the playback clock */
    volatile uint32_t lastRequest;
    volatile uint32_t underruns;
} SoundRing;

/* Capacity is rounded down to a power of two. Returns 0 or a negative
 * decoder error. */
int SoundOpen(uint32_t capacity, uint32_t channels);
void SoundClose(void);
SoundRing *SoundGetRing(void);
/* Copies interleaved frames in; returns how many fitted. */
uint32_t SoundFeed(const int16_t *pcmP, uint32_t frames);

/* The stream callback. Silence fills what the ring cannot; it counts as
 * played only once input has ended, so the clock waits for late data.
 * While paused it plays silence and counts nothing. */
int32_t SoundCallback(SoundRing *ringP, uint32_t channel, int16_t *bufferP,
                      uint32_t frames);

#endif
