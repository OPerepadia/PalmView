/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "bookmark.h"

#define BOOKMARK_FIELDS 5

static Boolean PackField(Char *dstP, UInt16 capacity, UInt16 *lengthP,
                         const Char *srcP, const TextCodec *codecP)
{
    if (!TextAppendUtf8(dstP, capacity, lengthP, srcP, codecP) ||
            *lengthP + 1 >= capacity)
        return false;
    dstP[(*lengthP)++] = '\0';
    return true;
}

UInt16 BookmarkPack(const YtSearchItem *itemP, const TextCodec *codecP,
                    Char *dstP, UInt16 capacity)
{
    UInt16 length = 1;
    if (capacity < 2) return 0;
    dstP[0] = BOOKMARK_VERSION;
    if (!PackField(dstP, capacity, &length, itemP->videoId, 0) ||
            !PackField(dstP, capacity, &length, itemP->title, codecP) ||
            !PackField(dstP, capacity, &length, itemP->channel, codecP) ||
            !PackField(dstP, capacity, &length, itemP->channelId, 0) ||
            !PackField(dstP, capacity, &length, itemP->length, 0))
        return 0;
    return length;
}

/* Finds where each field starts, and where the text ends. False unless all
 * are there. */
static Boolean SplitFields(const Char *srcP, UInt32 length,
                           const Char *fieldsP[BOOKMARK_FIELDS],
                           UInt32 *endP)
{
    UInt32 offset = 1;
    UInt16 field = 0;
    if (length < 2 || srcP[0] != BOOKMARK_VERSION) return false;
    while (field < BOOKMARK_FIELDS) {
        fieldsP[field++] = srcP + offset;
        while (offset < length && srcP[offset] != '\0') offset++;
        if (offset == length) return false;
        offset++;
    }
    *endP = offset;
    return fieldsP[0][0] != '\0';
}

static void UnpackField(const Char *srcP, Char *dstP, UInt16 capacity,
                        const TextCodec *codecP)
{
    TextUtf8ToPalm(srcP, StrLen(srcP), dstP, capacity, codecP);
}

Boolean BookmarkUnpack(const Char *srcP, UInt32 length,
                       const TextCodec *codecP, YtSearchItem *itemP)
{
    const Char *fieldsP[BOOKMARK_FIELDS];
    UInt32 end;
    MemSet(itemP, sizeof(*itemP), 0);
    if (!SplitFields(srcP, length, fieldsP, &end)) return false;
    UnpackField(fieldsP[0], itemP->videoId, sizeof(itemP->videoId), codecP);
    UnpackField(fieldsP[1], itemP->title, sizeof(itemP->title), codecP);
    UnpackField(fieldsP[2], itemP->channel, sizeof(itemP->channel), codecP);
    UnpackField(fieldsP[3], itemP->channelId, sizeof(itemP->channelId),
        codecP);
    UnpackField(fieldsP[4], itemP->length, sizeof(itemP->length), codecP);
    return true;
}

const Char *BookmarkVideoId(const Char *srcP, UInt32 length)
{
    const Char *fieldsP[BOOKMARK_FIELDS];
    UInt32 end;
    return SplitFields(srcP, length, fieldsP, &end) ? fieldsP[0] : 0;
}

const UInt8 *BookmarkThumb(const Char *srcP, UInt32 length,
                           UInt16 *thumbLengthP)
{
    const Char *fieldsP[BOOKMARK_FIELDS];
    UInt32 end;
    *thumbLengthP = 0;
    if (!SplitFields(srcP, length, fieldsP, &end) || end == length ||
            length - end > 0xFFFFUL)
        return 0;
    *thumbLengthP = (UInt16)(length - end);
    return (const UInt8 *)srcP + end;
}
