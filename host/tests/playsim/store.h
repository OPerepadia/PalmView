/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Host stand-in for src/palm/store.h. */

#ifndef PALMVIEW_STORE_H
#define PALMVIEW_STORE_H

#include <PalmOS.h>
#include "mp4_merge.h"

#define STORE_PATH_CAPACITY 96

typedef FILE *FileRef;

Err StoreOpenVolume(UInt16 *volumeP);
Err StoreOpenRead(UInt16 volume, const Char *pathP, FileRef *fileP,
                  UInt32 *sizeP);
Err StoreMp4Read(void *fileP, UInt32 offset, void *bufferP, UInt32 length);
void VFSFileClose(FileRef file);

#endif
