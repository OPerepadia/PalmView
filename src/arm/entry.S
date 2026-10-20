/* SPDX-License-Identifier: GPL-3.0-or-later */

/* PceNativeCall entry: UInt32 ArmEntry(emulState, ArmCall *call, call68K).
 * Loads the GOT base into r9 (the code is built with -msingle-pic-base),
 * switches to the stack the app allocated (PceNativeCall's is only 4 KB),
 * and calls ArmDispatch(call) through the GOT. */

    .syntax unified
    .arm
    .section .text.ArmEntry, "ax", %progbits
    .global ArmEntry
    .type ArmEntry, %function

/* Big-endian word to native: ARMv5 has no REV. Clobbers tmp. */
.macro SWAP32 reg, tmp
    eor \tmp, \reg, \reg, ror #16
    bic \tmp, \tmp, #0x00FF0000
    mov \reg, \reg, ror #8
    eor \reg, \reg, \tmp, lsr #8
.endm

ArmEntry:
    push {r4-r11, lr}
    mov r4, r1
    ldr r9, [r4, #4]          /* call->got */
    SWAP32 r9, r3
    ldr r5, [r4, #8]          /* call->stackTop */
    SWAP32 r5, r3
    mov r6, sp
    mov sp, r5
    push {r6, r7}             /* old sp; two words keep 8-byte alignment */
    mov r0, r4
    ldr ip, .Ldispatch
    ldr ip, [r9, ip]
    blx ip
    pop {r6, r7}
    mov sp, r6
    pop {r4-r11, pc}

    .align 2
.Ldispatch:
    .word ArmDispatch(GOT)

    .size ArmEntry, . - ArmEntry

/* Palm OS sound stream callback, called on the sound thread with
 * (SoundRing *ring, channel, buffer, frames). The OS does not know our
 * GOT, so it comes from the ring's first word; r9 is restored after.
 * It runs on the sound thread's own stack, not the app's ARM stack. */

    .section .text.SoundCallbackEntry, "ax", %progbits
    .global SoundCallbackEntry
    .type SoundCallbackEntry, %function

SoundCallbackEntry:
    push {r4, r9, r10, lr}    /* four words keep 8-byte alignment */
    ldr r9, [r0]
    ldr ip, .Lcallback
    ldr ip, [r9, ip]
    blx ip
    pop {r4, r9, r10, pc}

    .align 2
.Lcallback:
    .word SoundCallback(GOT)

    .size SoundCallbackEntry, . - SoundCallbackEntry
