/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_ARMCALL_H
#define PALMVIEW_ARMCALL_H

/* The command block the 68K app passes to the native ARM module through
 * PceNativeCall. Every field is a 32-bit word in 68K (big-endian) byte
 * order; the ARM side swaps them. unsigned long is 32 bits on both sides. */

typedef unsigned long ArmWord;

#define ARM_CALL_MAGIC 0x50565731UL      /* 'PVW1' */
#define ARM_CALL_ARGS 8

typedef struct ArmCall {
    ArmWord magic;
    ArmWord got;          /* runtime GOT address, set by the loader */
    ArmWord stackTop;     /* 8-byte aligned top of the ARM stack */
    ArmWord command;
    ArmWord result;       /* signed */
    ArmWord args[ARM_CALL_ARGS];
    ArmWord out[ARM_CALL_ARGS];
} ArmCall;

enum {
    /* args: arena, arena size, stack base, stack size
     * out: module version, heap free */
    armCmdInit = 1,
    /* args: config, length, queue depth */
    armCmdVideoOpen,
    /* args: data (0 to drain), length, pts, DEC_* flags
     * result: frames output; out: frames queued, oldest one's pts */
    armCmdVideoDecode,
    /* out: frames, checksum, width, height */
    armCmdVideoInfo,
    /* converts the oldest queued frame
     * args: destination, stride, width, height, DEC_RGB_* flags */
    armCmdVideoRgb,
    /* args: config, length */
    armCmdAudioOpen,
    /* args: data, length, PCM buffer, DEC_CHECKSUM or 0
     * result: samples per channel */
    armCmdAudioDecode,
    /* out: samples, checksum, channels, sample rate */
    armCmdAudioInfo,
    /* out: heap used, heap peak, heap free, stack used */
    armCmdStats,
    armCmdClose,
    /* args: words to sum; result: sum (checks calls and the stack) */
    armCmdPing,
    /* drops the oldest queued frame; out: as armCmdVideoDecode */
    armCmdVideoPop,
    /* PCM ring for the sound stream, after armCmdAudioOpen.
     * args: capacity in frames, the file's sample rate
     * out: callback (ARM native), its userdata, channels, sample rate */
    armCmdSoundOpen,
    /* decodes one AAC packet into the ring
     * args: data, length, DEC_CHECKSUM or 0; result: frames, or an error
     * out: frames buffered, frames played, underruns, last callback size,
     * the decoder's sample rate (known after the first packet) */
    armCmdSoundFeed,
    /* out: as armCmdSoundFeed */
    armCmdSoundState,
    /* no more input: silence now counts as played */
    armCmdSoundEnd,
    /* args: 1 to play silence and hold the clock, 0 to go on */
    armCmdSoundPause,
    /* more heap, after armCmdInit
     * args: region, region size; out: heap free */
    armCmdHeapAdd,
    /* args: memTest*; result: as MemTest */
    armCmdMemTest
};

/* Stop the sound stream before armCmdClose: it frees the ring. */

#define ARM_MODULE_VERSION 5

#endif
