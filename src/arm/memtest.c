/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "memtest.h"

#include <stdlib.h>

#include "decoder.h"

#define SMALL_BYTES 4096UL
#define LARGE_BYTES (1024UL * 1024)
#define LINE_BYTES 32UL
#define PLD_AHEAD 128

/* pld [ip]: the module is built for ARMv5T, which has no PLD mnemonic. */
static inline void Prefetch(const void *p)
{
    register const void *ip __asm__("ip") = p;
    __asm__ volatile(".inst 0xf5dcf000" : : "r"(ip));
}

static uint32_t Alu(uint32_t loops)
{
    uint32_t a = 1, b = 2;
    while (loops-- != 0) {
        a += b ^ loops;
        b = (b << 1) + a;
    }
    return a ^ b;
}

static uint32_t Read(const uint32_t *p, uint32_t bytes, uint32_t total, int pld)
{
    uint32_t sum = 0, done;
    for (done = 0; done < total; done += bytes) {
        const uint32_t *q = p, *end = p + bytes / 4;
        while (q < end) {
            if (pld) Prefetch((const uint8_t *)q + PLD_AHEAD);
            sum += q[0] + q[1] + q[2] + q[3] + q[4] + q[5] + q[6] + q[7];
            q += 8;
        }
    }
    return sum;
}

static uint32_t Write(volatile uint32_t *p, uint32_t bytes, uint32_t total)
{
    uint32_t done, v = 0;
    for (done = 0; done < total; done += bytes) {
        volatile uint32_t *q = p, *end = p + bytes / 4;
        while (q < end) {
            q[0] = v; q[1] = v; q[2] = v; q[3] = v;
            q[4] = v; q[5] = v; q[6] = v; q[7] = v;
            q += 8;
            v++;
        }
    }
    return v;
}

/* Links every line of the block into one loop, stride lines apart, so
 * each load needs the address the last one read. stride must share no
 * factor with the line count. */
static uint32_t Chase(uint8_t *p, uint32_t bytes, uint32_t stride, uint32_t steps)
{
    uint32_t lines = bytes / LINE_BYTES, i, at = 0;
    void **q;
    for (i = 0; i < lines; i++) {
        uint32_t next = at + stride;
        if (next >= lines) next -= lines;
        *(void **)(p + at * LINE_BYTES) = p + next * LINE_BYTES;
        at = next;
    }
    q = (void **)p;
    while (steps-- != 0) q = (void **)*q;
    return (uint32_t)(uintptr_t)q;
}

int32_t MemTest(uint32_t test)
{
    uint32_t *p, result = 0;
    if (test == memTestAlu) return (int32_t)Alu(MEM_TEST_ALU_LOOPS);
    p = (uint32_t *)malloc(LARGE_BYTES);
    if (p == 0) return decErrNoMemory;
    for (result = 0; result < LARGE_BYTES / 4; result++) p[result] = result;
    switch (test) {
        case memTestReadSmall:
            result = Read(p, SMALL_BYTES, MEM_TEST_READ_BYTES, 0);
            break;
        case memTestReadLarge:
            result = Read(p, LARGE_BYTES, MEM_TEST_READ_BYTES, 0);
            break;
        case memTestReadPld:
            result = Read(p, LARGE_BYTES, MEM_TEST_READ_BYTES, 1);
            break;
        case memTestWriteLarge:
            result = Write(p, LARGE_BYTES, MEM_TEST_WRITE_BYTES);
            break;
        case memTestChaseSmall:
            result = Chase((uint8_t *)p, SMALL_BYTES, 37, MEM_TEST_SMALL_STEPS);
            break;
        case memTestChaseLarge:
            result = Chase((uint8_t *)p, LARGE_BYTES, 1021, MEM_TEST_LARGE_STEPS);
            break;
        default:
            result = 0;
            break;
    }
    free(p);
    return (int32_t)(result & 0x7FFFFFFFUL);
}
