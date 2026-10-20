/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Just enough of the Palm OS API for the portable sources in src/core and
 * PalmTLS's HTTP component to build and run on the host. */

#ifndef PALMVIEW_HOST_PALMOS_SHIM_H
#define PALMVIEW_HOST_PALMOS_SHIM_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t UInt8;
typedef uint16_t UInt16;
typedef int16_t Int16;
typedef uint32_t UInt32;
typedef int32_t Int32;
typedef int16_t Err;
typedef uint8_t Boolean;
typedef char Char;
typedef void *MemPtr;

#define true 1
#define false 0
#define errNone 0
#define sysErrParamErr 1
#define memErrNotEnoughSpace 2

#define MemSet(pointer, length, value) memset((pointer), (value), (length))
#define MemMove(target, source, length) memmove((target), (source), (length))
#define MemCmp(left, right, length) memcmp((left), (right), (length))
#define MemPtrNew(size) malloc(size)
#define MemPtrFree(pointer) free(pointer)
#define StrLen(value) ((UInt16)strlen(value))
#define StrCopy(target, source) strcpy((target), (source))
#define StrNCopy(target, source, length) strncpy((target), (source), (length))
#define StrCat(target, source) strcat((target), (source))
#define StrCompare(left, right) strcmp((left), (right))
#define StrNCompare(left, right, length) strncmp((left), (right), (length))
#define StrChr(value, character) strchr((value), (character))
#define StrPrintF sprintf

#endif
