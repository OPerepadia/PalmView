/* SPDX-License-Identifier: GPL-3.0-or-later */

/* PalmView Bench: loads the native ARM decoder module and measures it on
 * MP4 files saved by PalmView.
 *
 * Info: CPU, heap, module load, an ARM round trip (milestone 1), and
 *       CPU and memory speed (see memtest.h).
 * Run:  decodes 20 seconds of the chosen file's video and audio and
 *       reports speed, memory and checksums (milestone 2). The checksums
 *       must match build/host/bench on the same file. */

#include <PalmOS.h>

#include "arm_module.h"
#include "decoder.h"
#include "memtest.h"
#include "mp4_read.h"
#include "resource.h"
#include "store.h"
#include "text.h"

#define BENCH_SECONDS 20
#define MAX_FILES 16
#define FILE_NAME_CAPACITY 64
#define LOG_CAPACITY 2048
#define SAMPLE_CAPACITY 65536UL
#define PCM_BYTES (2048UL * 2 * 2)
#define HEAP_RESERVE (192UL * 1024)
#define ARENA_LIMIT (3UL * 1024 * 1024)
#define PREVIEW_W 160
#define PREVIEW_H 90
#define RGB_BENCH_W 320
#define RGB_BENCH_H 180
#define RGB_BENCH_FRAMES 25

typedef struct Bench {
    ArmModule module;
    Boolean loaded;
    UInt8 *arenaP;
    UInt32 arenaSize;
    Mp4Reader *readerP;
    UInt8 *sampleP;
    UInt8 *pcmP;
    UInt16 fileCount;
    UInt16 selected;
    UInt16 mode;
    Char files[MAX_FILES][FILE_NAME_CAPACITY];
    Char *fileChoices[MAX_FILES];
    Char log[LOG_CAPACITY];
} Bench;

static void *GetObject(UInt16 id)
{
    FormType *formP = FrmGetActiveForm();
    return FrmGetObjectPtr(formP, FrmGetObjectIndex(formP, id));
}

/* Log */

static void UpdateScrollBar(void)
{
    FieldType *fieldP = (FieldType *)GetObject(LogField);
    UInt16 position, textHeight, fieldHeight, maximum;
    FldGetScrollValues(fieldP, &position, &textHeight, &fieldHeight);
    maximum = textHeight > fieldHeight ? textHeight - fieldHeight : 0;
    SclSetScrollBar((ScrollBarType *)GetObject(LogScrollBar), (Int16)position,
        0, (Int16)maximum, (Int16)(fieldHeight > 1 ? fieldHeight - 1 : 1));
}

static void ShowLog(Bench *b)
{
    FieldType *fieldP = (FieldType *)GetObject(LogField);
    FldSetTextPtr(fieldP, b->log);
    FldRecalculateField(fieldP, true);
    FldSetScrollPosition(fieldP, StrLen(b->log));
    FldDrawField(fieldP);
    UpdateScrollBar();
}

/* Appends text; drops the oldest lines when the log is full. */
static void Log(Bench *b, const Char *textP)
{
    UInt16 length = StrLen(b->log), add = StrLen(textP);
    if (length + add + 1 >= LOG_CAPACITY) {
        UInt16 cut = (UInt16)(length + add + 1 - LOG_CAPACITY + 256);
        while (cut < length && b->log[cut] != '\n') cut++;
        if (cut >= length) length = 0;
        else {
            MemMove(b->log, b->log + cut + 1, length - cut);
            length = StrLen(b->log);
        }
        b->log[length] = '\0';
    }
    TextAppend(b->log, LOG_CAPACITY, &length, textP);
}

static void LogNumber(Bench *b, UInt32 value)
{
    Char text[12];
    UInt16 length = 0;
    text[0] = '\0';
    TextAppendUInt32(text, sizeof(text), &length, value);
    Log(b, text);
}

static void LogSigned(Bench *b, Int32 value)
{
    if (value < 0) {
        Log(b, "-");
        LogNumber(b, (UInt32)-value);
    } else {
        LogNumber(b, (UInt32)value);
    }
}

static void LogHex(Bench *b, UInt32 value)
{
    static const Char kDigits[] = "0123456789abcdef";
    Char text[9];
    Int16 i;
    for (i = 7; i >= 0; i--) {
        text[i] = kDigits[value & 15];
        value >>= 4;
    }
    text[8] = '\0';
    Log(b, text);
}

static void LogKB(Bench *b, UInt32 bytes)
{
    LogNumber(b, (bytes + 512) / 1024);
    Log(b, " KB");
}

/* "12.34" from ticks */
static void LogSeconds(Bench *b, UInt32 ticks)
{
    UInt32 hundredths = ticks * 100UL / SysTicksPerSecond();
    LogNumber(b, hundredths / 100);
    Log(b, ".");
    if (hundredths % 100 < 10) Log(b, "0");
    LogNumber(b, hundredths % 100);
    Log(b, " s");
}

/* "12.3" from tenths */
static void LogTenths(Bench *b, UInt32 tenths)
{
    LogNumber(b, tenths / 10);
    Log(b, ".");
    LogNumber(b, tenths % 10);
}

/* value / ticks per second, with one decimal: frames per second etc. */
static void LogRate(Bench *b, UInt32 count, UInt32 ticks)
{
    LogTenths(b, ticks == 0 ? 0 : count * 10UL * SysTicksPerSecond() / ticks);
}

static void LogHeap(Bench *b)
{
    UInt32 freeBytes = 0, maxChunk = 0;
    MemHeapFreeBytes(0, &freeBytes, &maxChunk);
    Log(b, "Heap free ");
    LogKB(b, freeBytes);
    Log(b, ", largest ");
    LogKB(b, maxChunk);
    Log(b, "\n");
}

/* Module */

static Int32 Call(Bench *b, UInt32 command, UInt32 a0, UInt32 a1, UInt32 a2,
                  UInt32 a3)
{
    UInt32 args[4];
    args[0] = a0; args[1] = a1; args[2] = a2; args[3] = a3;
    return ArmModuleCall(&b->module, command, args, 4);
}

/* RGB565 in the 68K byte order of BmpCreate bitmaps. */
static Int32 CallRgb(Bench *b, void *dstP, UInt32 stride, UInt32 width,
                     UInt32 height)
{
    UInt32 args[5];
    args[0] = (UInt32)dstP; args[1] = stride; args[2] = width; args[3] = height;
    args[4] = DEC_RGB_BIG_ENDIAN;
    return ArmModuleCall(&b->module, armCmdVideoRgb, args, 5);
}

static UInt32 Out(const Bench *b, UInt16 i)
{
    return b->module.callP->out[i];
}

static Boolean EnsureModule(Bench *b)
{
    UInt32 start, freeBytes = 0, maxChunk = 0;
    Err error;
    Int32 result;
    if (b->loaded) return true;
    start = TimGetTicks();
    error = ArmModuleLoad(&b->module);
    if (error != errNone) {
        Log(b, "Module load failed, error ");
        LogHex(b, error);
        Log(b, "\n");
        return false;
    }
    Log(b, "Module: ");
    LogNumber(b, b->module.segmentCount);
    Log(b, " segments, ");
    LogKB(b, b->module.codeBytes);
    Log(b, " code, ");
    LogKB(b, b->module.rwSize);
    Log(b, " data, ");
    LogNumber(b, b->module.fixupCount);
    Log(b, " fixups, loaded in ");
    LogSeconds(b, TimGetTicks() - start);
    Log(b, "\nSegments not 8-byte aligned: ");
    LogNumber(b, b->module.misaligned8);
    Log(b, "\n");

    {
        UInt32 args[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        result = ArmModuleCall(&b->module, armCmdPing, args, 8);
    }
    Log(b, "ARM ping: ");
    LogSigned(b, result);
    Log(b, result == 36 ? " (ok)\n" : " (expected 36)\n");
    if (result != 36) return false;

    MemHeapFreeBytes(0, &freeBytes, &maxChunk);
    b->arenaSize = maxChunk > HEAP_RESERVE ? maxChunk - HEAP_RESERVE : 0;
    if (b->arenaSize > ARENA_LIMIT) b->arenaSize = ARENA_LIMIT;
    b->arenaP = b->arenaSize != 0 ? (UInt8 *)ArmLargeAlloc(b->arenaSize) : 0;
    if (b->arenaP == 0) {
        Log(b, "No memory for the ARM heap\n");
        return false;
    }
    result = Call(b, armCmdInit, (UInt32)b->arenaP, b->arenaSize,
        (UInt32)b->module.stackP, ARM_STACK_SIZE);
    Log(b, "ARM heap ");
    LogKB(b, b->arenaSize);
    Log(b, ", init ");
    LogSigned(b, result);
    Log(b, ", module v");
    LogNumber(b, Out(b, 0));
    Log(b, "\n");
    b->loaded = result == 0;
    return b->loaded;
}

/* Nanoseconds per step, from ticks for steps (a multiple of 10000). */
static void LogStepNs(Bench *b, UInt32 ticks, UInt32 steps)
{
    UInt32 us = ticks * 1000000UL / SysTicksPerSecond();
    LogTenths(b, us / (steps / 10000UL));
    Log(b, " ns\n");
}

static void ShowMemory(Bench *b)
{
    UInt32 ticks[memTestCount];
    UInt16 i;
    Log(b, "Testing memory...\n");
    ShowLog(b);
    for (i = 0; i < memTestCount; i++) {
        UInt32 start = TimGetTicks();
        if (Call(b, armCmdMemTest, i, 0, 0, 0) < 0) {
            Log(b, "No memory for the memory test\n");
            return;
        }
        ticks[i] = TimGetTicks() - start;
    }
    for (i = memTestReadSmall; i < memTestCount; i++)
        ticks[i] = ticks[i] > ticks[memTestSetup] ? ticks[i] - ticks[memTestSetup] : 1;
    Log(b, "ALU: ");
    LogRate(b, MEM_TEST_ALU_LOOPS / 1000000UL, ticks[memTestAlu]);
    Log(b, " M loops/s\nRead 4 KB: ");
    LogRate(b, MEM_TEST_READ_BYTES >> 20, ticks[memTestReadSmall]);
    Log(b, " MB/s\nRead 1 MB: ");
    LogRate(b, MEM_TEST_READ_BYTES >> 20, ticks[memTestReadLarge]);
    Log(b, " MB/s\nRead 1 MB, PLD: ");
    LogRate(b, MEM_TEST_READ_BYTES >> 20, ticks[memTestReadPld]);
    Log(b, " MB/s\nWrite 1 MB: ");
    LogRate(b, MEM_TEST_WRITE_BYTES >> 20, ticks[memTestWriteLarge]);
    Log(b, " MB/s\nLoad in 4 KB: ");
    LogStepNs(b, ticks[memTestChaseSmall], MEM_TEST_SMALL_STEPS);
    Log(b, "Load in 1 MB: ");
    LogStepNs(b, ticks[memTestChaseLarge], MEM_TEST_LARGE_STEPS);
}

static void ShowInfo(Bench *b)
{
    UInt32 rom = 0, cpu = 0;
    FtrGet(sysFtrCreator, sysFtrNumROMVersion, &rom);
    FtrGet(sysFileCSystem, sysFtrNumProcessorID, &cpu);
    Log(b, "== Info\nROM ");
    LogHex(b, rom);
    Log(b, ", CPU ");
    LogHex(b, cpu);
    Log(b, "\n");
    LogHeap(b);
    if (EnsureModule(b)) {
        Call(b, armCmdStats, 0, 0, 0, 0);
        Log(b, "ARM stack used ");
        LogKB(b, Out(b, 3));
        Log(b, "\n");
        LogHeap(b);
        ShowMemory(b);
    }
    ShowLog(b);
}

/* Files */

static void ScanFiles(Bench *b)
{
    UInt16 volume, i;
    ListType *listP = (ListType *)GetObject(FileList);
    ControlType *triggerP = (ControlType *)GetObject(FileTrigger);
    b->fileCount = StoreOpenVolume(&volume) == errNone
        ? StoreListVideos(volume, b->files[0], FILE_NAME_CAPACITY, MAX_FILES) : 0;
    for (i = 0; i < b->fileCount; i++) b->fileChoices[i] = b->files[i];
    LstSetListChoices(listP, b->fileChoices, (Int16)b->fileCount);
    LstSetHeight(listP, (Int16)(b->fileCount < 9 ? (b->fileCount ? b->fileCount : 1) : 9));
    if (b->selected >= b->fileCount) b->selected = 0;
    CtlSetLabel(triggerP, b->fileCount != 0 ? b->files[b->selected] : "(no files)");
}

/* Benchmark */

static void DrawPreview(Bench *b)
{
    FormType *formP = FrmGetActiveForm();
    RectangleType bounds;
    BitmapType *bmpP;
    BitmapTypeV3 *v3P;
    Err error;
    FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, PreviewGadget), &bounds);
    bmpP = BmpCreate(PREVIEW_W, PREVIEW_H, 16, 0, &error);
    if (bmpP == 0) return;
    if (CallRgb(b, BmpGetBits(bmpP), PREVIEW_W * 2, PREVIEW_W, PREVIEW_H) == 0) {
        v3P = BmpCreateBitmapV3(bmpP, kDensityDouble, BmpGetBits(bmpP), 0);
        if (v3P != 0) {
            WinDrawBitmap((BitmapType *)v3P, bounds.topLeft.x, bounds.topLeft.y);
            BmpDelete((BitmapType *)v3P);
        }
    }
    BmpDelete(bmpP);
}

static void BenchColor(Bench *b)
{
    UInt8 *scratchP = (UInt8 *)ArmLargeAlloc((UInt32)RGB_BENCH_W * RGB_BENCH_H * 2);
    UInt32 start;
    UInt16 i;
    if (scratchP == 0) return;
    start = TimGetTicks();
    for (i = 0; i < RGB_BENCH_FRAMES; i++)
        CallRgb(b, scratchP, RGB_BENCH_W * 2, RGB_BENCH_W, RGB_BENCH_H);
    Log(b, "Color 320x180: ");
    LogRate(b, RGB_BENCH_FRAMES, TimGetTicks() - start);
    Log(b, " frames/s\n");
    MemPtrFree(scratchP);
}

static Boolean ReadSample(FileRef file, const Mp4Sample *s, UInt8 *bufferP)
{
    if (s->size + DEC_PADDING > SAMPLE_CAPACITY) return false;
    if (StoreMp4Read((void *)file, s->offset, bufferP, s->size) != errNone)
        return false;
    MemSet(bufferP + s->size, DEC_PADDING, 0);
    return true;
}

static void RunVideo(Bench *b, FileRef file)
{
    static const UInt32 kSkip[3] = { 0, DEC_SKIP_DEBLOCK, DEC_SKIP_NONREF };
    Mp4ReadTrack *t = &b->readerP->video;
    UInt32 limit = BENCH_SECONDS * t->timescale, samples = 0;
    UInt32 start, readTicks = 0, total;
    Int32 result;
    Mp4Sample s;
    MemMove(b->sampleP, t->config, t->configLength);
    result = Call(b, armCmdVideoOpen, (UInt32)b->sampleP, t->configLength, 1, 0);
    if (result != 0) {
        Log(b, "Video open failed: ");
        LogSigned(b, result);
        Log(b, "\n");
        return;
    }
    start = TimGetTicks();
    while (Mp4ReaderNext(b->readerP, t, &s) && s.dts < limit) {
        UInt32 readStart = TimGetTicks();
        if (!ReadSample(file, &s, b->sampleP)) {
            Log(b, "Read failed\n");
            return;
        }
        readTicks += TimGetTicks() - readStart;
        result = Call(b, armCmdVideoDecode, (UInt32)b->sampleP, s.size,
            s.dts + (UInt32)s.cto, kSkip[b->mode] | DEC_CHECKSUM);
        if (result < 0) {
            Log(b, "Decode failed at ");
            LogNumber(b, samples);
            Log(b, ": ");
            LogSigned(b, result);
            Log(b, "\n");
            return;
        }
        samples++;
        EvtResetAutoOffTimer();
    }
    while (Call(b, armCmdVideoDecode, 0, 0, 0, kSkip[b->mode] | DEC_CHECKSUM) > 0) {
    }
    total = TimGetTicks() - start;
    Call(b, armCmdVideoInfo, 0, 0, 0, 0);
    Log(b, "Video ");
    LogNumber(b, Out(b, 0));
    Log(b, " frames ");
    LogNumber(b, Out(b, 2));
    Log(b, "x");
    LogNumber(b, Out(b, 3));
    Log(b, " from ");
    LogNumber(b, samples);
    Log(b, "\n decode ");
    LogSeconds(b, total - readTicks);
    Log(b, " = ");
    LogRate(b, Out(b, 0), total - readTicks);
    Log(b, " fps\n reads ");
    LogSeconds(b, readTicks);
    Log(b, ", with reads ");
    LogRate(b, Out(b, 0), total);
    Log(b, " fps\n checksum ");
    LogHex(b, Out(b, 1));
    Log(b, "\n");
    ShowLog(b);
    DrawPreview(b);
    BenchColor(b);
}

static void RunAudio(Bench *b, FileRef file)
{
    Mp4ReadTrack *t = &b->readerP->audio;
    UInt32 limit = BENCH_SECONDS * t->timescale, packets = 0;
    UInt32 start, readTicks = 0, total, sampleRate;
    Int32 result;
    Mp4Sample s;
    MemMove(b->sampleP, t->config, t->configLength);
    result = Call(b, armCmdAudioOpen, (UInt32)b->sampleP, t->configLength, 0, 0);
    if (result != 0) {
        Log(b, "Audio open failed: ");
        LogSigned(b, result);
        Log(b, "\n");
        return;
    }
    start = TimGetTicks();
    while (Mp4ReaderNext(b->readerP, t, &s) && s.dts < limit) {
        UInt32 readStart = TimGetTicks();
        if (!ReadSample(file, &s, b->sampleP)) {
            Log(b, "Read failed\n");
            return;
        }
        readTicks += TimGetTicks() - readStart;
        result = Call(b, armCmdAudioDecode, (UInt32)b->sampleP, s.size,
            (UInt32)b->pcmP, DEC_CHECKSUM);
        if (result < 0) {
            Log(b, "Audio decode failed: ");
            LogSigned(b, result);
            Log(b, "\n");
            return;
        }
        packets++;
        EvtResetAutoOffTimer();
    }
    total = TimGetTicks() - start;
    Call(b, armCmdAudioInfo, 0, 0, 0, 0);
    sampleRate = Out(b, 3);
    Log(b, "Audio ");
    LogNumber(b, packets);
    Log(b, " packets, ");
    LogNumber(b, Out(b, 2));
    Log(b, " ch ");
    LogNumber(b, sampleRate);
    Log(b, " Hz\n decode ");
    LogSeconds(b, total - readTicks);
    Log(b, " = ");
    /* Real-time factor: seconds of sound per second of decoding. */
    if (sampleRate != 0 && total > readTicks) {
        UInt32 tenths = Out(b, 0) * SysTicksPerSecond() / (total - readTicks) * 10UL /
            sampleRate;
        LogNumber(b, tenths / 10);
        Log(b, ".");
        LogNumber(b, tenths % 10);
    }
    Log(b, "x real time\n checksum ");
    LogHex(b, Out(b, 1));
    Log(b, "\n");
}

static void RunBench(Bench *b)
{
    static const Char kModes[3][16] = { "full", "no deblocking", "skip B" };
    UInt16 volume;
    FileRef file;
    UInt32 size;
    Char path[STORE_PATH_CAPACITY];
    if (b->fileCount == 0) {
        Log(b, "No MP4 files in /PalmView\n");
        ShowLog(b);
        return;
    }
    Log(b, "== ");
    Log(b, b->files[b->selected]);
    Log(b, " (");
    Log(b, kModes[b->mode]);
    Log(b, ", first 20 s)\n");
    ShowLog(b);
    if (!EnsureModule(b)) {
        ShowLog(b);
        return;
    }
    StoreMakePath(path, b->files[b->selected]);
    if (StoreOpenVolume(&volume) != errNone ||
        StoreOpenRead(volume, path, &file, &size) != errNone) {
        Log(b, "Cannot open the file\n");
        ShowLog(b);
        return;
    }
    if (Mp4ReaderOpen(b->readerP, StoreMp4Read, (void *)file, size) != mp4MergeOk) {
        Log(b, "Not a playable MP4: status ");
        LogNumber(b, b->readerP->status);
        Log(b, ", line ");
        LogNumber(b, b->readerP->detail);
        Log(b, "\n");
    } else {
        if (b->readerP->video.present) RunVideo(b, file);
        if (b->readerP->audio.present) RunAudio(b, file);
        Call(b, armCmdStats, 0, 0, 0, 0);
        Log(b, "ARM heap peak ");
        LogKB(b, Out(b, 1));
        Log(b, ", stack ");
        LogKB(b, Out(b, 3));
        Log(b, "\n");
        Call(b, armCmdClose, 0, 0, 0, 0);
    }
    VFSFileClose(file);
    ShowLog(b);
}

/* Events */

static Boolean HandleEvent(EventType *eventP, Bench *b)
{
    FormType *formP;
    switch (eventP->eType) {
        case frmLoadEvent:
            formP = FrmInitForm(eventP->data.frmLoad.formID);
            FrmSetActiveForm(formP);
            return true;
        case frmOpenEvent:
            FldSetTextPtr((FieldType *)GetObject(LogField), b->log);
            ScanFiles(b);
            FrmDrawForm(FrmGetActiveForm());
            ShowLog(b);
            return true;
        case popSelectEvent:
            if (eventP->data.popSelect.listID == FileList) {
                b->selected = (UInt16)eventP->data.popSelect.selection;
            } else if (eventP->data.popSelect.listID == ModeList) {
                b->mode = (UInt16)eventP->data.popSelect.selection;
            }
            return false;
        case ctlSelectEvent:
            if (eventP->data.ctlSelect.controlID == InfoButton) {
                ShowInfo(b);
                return true;
            }
            if (eventP->data.ctlSelect.controlID == RunButton) {
                RunBench(b);
                return true;
            }
            break;
        case sclRepeatEvent:
        case sclExitEvent: {
            FieldType *fieldP = (FieldType *)GetObject(LogField);
            Int16 from = eventP->eType == sclRepeatEvent
                ? eventP->data.sclRepeat.value : eventP->data.sclExit.value;
            Int16 to = eventP->eType == sclRepeatEvent
                ? eventP->data.sclRepeat.newValue : eventP->data.sclExit.newValue;
            if (to > from) FldScrollField(fieldP, (UInt16)(to - from), winDown);
            else if (to < from) FldScrollField(fieldP, (UInt16)(from - to), winUp);
            UpdateScrollBar();
            return true;
        }
        default:
            break;
    }
    return false;
}

UInt32 PilotMain(UInt16 cmd, void *cmdPBP, UInt16 launchFlags)
{
    Bench *b;
    EventType event;
    UInt32 rom = 0, cpu = 0;
    Err error;
    (void)cmdPBP;
    (void)launchFlags;
    if (cmd != sysAppLaunchCmdNormalLaunch) return errNone;
    FtrGet(sysFtrCreator, sysFtrNumROMVersion, &rom);
    if (rom < sysMakeROMVersion(5, 0, 0, sysROMStageRelease, 0) ||
        FtrGet(sysFileCSystem, sysFtrNumProcessorID, &cpu) != errNone ||
        !sysFtrNumProcessorIsARM(cpu)) {
        FrmAlert(RomIncompatibleAlert);
        return sysErrRomIncompatible;
    }
    b = (Bench *)MemPtrNew(sizeof(Bench));
    if (b == 0) return memErrNotEnoughSpace;
    MemSet(b, sizeof(*b), 0);
    b->readerP = (Mp4Reader *)MemPtrNew(sizeof(Mp4Reader));
    b->sampleP = (UInt8 *)ArmLargeAlloc(SAMPLE_CAPACITY);
    b->pcmP = (UInt8 *)MemPtrNew(PCM_BYTES);
    if (b->readerP == 0 || b->sampleP == 0 || b->pcmP == 0) {
        if (b->readerP != 0) MemPtrFree(b->readerP);
        if (b->sampleP != 0) MemPtrFree(b->sampleP);
        if (b->pcmP != 0) MemPtrFree(b->pcmP);
        MemPtrFree(b);
        return memErrNotEnoughSpace;
    }
    StrCopy(b->log, "Tap Info to load the ARM module,\nor pick a file and tap Run.\n");
    FrmGotoForm(MainForm);
    do {
        EvtGetEvent(&event, evtWaitForever);
        if (!SysHandleEvent(&event) && !MenuHandleEvent(0, &event, &error) &&
            !HandleEvent(&event, b))
            FrmDispatchEvent(&event);
    } while (event.eType != appStopEvent);
    FrmCloseAllForms();
    if (b->loaded) Call(b, armCmdClose, 0, 0, 0, 0);
    ArmModuleUnload(&b->module);
    if (b->arenaP != 0) MemPtrFree(b->arenaP);
    MemPtrFree(b->readerP);
    MemPtrFree(b->sampleP);
    MemPtrFree(b->pcmP);
    MemPtrFree(b);
    return errNone;
}
