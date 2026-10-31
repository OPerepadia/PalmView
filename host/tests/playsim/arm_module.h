/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Host stand-in for src/palm/arm_module.h: commands go straight to the
 * decoder, and the simulated Palm clock advances by what each one would
 * cost on the device (see playback_sim.c). */

#ifndef PALMVIEW_ARM_MODULE_H
#define PALMVIEW_ARM_MODULE_H

#include <PalmOS.h>
#include "armcall.h"

#define ARM_STACK_SIZE 65536UL
#define ARM_DECODER_ID 1000

typedef struct ArmModule {
    ArmCall *callP;
    UInt8 *stackP;
} ArmModule;

void *ArmLargeAlloc(UInt32 size);
Err ArmModuleLoad(ArmModule *moduleP, UInt16 firstId, UInt32 stackSize);
void ArmModuleUnload(ArmModule *moduleP);
Int32 ArmModuleCall(ArmModule *moduleP, UInt32 command, const UInt32 *argsP,
                    UInt16 argCount);

#endif
