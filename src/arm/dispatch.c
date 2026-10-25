/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Runs one ArmCall command. Called from entry.S with r9 and the stack set. */

#include <stdint.h>

#include "armcall.h"
#include "decoder.h"
#include "libc_arm.h"
#include "memtest.h"
#include "sound.h"
#include "libavutil/log.h"

#define STACK_PATTERN 0x5A5AA5A5UL
#define ARM_ABORTED (-100)

static uint32_t *gStackBase;
static uint32_t *gStackTop;
static int16_t gPcm[2048 * 2];

uint32_t ArmDispatch(ArmCall *callP);
void SoundCallbackEntry(void);

static uint32_t Swap(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00U) | ((v << 8) & 0xFF0000U) | (v << 24);
}

static uint32_t Arg(const ArmCall *c, int i)
{
    return Swap(c->args[i]);
}

static void SetOut(ArmCall *c, int i, uint32_t value)
{
    c->out[i] = Swap(value);
}

static void SetQueueOut(ArmCall *c)
{
    uint32_t pts = 0;
    SetOut(c, 0, DecVideoQueued(&pts));
    SetOut(c, 1, pts);
}

static void SetSoundOut(ArmCall *c)
{
    const SoundRing *r = SoundGetRing();
    DecAudioInfo info;
    DecAudioGetInfo(&info);
    SetOut(c, 0, r->written - r->read);
    SetOut(c, 1, r->played);
    SetOut(c, 2, r->underruns);
    SetOut(c, 3, r->lastRequest);
    SetOut(c, 4, info.sampleRate);
}

/* Matches decoded PCM to the ring's channel count, in place. */
static void MatchChannels(int16_t *pcmP, uint32_t frames, uint32_t from,
                          uint32_t to)
{
    uint32_t i;
    if (from == 1 && to == 2) {
        for (i = frames; i-- != 0;) pcmP[2 * i] = pcmP[2 * i + 1] = pcmP[i];
    } else if (from == 2 && to == 1) {
        for (i = 0; i < frames; i++)
            pcmP[i] = (int16_t)(((int32_t)pcmP[2 * i] + pcmP[2 * i + 1]) >> 1);
    }
}

static void NoLog(void *avcl, int level, const char *format, va_list args)
{
    (void)avcl; (void)level; (void)format; (void)args;
}

/* Fills the unused part of the stack so its high-water mark can be read. */
static void MarkStack(void)
{
    uint32_t here = 0, *p;
    __asm__ volatile ("mov %0, sp" : "=r" (here));
    for (p = gStackBase; p < (uint32_t *)(here - 256U) && p < gStackTop; p++)
        *p = STACK_PATTERN;
}

static uint32_t StackUsed(void)
{
    const uint32_t *p = gStackBase;
    while (p < gStackTop && *p == STACK_PATTERN) p++;
    return (uint32_t)((uintptr_t)gStackTop - (uintptr_t)p);
}

static int32_t Run(ArmCall *c, uint32_t command)
{
    switch (command) {
        case armCmdPing: {
            uint32_t sum = 0;
            int i;
            for (i = 0; i < ARM_CALL_ARGS; i++) sum += Arg(c, i);
            return (int32_t)sum;
        }
        case armCmdInit:
            ArmHeapInit((void *)Arg(c, 0), Arg(c, 1));
            gStackBase = (uint32_t *)Arg(c, 2);
            gStackTop = (uint32_t *)(Arg(c, 2) + Arg(c, 3));
            MarkStack();
            av_log_set_callback(NoLog);
            SetOut(c, 0, ARM_MODULE_VERSION);
            {
                uint32_t used, peak, free;
                ArmHeapStats(&used, &peak, &free);
                SetOut(c, 1, free);
            }
            return 0;
        case armCmdHeapAdd: {
            uint32_t used, peak, free;
            ArmHeapAdd((void *)Arg(c, 0), Arg(c, 1));
            ArmHeapStats(&used, &peak, &free);
            SetOut(c, 0, free);
            return 0;
        }
        case armCmdMemTest:
            return MemTest(Arg(c, 0));
        case armCmdVideoOpen:
            return DecVideoOpen((const uint8_t *)Arg(c, 0), Arg(c, 1), Arg(c, 2));
        case armCmdVideoDecode: {
            int32_t result = DecVideoDecode((const uint8_t *)Arg(c, 0), Arg(c, 1),
                Arg(c, 2), Arg(c, 3));
            SetQueueOut(c);
            return result;
        }
        case armCmdVideoPop:
            DecVideoPop();
            SetQueueOut(c);
            return 0;
        case armCmdVideoInfo: {
            DecVideoInfo info;
            DecVideoGetInfo(&info);
            SetOut(c, 0, info.frames);
            SetOut(c, 1, info.checksum);
            SetOut(c, 2, info.width);
            SetOut(c, 3, info.height);
            return 0;
        }
        case armCmdVideoRgb:
            return DecVideoToRgb565((uint8_t *)Arg(c, 0), Arg(c, 1), Arg(c, 2),
                Arg(c, 3), Arg(c, 4));
        case armCmdAudioOpen:
            return DecAudioOpen((const uint8_t *)Arg(c, 0), Arg(c, 1));
        case armCmdAudioDecode:
            return DecAudioDecode((const uint8_t *)Arg(c, 0), Arg(c, 1),
                (int16_t *)Arg(c, 2), Arg(c, 3));
        case armCmdAudioInfo: {
            DecAudioInfo info;
            DecAudioGetInfo(&info);
            SetOut(c, 0, info.samples);
            SetOut(c, 1, info.checksum);
            SetOut(c, 2, info.channels);
            SetOut(c, 3, info.sampleRate);
            return 0;
        }
        case armCmdStats: {
            uint32_t used, peak, free;
            ArmHeapStats(&used, &peak, &free);
            SetOut(c, 0, used);
            SetOut(c, 1, peak);
            SetOut(c, 2, free);
            SetOut(c, 3, gStackBase != 0 ? StackUsed() : 0);
            return 0;
        }
        case armCmdClose:
            DecVideoClose();
            DecAudioClose();
            SoundClose();
            return 0;
        case armCmdSoundOpen: {
            DecAudioInfo info;
            int32_t result;
            DecAudioGetInfo(&info);
            if (info.channels == 0) return decErrState;
            result = SoundOpen(Arg(c, 0), info.channels > 2 ? 2 : info.channels);
            SetOut(c, 0, (uint32_t)(uintptr_t)SoundCallbackEntry);
            SetOut(c, 1, (uint32_t)(uintptr_t)SoundGetRing());
            SetOut(c, 2, SoundGetRing()->channels);
            /* AAC reports its rate only after the first frame. */
            SetOut(c, 3, info.sampleRate != 0 ? info.sampleRate : Arg(c, 1));
            return result;
        }
        case armCmdSoundFeed: {
            int32_t frames = DecAudioDecode((const uint8_t *)Arg(c, 0), Arg(c, 1),
                gPcm, Arg(c, 2));
            if (frames > 0) {
                DecAudioInfo info;
                DecAudioGetInfo(&info);
                MatchChannels(gPcm, (uint32_t)frames, info.channels > 2 ? 2 : info.channels,
                    SoundGetRing()->channels);
                SoundFeed(gPcm, (uint32_t)frames);
            }
            SetSoundOut(c);
            return frames;
        }
        case armCmdSoundState:
            SetSoundOut(c);
            return 0;
        case armCmdSoundEnd:
            SoundGetRing()->ended = 1;
            return 0;
        case armCmdSoundPause:
            SoundGetRing()->paused = Arg(c, 0) != 0;
            return 0;
        default:
            return decErrState;
    }
}

uint32_t ArmDispatch(ArmCall *callP)
{
    int32_t result;
    if (Swap(callP->magic) != ARM_CALL_MAGIC) return (uint32_t)decErrState;
    if (__builtin_setjmp(gArmAbortJump)) {
        result = ARM_ABORTED;
    } else {
        result = Run(callP, Swap(callP->command));
    }
    callP->result = Swap((uint32_t)result);
    return (uint32_t)result;
}
