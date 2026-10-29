/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_ARM_MODULE_H
#define PALMVIEW_ARM_MODULE_H

#include <PalmOS.h>
#include "armcall.h"

/* Loads the native ARM module packaged by tools/armlink.py. Code segments
 * stay in their resources, locked and unmodified; the GOT and writable data
 * go into one heap block that gets fixed up here. */

#define ARM_MODULE_MAX_SEGMENTS 40

/* Load errors, besides Palm OS memory and resource errors. */
#define armErrBadHeader (appErrorClass | 1)
#define armErrMisaligned (appErrorClass | 2)    /* code not word-aligned */
#define armErrBadSize (appErrorClass | 3)
#define armErrBadFixups (appErrorClass | 4)
#define ARM_STACK_SIZE 65536UL

/* Each module's resources start at its header's ID: code from ID + 1,
 * data from ID + 1001, fixups from ID + 2001. */
#define ARM_DECODER_ID 1000
#define ARM_THUMB_ID 1100
#define ARM_THUMB_STACK_SIZE 16384UL

typedef struct ArmModule {
    MemHandle headerH;
    MemHandle segmentH[ARM_MODULE_MAX_SEGMENTS];
    UInt32 segmentBase[ARM_MODULE_MAX_SEGMENTS];
    UInt16 segmentCount;
    UInt16 misaligned8;     /* segments not 8-byte aligned, for diagnostics */
    UInt8 *rwBlockP;        /* as allocated */
    UInt8 *rwP;             /* 16-byte aligned start */
    UInt32 rwSize;
    UInt32 fixupCount;
    UInt8 *stackP;
    UInt32 stackSize;
    ArmCall *callP;
    void *entryP;
    UInt32 codeBytes;
} ArmModule;

/* Large non-movable chunk from the dynamic heap, past the 64 KB limit of
 * MemPtrNew. */
void *ArmLargeAlloc(UInt32 size);

Err ArmModuleLoad(ArmModule *moduleP, DmResID firstId, UInt32 stackSize);
void ArmModuleUnload(ArmModule *moduleP);
/* Runs one command; arguments are copied in, outputs are left in
 * moduleP->callP->out. Returns the command's signed result. */
Int32 ArmModuleCall(ArmModule *moduleP, UInt32 command, const UInt32 *argsP,
                    UInt16 argCount);

#endif
