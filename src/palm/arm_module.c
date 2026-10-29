/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "arm_module.h"

#include <PceNativeCall.h>

#define HEADER_MAGIC 0x5054414DUL   /* 'PTAM' */
#define HEADER_VERSION 1
#define RW_REGION 0xFF

/* Not in SDK 5r4's headers; PalmOSGlue's MemGluePtrNew uses it on Palm OS 5
 * to allocate chunks of 64 KB and more. */
#define memNewChunkFlagAllowLarge 0x1000

/* Not declared in SDK 5r4 either. */
SysAppInfoPtr SysGetAppInfo(SysAppInfoPtr *uiAppPP,
                            SysAppInfoPtr *actionCodeAppPP)
    SYS_TRAP(sysTrapSysGetAppInfo);

static UInt32 Get32(const UInt8 *p)
{
    return ((UInt32)p[0] << 24) | ((UInt32)p[1] << 16) |
        ((UInt32)p[2] << 8) | (UInt32)p[3];
}

static UInt16 Get16(const UInt8 *p)
{
    return (UInt16)(((UInt16)p[0] << 8) | p[1]);
}

/* ARM data is little-endian. */
static void PutLittle32(UInt8 *p, UInt32 value)
{
    p[0] = (UInt8)value;
    p[1] = (UInt8)(value >> 8);
    p[2] = (UInt8)(value >> 16);
    p[3] = (UInt8)(value >> 24);
}

void *ArmLargeAlloc(UInt32 size)
{
    SysAppInfoPtr uiAppP = 0, actionAppP = 0;
    SysAppInfoPtr appP = SysGetAppInfo(&uiAppP, &actionAppP);
    UInt16 owner = appP != 0 ? appP->memOwnerID : 0;
    void *p = MemChunkNew(0, size, (UInt16)(owner | memNewChunkFlagNonMovable |
        memNewChunkFlagAllowLarge));
    if (p == 0 && size < 65000UL) p = MemPtrNew(size);
    return p;
}

static MemHandle LockResource(DmResType type, DmResID id, UInt8 **dataPP)
{
    MemHandle h = DmGetResource(type, id);
    *dataPP = h != 0 ? (UInt8 *)MemHandleLock(h) : 0;
    return h;
}

static void ReleaseResource(MemHandle h)
{
    if (h == 0) return;
    MemHandleUnlock(h);
    DmReleaseResource(h);
}

void ArmModuleUnload(ArmModule *m)
{
    UInt16 i;
    for (i = 0; i < m->segmentCount; i++) ReleaseResource(m->segmentH[i]);
    ReleaseResource(m->headerH);
    if (m->rwBlockP != 0) MemPtrFree(m->rwBlockP);
    if (m->stackP != 0) MemPtrFree(m->stackP);
    if (m->callP != 0) MemPtrFree(m->callP);
    MemSet(m, sizeof(*m), 0);
}

Err ArmModuleLoad(ArmModule *m, DmResID firstId, UInt32 stackSize)
{
    UInt8 *headerP, *dataP;
    UInt16 dataChunks, fixupChunks, i;
    UInt32 entryOffset, imageSize, gotOffset, copied = 0, fixups = 0;
    Err error = errNone;

    MemSet(m, sizeof(*m), 0);
    m->headerH = LockResource('armh', firstId, &headerP);
    if (headerP == 0) return dmErrResourceNotFound;
    if (Get32(headerP) != HEADER_MAGIC || Get32(headerP + 4) != HEADER_VERSION) {
        error = armErrBadHeader;
        goto fail;
    }
    m->segmentCount = Get16(headerP + 8);
    dataChunks = Get16(headerP + 10);
    fixupChunks = Get16(headerP + 12);
    entryOffset = Get32(headerP + 16);
    imageSize = Get32(headerP + 20);
    m->rwSize = Get32(headerP + 24);
    gotOffset = Get32(headerP + 28);
    m->fixupCount = Get32(headerP + 32);
    if (m->segmentCount == 0 || m->segmentCount > ARM_MODULE_MAX_SEGMENTS) {
        error = armErrBadHeader;
        goto fail;
    }

    /* Code runs where it lies; it must be word-aligned. */
    for (i = 0; i < m->segmentCount; i++) {
        UInt8 *codeP;
        UInt32 size = Get32(headerP + 44 + 8UL * i);
        m->segmentH[i] = LockResource('armc', (DmResID)(firstId + 1 + i), &codeP);
        if (codeP == 0) {
            error = dmErrResourceNotFound;
            goto fail;
        }
        if (((UInt32)codeP & 3) != 0) {
            error = armErrMisaligned;
            goto fail;
        }
        if (MemHandleSize(m->segmentH[i]) < size) {
            error = armErrBadSize;
            goto fail;
        }
        if (((UInt32)codeP & 7) != 0) m->misaligned8++;
        m->segmentBase[i] = (UInt32)codeP;
        m->codeBytes += size;
    }

    m->rwBlockP = (UInt8 *)ArmLargeAlloc(m->rwSize + 16);
    m->stackP = (UInt8 *)ArmLargeAlloc(stackSize);
    m->stackSize = stackSize;
    m->callP = (ArmCall *)MemPtrNew(sizeof(ArmCall));
    if (m->rwBlockP == 0 || m->stackP == 0 || m->callP == 0) {
        error = memErrNotEnoughSpace;
        goto fail;
    }
    m->rwP = (UInt8 *)(((UInt32)m->rwBlockP + 15) & ~15UL);

    for (i = 0; i < dataChunks; i++) {
        MemHandle h = LockResource('armd', (DmResID)(firstId + 1001 + i), &dataP);
        UInt32 size;
        if (dataP == 0) {
            error = dmErrResourceNotFound;
            goto fail;
        }
        size = MemHandleSize(h);
        if (copied + size > imageSize) {
            ReleaseResource(h);
            error = armErrBadSize;
            goto fail;
        }
        MemMove(m->rwP + copied, dataP, (Int32)size);
        copied += size;
        ReleaseResource(h);
    }
    if (copied != imageSize) {
        error = armErrBadSize;
        goto fail;
    }
    MemSet(m->rwP + imageSize, (Int32)(m->rwSize - imageSize), 0);

    for (i = 0; i < fixupChunks; i++) {
        MemHandle h = LockResource('armr', (DmResID)(firstId + 2001 + i), &dataP);
        UInt32 count, k;
        if (dataP == 0) {
            error = dmErrResourceNotFound;
            goto fail;
        }
        count = MemHandleSize(h) / 8;
        for (k = 0; k < count; k++) {
            const UInt8 *e = dataP + 8 * k;
            UInt32 rwOffset = Get32(e);
            UInt32 region = e[4];
            UInt32 offset = Get32(e + 4) & 0x00FFFFFFUL;
            UInt32 base;
            if (region == RW_REGION) base = (UInt32)m->rwP;
            else if (region < m->segmentCount) base = m->segmentBase[region];
            else break;
            if (rwOffset + 4 > m->rwSize) break;
            PutLittle32(m->rwP + rwOffset, base + offset);
            fixups++;
        }
        ReleaseResource(h);
    }
    if (fixups != m->fixupCount) {
        error = armErrBadFixups;
        goto fail;
    }

    m->entryP = (void *)(m->segmentBase[0] + entryOffset);
    MemSet(m->callP, sizeof(ArmCall), 0);
    m->callP->magic = ARM_CALL_MAGIC;
    m->callP->got = (UInt32)m->rwP + gotOffset;
    m->callP->stackTop = ((UInt32)m->stackP + m->stackSize) & ~7UL;
    return errNone;

fail:
    ArmModuleUnload(m);
    return error;
}

Int32 ArmModuleCall(ArmModule *m, UInt32 command, const UInt32 *argsP,
                    UInt16 argCount)
{
    UInt16 i;
    m->callP->command = command;
    m->callP->result = 0;
    for (i = 0; i < ARM_CALL_ARGS; i++) {
        m->callP->args[i] = i < argCount ? argsP[i] : 0;
        m->callP->out[i] = 0;
    }
    return (Int32)PceNativeCall((NativeFuncType *)m->entryP, m->callP);
}
