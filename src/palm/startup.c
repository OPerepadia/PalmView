/* SPDX-License-Identifier: GPL-3.0-or-later */

/* From PalmTLS apps/tls-tester/src/startup.c: relocates absolute code
 * addresses in both code segments, then runs PilotMain. Until relocation
 * is done, this file must not call code in other files: those calls go
 * through veneers with absolute addresses. */

#include <PalmOS.h>

#define CODE_SEGMENTS 2
#define RELOC_TYPE 'Rloc'

/* Written by tools/m68k-relocs.py, after a count for each pair of source
 * and target segments. */
typedef struct Relocation {
    UInt16 offset;
    UInt16 target;
} Relocation;

UInt32 PilotMain(UInt16 cmd, void *cmdPBP, UInt16 launchFlags);
UInt32 __Startup__(void);

/* Rewrites every address from the segments' current places, whatever the
 * code holds now: NVFS can reload a resource's original bytes from flash
 * at any time it is unlocked. Writes only addresses that changed. */
static Boolean Relocate(UInt8 *code1P, UInt8 *code2P)
{
    MemHandle relocH = DmGet1Resource(RELOC_TYPE, 1);
    const UInt16 *countP;
    const Relocation *relocationP;
    UInt8 *baseP[CODE_SEGMENTS];
    UInt16 source, target, index;
    Boolean ok, reserved = false;

    if (relocH == 0) return false;
    countP = (const UInt16 *)MemHandleLock(relocH);
    relocationP = (const Relocation *)(countP + CODE_SEGMENTS * CODE_SEGMENTS);
    baseP[0] = code1P;
    baseP[1] = code2P;
    ok = code2P != 0 || (countP[1] == 0 && countP[2] == 0 && countP[3] == 0);
    for (source = 0; ok && source < CODE_SEGMENTS; source++) {
        for (target = 0; ok && target < CODE_SEGMENTS; target++) {
            for (index = *countP++; ok && index > 0; index--, relocationP++) {
                UInt32 *addressP = (UInt32 *)(baseP[source] + relocationP->offset);
                UInt32 value = (UInt32)baseP[target] + relocationP->target;
                if (*addressP == value) continue;
                if (!reserved) {
                    reserved = MemSemaphoreReserve(true) == errNone;
                    ok = reserved;
                    if (!ok) break;
                }
                *addressP = value;
            }
        }
    }
    if (reserved) MemSemaphoreRelease(true);
    MemHandleUnlock(relocH);
    DmReleaseResource(relocH);
    return ok;
}

UInt32 __attribute__((section(".vectors"))) __Startup__(void)
{
    MemHandle code2H = DmGet1Resource('code', 2);
    UInt8 *code2P = code2H != 0 ? (UInt8 *)MemHandleLock(code2H) : 0;
    UInt8 *code1P = (UInt8 *)&__Startup__;
    SysAppInfoPtr appInfoP;
    void *prevGlobalsP;
    void *globalsP;
    UInt32 result = dmErrResourceNotFound;

    __asm__ volatile ("" : "+a" (code1P));
    if (Relocate(code1P, code2P)) {
        SysAppStartup(&appInfoP, &prevGlobalsP, &globalsP);
        result = PilotMain(appInfoP->cmd, appInfoP->cmdPBP, appInfoP->launchFlags);
        SysAppExit(appInfoP, prevGlobalsP, globalsP);
    }
    if (code2P != 0) MemHandleUnlock(code2H);
    if (code2H != 0) DmReleaseResource(code2H);
    return result;
}
