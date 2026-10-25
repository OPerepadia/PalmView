/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_MEMTEST_H
#define PALMVIEW_MEMTEST_H

#include <stdint.h>

/* CPU and memory speed tests for PalmView Bench. Each does a fixed amount
 * of work in memory from the ARM heap, which the caller times. Memory
 * tests also fill their block first; memTestSetup does only that, so its
 * time can be taken off. Comparing the results at two clock speeds shows
 * what scales with the CPU and what waits for memory. */

enum {
    memTestSetup = 0,    /* only what the memory tests do before timing */
    memTestAlu,          /* MEM_TEST_ALU_LOOPS turns of a register-only loop */
    memTestReadSmall,    /* MEM_TEST_READ_BYTES read from a 4 KB block */
    memTestReadLarge,    /* the same from a 1 MB block */
    memTestReadPld,      /* the same, asking for each line ahead with PLD */
    memTestWriteLarge,   /* MEM_TEST_WRITE_BYTES written to a 1 MB block */
    memTestChaseSmall,   /* MEM_TEST_SMALL_STEPS loads, each needing the last */
    memTestChaseLarge,   /* MEM_TEST_LARGE_STEPS such loads, each in a new line */
    memTestCount
};

#define MEM_TEST_ALU_LOOPS 30000000UL
#define MEM_TEST_READ_BYTES (64UL * 1024 * 1024)
#define MEM_TEST_WRITE_BYTES (32UL * 1024 * 1024)
#define MEM_TEST_SMALL_STEPS 20000000UL
#define MEM_TEST_LARGE_STEPS 1000000UL

/* Returns a value that depends on the work done, or decErrNoMemory. */
int32_t MemTest(uint32_t test);

#endif
