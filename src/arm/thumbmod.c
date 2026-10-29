/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The thumbnail module's commands: decodes thumbnails natively, many times
 * faster than the 68K code under PACE. Called from entry.S with r9 and the
 * stack set. */

#include <stdint.h>

#include "armcall.h"
#include "thumb_decode.h"

static uint32_t gWork[(THUMB_WORK_SIZE + 3) / 4];

uint32_t ArmDispatch(ArmCall *callP);

static uint32_t Swap(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00U) | ((v << 8) & 0xFF0000U) | (v << 24);
}

static int32_t Decode(const ArmCall *c)
{
    const uint8_t *jpegP = (const uint8_t *)Swap(c->args[0]);
    uint16_t *pixelsP = (uint16_t *)Swap(c->args[2]);
    int list = Swap(c->args[3]) != 0;
    uint32_t count = list ? THUMB_LIST_WIDTH * THUMB_LIST_HEIGHT
        : THUMB_WIDTH * THUMB_HEIGHT, i;
    if (!ThumbDecode(jpegP, (uint16_t)Swap(c->args[1]), pixelsP, gWork, list))
        return 0;
    for (i = 0; i < count; i++)
        pixelsP[i] = (uint16_t)((pixelsP[i] >> 8) | (pixelsP[i] << 8));
    return 1;
}

uint32_t ArmDispatch(ArmCall *callP)
{
    int32_t result = -1;
    if (Swap(callP->magic) == ARM_CALL_MAGIC &&
            Swap(callP->command) == armThumbCmdDecode)
        result = Decode(callP);
    callP->result = Swap((uint32_t)result);
    return (uint32_t)result;
}
