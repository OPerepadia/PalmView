/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_LIBC_ARM_H
#define PALMVIEW_LIBC_ARM_H

#include <stdint.h>

void ArmHeapInit(void *arenaP, uint32_t size);
void ArmHeapStats(uint32_t *usedP, uint32_t *peakP, uint32_t *freeP);

/* abort() longjmps here (__builtin_setjmp buffer) and sets the flag. */
extern void *gArmAbortJump[5];
extern int gArmAborted;

#endif
