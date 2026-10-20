/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The host shim plus the window, bitmap and time calls src/palm/playback.c
 * uses, for host/tests/playback_sim.c. */

#ifndef PALMVIEW_PLAYSIM_PALMOS_H
#define PALMVIEW_PLAYSIM_PALMOS_H

#include "../../shim/PalmOS.h"

typedef Int16 Coord;
typedef struct PointType { Coord x, y; } PointType;
typedef struct RectangleType { PointType topLeft, extent; } RectangleType;
typedef struct BitmapType BitmapType;
typedef void *WinHandle;

enum { winScreenWidth, winScreenHeight, winScreenRowBytes, winScreenDepth,
       winScreenAllDepths, winScreenDensity, winScreenPixelFormat };
enum { pixelFormatIndexed, pixelFormat565, pixelFormat565LE };
#define kDensityLow 72
#define evtWaitForever (-1)

UInt32 TimGetTicks(void);
Boolean EvtEventAvail(void);
Boolean EvtSysEventAvail(Boolean ignorePenUps);
UInt32 SysTicksPerSecond(void);
Err MemHeapFreeBytes(UInt16 heapID, UInt32 *freeP, UInt32 *maxP);
WinHandle WinGetDisplayWindow(void);
BitmapType *WinGetBitmap(WinHandle windowH);
Err WinScreenGetAttribute(int selector, UInt32 *attrP);
void *BmpGetBits(BitmapType *bitmapP);
void BmpGetDimensions(const BitmapType *bitmapP, Coord *widthP, Coord *heightP,
                      UInt16 *rowBytesP);

typedef UInt32 SndStreamRef;
typedef Err (*SndStreamBufferCallback)(void *userdata, SndStreamRef channel,
                                       void *buffer, UInt32 frames);
enum { sndInput, sndOutput };
enum { sndMono, sndStereo };
#define sndInt16Little 0x12

Err SndStreamCreate(SndStreamRef *channelP, int mode, UInt32 sampleRate,
                    int type, int width, SndStreamBufferCallback callbackP,
                    void *userdata, UInt32 bufferFrames, Boolean armNative);
Err SndStreamStart(SndStreamRef channel);
Err SndStreamStop(SndStreamRef channel);
Err SndStreamDelete(SndStreamRef channel);
Err SndStreamSetVolume(SndStreamRef channel, Int32 volume);

/* ArmLargeAlloc memory lives below 4 GB so it fits ArmCall's 32-bit words. */
#undef MemPtrFree
void SimFree(void *p);
#define MemPtrFree(pointer) SimFree(pointer)

#endif
