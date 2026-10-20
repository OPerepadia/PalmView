/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_STORE_H
#define PALMVIEW_STORE_H

#include <PalmOS.h>
#include <Extensions/ExpansionMgr/VFSMgr.h>
#include "mp4_merge.h"

/* Everything the app writes lives in one folder on the memory card. */
#define STORE_DIR "/PalmView"
#define STORE_PATH_CAPACITY 96

/* Finds the first mounted card volume and creates STORE_DIR on it. */
Err StoreOpenVolume(UInt16 *volumeP);
Err StoreFreeBytes(UInt16 volume, UInt32 *freeP);
void StoreMakePath(Char *pathP, const Char *nameP);

/* Opens a file for appending; *sizeP receives its current size. */
Err StoreOpenAppend(UInt16 volume, const Char *pathP, FileRef *fileP,
                    UInt32 *sizeP);
Err StoreOpenRead(UInt16 volume, const Char *pathP, FileRef *fileP,
                  UInt32 *sizeP);
Err StoreCreate(UInt16 volume, const Char *pathP, FileRef *fileP);
Err StoreWrite(FileRef file, const void *dataP, UInt32 length);

/* Lists the playable MP4 files in STORE_DIR, skipping leftover download
 * streams. Names go into namesP, nameCapacity bytes apart. Returns the
 * number found, up to maxNames. */
UInt16 StoreListVideos(UInt16 volume, Char *namesP, UInt16 nameCapacity,
                       UInt16 maxNames);

/* Reads a small text file into bufferP (NUL-terminated). */
Err StoreReadText(UInt16 volume, const Char *pathP, Char *bufferP,
                  UInt16 capacity, UInt16 *lengthP);

/* Mp4Io callbacks; the file pointers are FileRef values. */
Err StoreMp4Read(void *fileP, UInt32 offset, void *bufferP, UInt32 length);
Err StoreMp4Write(void *fileP, const void *bufferP, UInt32 length);

#endif
