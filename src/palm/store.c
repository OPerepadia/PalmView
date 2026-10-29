/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "store.h"

Err StoreOpenVolume(UInt16 *volumeP)
{
    UInt32 iterator = vfsIteratorStart;
    UInt32 romVersion = 0;
    Err error;
    if (FtrGet(sysFtrCreator, sysFtrNumROMVersion, &romVersion) != errNone ||
        romVersion < sysMakeROMVersion(4, 0, 0, sysROMStageRelease, 0))
        return vfsErrNoFileSystem;
    error = VFSVolumeEnumerate(volumeP, &iterator);
    if (error != errNone) return error;
    error = VFSDirCreate(*volumeP, STORE_DIR);
    return error == vfsErrFileAlreadyExists ? errNone : error;
}

Err StoreFreeBytes(UInt16 volume, UInt32 *freeP)
{
    UInt32 used = 0, total = 0;
    Err error = VFSVolumeSize(volume, &used, &total);
    *freeP = error == errNone && total > used ? total - used : 0;
    return error;
}

void StoreMakePath(Char *pathP, const Char *nameP)
{
    StrCopy(pathP, STORE_DIR "/");
    StrNCat(pathP, nameP, STORE_PATH_CAPACITY);
}

void StoreThumbPath(Char *pathP, const Char *videoPathP)
{
    UInt16 length;
    StrNCopy(pathP, videoPathP, STORE_PATH_CAPACITY - 1);
    pathP[STORE_PATH_CAPACITY - 1] = '\0';
    length = StrLen(pathP);
    if (length >= 4) StrCopy(pathP + length - 4, ".jpg");
}

static Boolean IsVideoName(const Char *nameP)
{
    UInt16 length = StrLen(nameP);
    if (length < 5 || StrCaselessCompare(nameP + length - 4, ".mp4") != 0)
        return false;
    return !(length > 10 &&
        (StrCaselessCompare(nameP + length - 10, ".video.mp4") == 0 ||
         StrCaselessCompare(nameP + length - 10, ".audio.mp4") == 0));
}

UInt16 StoreListVideos(UInt16 volume, Char *namesP, UInt16 nameCapacity,
                       UInt16 maxNames)
{
    UInt32 iterator = vfsIteratorStart;
    FileInfoType info;
    FileRef dir;
    UInt16 count = 0;
    if (VFSFileOpen(volume, STORE_DIR, vfsModeRead, &dir) != errNone) return 0;
    while (count < maxNames && iterator != vfsIteratorStop) {
        Char *nameP = namesP + (UInt32)count * nameCapacity;
        info.nameP = nameP;
        info.nameBufLen = nameCapacity;
        if (VFSDirEntryEnumerate(dir, &iterator, &info) != errNone) break;
        if ((info.attributes & vfsFileAttrDirectory) == 0 && IsVideoName(nameP))
            count++;
    }
    VFSFileClose(dir);
    return count;
}

Err StoreOpenAppend(UInt16 volume, const Char *pathP, FileRef *fileP,
                    UInt32 *sizeP)
{
    Err error = VFSFileOpen(volume, pathP, vfsModeReadWrite | vfsModeCreate,
        fileP);
    if (error != errNone) return error;
    error = VFSFileSize(*fileP, sizeP);
    /* Seeking to the end of a file reports vfsErrFileEOF, which is fine
     * here; a new file is already positioned there. */
    if (error == errNone && *sizeP != 0) {
        error = VFSFileSeek(*fileP, vfsOriginBeginning, (Int32)*sizeP);
        if (error == vfsErrFileEOF) error = errNone;
    }
    if (error != errNone) VFSFileClose(*fileP);
    return error;
}

Err StoreOpenRead(UInt16 volume, const Char *pathP, FileRef *fileP,
                  UInt32 *sizeP)
{
    Err error = VFSFileOpen(volume, pathP, vfsModeRead, fileP);
    if (error != errNone) return error;
    error = VFSFileSize(*fileP, sizeP);
    if (error != errNone) VFSFileClose(*fileP);
    return error;
}

Err StoreCreate(UInt16 volume, const Char *pathP, FileRef *fileP)
{
    VFSFileDelete(volume, pathP);
    return VFSFileOpen(volume, pathP, vfsModeReadWrite | vfsModeCreate |
        vfsModeTruncate, fileP);
}

Err StoreWrite(FileRef file, const void *dataP, UInt32 length)
{
    const UInt8 *p = (const UInt8 *)dataP;
    while (length != 0) {
        UInt32 written = 0;
        Err error = VFSFileWrite(file, length, p, &written);
        if (error != errNone) return error;
        if (written == 0) return vfsErrVolumeFull;
        p += written;
        length -= written;
    }
    return errNone;
}

Err StoreReadText(UInt16 volume, const Char *pathP, Char *bufferP,
                  UInt16 capacity, UInt16 *lengthP)
{
    FileRef file;
    UInt32 size = 0, read = 0;
    Err error = StoreOpenRead(volume, pathP, &file, &size);
    *lengthP = 0;
    bufferP[0] = '\0';
    if (error != errNone) return error;
    if (size > (UInt32)(capacity - 1)) size = capacity - 1;
    error = VFSFileRead(file, size, bufferP, &read);
    if (error == vfsErrFileEOF) error = errNone;
    VFSFileClose(file);
    bufferP[read] = '\0';
    *lengthP = (UInt16)read;
    return error;
}

Err StoreMp4Read(void *fileP, UInt32 offset, void *bufferP, UInt32 length)
{
    FileRef file = (FileRef)fileP;
    UInt8 *p = (UInt8 *)bufferP;
    Err error = VFSFileSeek(file, vfsOriginBeginning, (Int32)offset);
    while (error == errNone && length != 0) {
        UInt32 read = 0;
        error = VFSFileRead(file, length, p, &read);
        if (error == vfsErrFileEOF && read == length) error = errNone;
        if (read == 0 && error == errNone) error = vfsErrFileEOF;
        p += read;
        length -= read;
    }
    return error;
}

Err StoreMp4Write(void *fileP, const void *bufferP, UInt32 length)
{
    return StoreWrite((FileRef)fileP, bufferP, length);
}
