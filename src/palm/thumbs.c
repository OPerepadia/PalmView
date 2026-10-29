/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Thumbnails come in one after another over one kept-open connection,
 * those in view first, while nothing else uses the network. */

#include <PalmOS.h>

#include "resource.h"
#include "thumb.h"
#include "thumbs.h"

/* Decodes a thumbnail into a bitmap drawn at double density; the list's
 * has the video's duration on it. *bitsPP gets the bitmap that holds its
 * pixels. Returns 0 if it cannot. */
static BitmapType *DecodeThumb(AppState *s, const Thumb *thumbP, Boolean list,
                               const Char *durationP, BitmapType **bitsPP)
{
    Coord width = list ? THUMB_LIST_WIDTH : THUMB_WIDTH;
    Coord height = list ? THUMB_LIST_HEIGHT : THUMB_HEIGHT;
    BitmapType *bitsP = 0, *bitmapP = 0;
    Coord bitsWidth, bitsHeight;
    UInt16 rowBytes, error;
    if (s->thumbModuleLoaded &&
            (bitsP = BmpCreate(width, height, 16, 0, &error)) != 0) {
        UInt32 args[4];
        args[0] = (UInt32)thumbP->jpegP;
        args[1] = thumbP->length;
        args[2] = (UInt32)BmpGetBits(bitsP);
        args[3] = list;
        BmpGetDimensions(bitsP, &bitsWidth, &bitsHeight, &rowBytes);
        if (rowBytes == (UInt16)(width * 2) &&
                ArmModuleCall(&s->thumbModule, armThumbCmdDecode, args, 4) == 1) {
            if (list) ThumbBadge((UInt16 *)BmpGetBits(bitsP), durationP);
            bitmapP = (BitmapType *)BmpCreateBitmapV3(bitsP, kDensityDouble,
                BmpGetBits(bitsP), 0);
        }
        if (bitmapP == 0) {
            BmpDelete(bitsP);
            bitsP = 0;
        }
    }
    *bitsPP = bitsP;
    return bitmapP;
}

void FreeBitmap(BitmapType **bitmapPP, BitmapType **bitsPP)
{
    if (*bitmapPP != 0) BmpDelete(*bitmapPP);
    if (*bitsPP != 0) BmpDelete(*bitsPP);
    *bitmapPP = 0;
    *bitsPP = 0;
}

/* Stops downloading thumbnails; StartThumbs carries on later. */
void StopThumbs(AppState *s)
{
    if (s->task == taskThumbs) {
        if (s->jobP != 0) {
            NetJobCancel(s->jobP);
            while (s->jobP != 0) NetJobStep(&s->jobP);
        }
        s->task = taskIdle;
    }
    s->thumbItem = NO_RESULT;
    if (s->thumbBufferP != 0) MemPtrFree(s->thumbBufferP);
    s->thumbBufferP = 0;
}

/* Heap the thumbnails hold, for the log */
UInt32 ThumbBytes(const AppState *s)
{
    UInt32 total = s->detailThumbP != 0 ? THUMB_WIDTH * THUMB_HEIGHT * 2UL : 0;
    UInt16 i;
    for (i = 0; i < YT_SEARCH_MAX; i++) {
        total += s->thumbs[i].length;
        if (s->thumbs[i].bitmapP != 0)
            total += THUMB_LIST_WIDTH * THUMB_LIST_HEIGHT * 2UL;
    }
    return total;
}

void FreeThumbs(AppState *s)
{
    UInt16 i;
    StopThumbs(s);
    for (i = 0; i < YT_SEARCH_MAX; i++) {
        Thumb *thumbP = &s->thumbs[i];
        if (thumbP->jpegP != 0) MemPtrFree(thumbP->jpegP);
        FreeBitmap(&thumbP->bitmapP, &thumbP->bitsP);
        MemSet(thumbP, sizeof(*thumbP), 0);
    }
    FreeBitmap(&s->detailThumbP, &s->detailBitsP);
}

static Int16 FeedThumb(void *contextP, const UInt8 *dataP, UInt16 length)
{
    AppState *s = (AppState *)contextP;
    if (length > THUMB_JPEG_CAPACITY - s->thumbLength) return 1;
    MemMove(s->thumbBufferP + s->thumbLength, dataP, length);
    s->thumbLength += length;
    return 0;
}

/* A light gray box stands in until the thumbnail arrives, or if it
 * does not. */
static void DrawThumb(const AppState *s)
{
    static const RGBColorType kPlaceholder = { 0, 0xE0, 0xE0, 0xE0 };
    FormType *formP;
    RectangleType bounds;
    if (!OnForm(DetailForm)) return;
    formP = FrmGetActiveForm();
    FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, ThumbGadget), &bounds);
    if (s->detailThumbP != 0) {
        WinDrawBitmap(s->detailThumbP, bounds.topLeft.x, bounds.topLeft.y);
        return;
    }
    WinPushDrawState();
    WinSetForeColorRGB(&kPlaceholder, 0);
    WinDrawRectangle(&bounds, 0);
    WinPopDrawState();
}

/* The selected video's thumbnail at full size, from the list's JPEG */
void ShowDetailThumb(AppState *s)
{
    if (s->detailThumbP == 0 && s->thumbs[s->selected].state == thumbLoaded)
        s->detailThumbP = DecodeThumb(s, &s->thumbs[s->selected], false,
            0, &s->detailBitsP);
    DrawThumb(s);
}

/* The thumbnail the screen needs next: the selected video's on the details
 * screen, otherwise the results in view first. */
static UInt16 NextThumb(const AppState *s)
{
    UInt16 count = s->searchP->count, i;
    if (OnForm(DetailForm))
        return s->thumbs[s->selected].state == thumbMissing
            ? s->selected : NO_RESULT;
    if (!OnForm(MainForm)) return NO_RESULT;
    for (i = s->resultTop; i < s->resultTop + s->resultRows && i < count; i++)
        if (s->thumbs[i].state == thumbMissing) return i;
    for (i = 0; i < count; i++)
        if (s->thumbs[i].state == thumbMissing) return i;
    return NO_RESULT;
}

/* Asks for a thumbnail on the open connection, or opens one. */
static Boolean RequestThumb(AppState *s, UInt16 item)
{
    NetJobConfig config = s->netConfig;
    UInt16 length = YtBuildThumbRequest(&s->client,
        s->searchP->items[item].videoId, true, s->request, sizeof(s->request));
    if (length == 0) return false;
    s->thumbItem = item;
    s->thumbLength = 0;
    if (s->jobP != 0) return NetJobSend(s->jobP, s->request, length);
    /* Polled, so the list scrolls freely meanwhile. */
    config.keepAlive = true;
    config.poll = true;
    StrCopy(s->host, YT_THUMB_HOST);
    return NetJobStart(&s->jobP, &config, s->host, s->request, length,
        FeedThumb, s, &s->jobResult);
}

/* Only on an open network: the connection dialog is not worth
 * a thumbnail. */
void StartThumbs(AppState *s)
{
    UInt16 item = NextThumb(s);
    if (s->task != taskIdle || !NetworkOpen(s)) return;
    if (item == NO_RESULT) {
        StopThumbs(s);
        return;
    }
    if (s->thumbBufferP == 0)
        s->thumbBufferP = (UInt8 *)MemPtrNew(THUMB_JPEG_CAPACITY);
    if (s->thumbBufferP != 0 && RequestThumb(s, item)) {
        s->task = taskThumbs;
        return;
    }
    s->thumbs[item].state = thumbFailed;
    StopThumbs(s);
}

/* Keeps the downloaded thumbnail and shows it where it belongs. */
static void StoreThumb(AppState *s, UInt16 item)
{
    Thumb *thumbP = &s->thumbs[item];
    thumbP->jpegP = (UInt8 *)MemPtrNew(s->thumbLength);
    if (thumbP->jpegP != 0) {
        MemMove(thumbP->jpegP, s->thumbBufferP, s->thumbLength);
        thumbP->length = s->thumbLength;
        thumbP->bitmapP = DecodeThumb(s, thumbP, true,
            s->searchP->items[item].length, &thumbP->bitsP);
    }
    if (thumbP->bitmapP == 0) {
        if (thumbP->jpegP != 0) MemPtrFree(thumbP->jpegP);
        thumbP->jpegP = 0;
        thumbP->state = thumbFailed;
        return;
    }
    thumbP->state = thumbLoaded;
    if (OnForm(MainForm) && item >= s->resultTop &&
            (UInt16)(item - s->resultTop) < s->resultRows)
        DrawResultRow(s, item - s->resultTop);
    if (OnForm(DetailForm) && item == s->selected) ShowDetailThumb(s);
}

/* A thumbnail arrived and the connection stays open: ask for the next. */
void ThumbArrived(AppState *s)
{
    UInt16 next;
    StoreThumb(s, s->thumbItem);
    s->thumbItem = NO_RESULT;
    next = NextThumb(s);
    if (next == NO_RESULT || !RequestThumb(s, next)) {
        s->thumbItem = NO_RESULT;
        NetJobCancel(s->jobP);
    }
}

/* The connection closed: after the last thumbnail, or a failure. */
void ThumbsEnded(AppState *s)
{
    UInt16 item = s->thumbItem, status = s->jobResult.status;
    s->task = taskIdle;
    s->thumbItem = NO_RESULT;
    if (item != NO_RESULT) {
        if (status == netJobOk) {
            StoreThumb(s, item);
        } else if (!s->jobResult.reused || s->jobResult.received != 0) {
            s->thumbs[item].state = thumbFailed;
            /* Without the network, the rest would fail one by one. */
            if (status != netJobHttpError && status != netJobSinkError) {
                StopThumbs(s);
                return;
            }
        }
        /* Otherwise the server closed the open connection: ask again. */
    }
    StartThumbs(s);
}
