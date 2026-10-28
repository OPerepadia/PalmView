/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Downloads the selected video's streams to the card in 4 MB chunks, each
 * file resuming where it ends, then merges them into one MP4. */

#include <PalmOS.h>

#include "detail.h"
#include "download.h"
#include "mp4_merge.h"
#include "resource.h"
#include "thumbs.h"

#define CHUNK_BYTES (4UL * 1024UL * 1024UL)
#define PROGRESS_INTERVAL_TICKS 50
/* Redirects in a row a download follows. */
#define MAX_REDIRECTS 5

static YtFormat *CurrentFormat(const AppState *s)
{
    return s->stream == 0 ? &s->playerP->video[VideoChoice(s)]
        : &s->playerP->audio;
}

void CloseFile(AppState *s)
{
    if (s->fileOpen) VFSFileClose(s->file);
    s->fileOpen = false;
}

static void ShowTransfer(AppState *s, Boolean force)
{
    const YtFormat *formatP = CurrentFormat(s);
    UInt32 now = TimGetTicks();
    UInt32 elapsed = now - s->sessionStartTicks;
    if (!force && now - s->lastProgressTicks < PROGRESS_INTERVAL_TICKS) return;
    s->lastProgressTicks = now;
    StrCopy(s->progress, s->stream == 0 ? "Video " : "Audio ");
    AppendSize(s->progress, sizeof(s->progress), s->offset);
    Append(s->progress, sizeof(s->progress), " of ");
    AppendSize(s->progress, sizeof(s->progress), formatP->contentLength);
    if (elapsed >= SysTicksPerSecond()) {
        Append(s->progress, sizeof(s->progress), "\n");
        AppendNumber(s->progress, sizeof(s->progress),
            s->sessionBytes / 1024UL * SysTicksPerSecond() / elapsed);
        Append(s->progress, sizeof(s->progress), " KB/s");
    }
    if (s->jobResult.handshakeTicks != 0) {
        Append(s->progress, sizeof(s->progress), ", TLS handshake ");
        AppendSeconds(s->progress, sizeof(s->progress),
            s->jobResult.handshakeTicks);
    }
    s->percent = (UInt16)(formatP->contentLength == 0 ? 0
        : (s->offset / 1024UL) * 100UL / (formatP->contentLength / 1024UL + 1));
    if (OnForm(DetailForm)) {
        SetFieldText(ProgressField, s->progress);
        DrawProgressBar(s);
    }
}

static Int16 WriteMedia(void *contextP, const UInt8 *dataP, UInt16 length)
{
    AppState *s = (AppState *)contextP;
    if (StoreWrite(s->file, dataP, length) != errNone) return 1;
    s->offset += length;
    s->sessionBytes += length;
    ShowTransfer(s, false);
    return 0;
}

static void FailDownload(AppState *s, const Char *messageP)
{
    CloseFile(s);
    s->task = taskIdle;
    s->percent = 0;
    StrCopy(s->progress, messageP);
    Append(s->progress, sizeof(s->progress),
        "\nTap Download to resume.");
}

static void StartMerge(AppState *s);

static void StartChunk(AppState *s)
{
    const YtFormat *formatP = CurrentFormat(s);
    const Char *pathP;
    UInt32 end = s->offset + CHUNK_BYTES - 1;
    UInt16 length;
    if (end >= formatP->contentLength) end = formatP->contentLength - 1;
    length = YtBuildMediaRequest(&s->client, formatP->url, s->offset, end,
        false, s->request, sizeof(s->request));
    if (length == 0 || !YtSplitUrl(formatP->url, s->host, sizeof(s->host),
            &pathP)) {
        FailDownload(s, "Stream URL is not usable.");
        return;
    }
    if (!NetJobStart(&s->jobP, &s->netConfig, s->host, s->request, length,
            WriteMedia, s, &s->jobResult)) {
        FailDownload(s, "Out of memory.");
        return;
    }
    NetJobKeepLocation(s->jobP, s->location, sizeof(s->location));
}

/* Opens the next stream's file and resumes it where it stopped. */
static void StartStream(AppState *s)
{
    while (s->stream < 2) {
        const YtFormat *formatP = CurrentFormat(s);
        const Char *pathP = s->stream == 0 ? s->videoPath : s->audioPath;
        Err error = StoreOpenAppend(s->volume, pathP, &s->file, &s->offset);
        if (error == errNone && s->offset > formatP->contentLength) {
            VFSFileClose(s->file);
            error = StoreCreate(s->volume, pathP, &s->file);
            s->offset = 0;
        }
        if (error != errNone) {
            Char message[64];
            StrCopy(message, "Could not create a file on the card, error ");
            AppendHex(message, sizeof(message), (UInt16)error);
            Append(message, sizeof(message), ".");
            FailDownload(s, message);
            return;
        }
        s->fileOpen = true;
        if (s->offset < formatP->contentLength) {
            ShowTransfer(s, true);
            StartChunk(s);
            return;
        }
        CloseFile(s);
        s->stream++;
    }
    StartMerge(s);
}

void StartDownload(AppState *s)
{
    const YtPlayer *playerP = s->playerP;
    const YtSearchItem *itemP = &s->searchP->items[s->selected];
    Char name[64];
    UInt32 freeBytes = 0, needed;
    Err error;
    StopThumbs(s);
    if (s->task != taskIdle || !YtPlayerPlayable(playerP)) return;
    error = StoreOpenVolume(&s->volume);
    if (error != errNone) {
        SetProgressText(s, "No memory card found.");
        return;
    }
    /* Both streams plus the merged copy. */
    needed = (playerP->video[VideoChoice(s)].contentLength +
        playerP->audio.contentLength) * 2UL + 65536UL;
    if (StoreFreeBytes(s->volume, &freeBytes) == errNone && freeBytes < needed) {
        StrCopy(s->progress, "Not enough space on the card: need ");
        AppendSize(s->progress, sizeof(s->progress), needed);
        Append(s->progress, sizeof(s->progress), ".");
        RefreshDetail(s);
        return;
    }
    error = EnsureNetwork(s);
    if (error != errNone) {
        SetProgressText(s, "Wi-Fi unavailable.");
        return;
    }
    StrCopy(name, itemP->videoId);
    StrCat(name, ".video.mp4");
    StoreMakePath(s->videoPath, name);
    StrCopy(name, itemP->videoId);
    StrCat(name, ".audio.mp4");
    StoreMakePath(s->audioPath, name);
    MakeOutPath(s);
    s->task = taskDownload;
    s->taskStartTicks = TimGetTicks();
    s->sessionStartTicks = s->taskStartTicks;
    s->sessionBytes = 0;
    s->lastProgressTicks = 0;
    s->redirects = 0;
    s->stream = 0;
    RefreshDetail(s);
    StartStream(s);
    RefreshDetail(s);
}

void FinishChunk(AppState *s)
{
    Char message[80];
    /* YouTube moves some streams to another server. */
    if (s->jobResult.redirected) {
        YtFormat *formatP = CurrentFormat(s);
        if (++s->redirects <= MAX_REDIRECTS &&
            YtApplyRedirect(formatP->url, sizeof(formatP->url), s->location)) {
            StartChunk(s);
            return;
        }
    }
    s->redirects = 0;
    if (s->jobResult.status != netJobOk) {
        DescribeFailure(s, message, sizeof(message));
        FailDownload(s, message);
        return;
    }
    if (s->jobResult.received == 0) {
        FailDownload(s, "The server sent no data.");
        return;
    }
    if (s->offset < CurrentFormat(s)->contentLength) {
        StartChunk(s);
        return;
    }
    CloseFile(s);
    s->stream++;
    StartStream(s);
}

static Boolean MergeProgress(void *contextP, UInt32 done, UInt32 total)
{
    AppState *s = (AppState *)contextP;
    UInt32 now = TimGetTicks();
    EvtResetAutoOffTimer();
    if (now - s->lastProgressTicks < PROGRESS_INTERVAL_TICKS) return true;
    s->lastProgressTicks = now;
    s->percent = (UInt16)((done / 1024UL) * 100UL / (total / 1024UL + 1));
    DrawProgressBar(s);
    return true;
}

static void StartMerge(AppState *s)
{
    Mp4Io io;
    Mp4MergeResult result;
    FileRef videoFile = 0, audioFile = 0, outFile = 0;
    Err error;
    MemSet(&io, sizeof(io), 0);
    s->percent = 0;
    SetProgressText(s, "Merging video and audio...");
    DrawProgressBar(s);
    error = StoreOpenRead(s->volume, s->videoPath, &videoFile, &io.videoSize);
    if (error == errNone)
        error = StoreOpenRead(s->volume, s->audioPath, &audioFile,
            &io.audioSize);
    if (error == errNone) error = StoreCreate(s->volume, s->outPath, &outFile);
    if (error != errNone) {
        Char message[64];
        if (videoFile != 0) VFSFileClose(videoFile);
        if (audioFile != 0) VFSFileClose(audioFile);
        StrCopy(message, "Could not open the streams for merging, error ");
        AppendHex(message, sizeof(message), (UInt16)error);
        Append(message, sizeof(message), ".");
        FailDownload(s, message);
        return;
    }
    io.readProcP = StoreMp4Read;
    io.writeProcP = StoreMp4Write;
    io.progressProcP = MergeProgress;
    io.contextP = s;
    io.videoP = (void *)videoFile;
    io.audioP = (void *)audioFile;
    io.outP = (void *)outFile;
    Mp4Merge(&io, &result);
    VFSFileClose(videoFile);
    VFSFileClose(audioFile);
    VFSFileClose(outFile);
    s->task = taskIdle;
    if (result.status != mp4MergeOk) {
        VFSFileDelete(s->volume, s->outPath);
        s->saved = false;
        StrCopy(s->progress, "Merge failed: status ");
        AppendNumber(s->progress, sizeof(s->progress), result.status);
        Append(s->progress, sizeof(s->progress), ", line ");
        AppendNumber(s->progress, sizeof(s->progress), result.detail);
        s->percent = 0;
        return;
    }
    VFSFileDelete(s->volume, s->videoPath);
    VFSFileDelete(s->volume, s->audioPath);
    s->percent = 100;
    FindSavedCopy(s);
    StrCopy(s->progress, "Saved ");
    Append(s->progress, sizeof(s->progress), s->outPath);
    Append(s->progress, sizeof(s->progress), " in ");
    AppendSeconds(s->progress, sizeof(s->progress),
        TimGetTicks() - s->taskStartTicks);
    Append(s->progress, sizeof(s->progress), ".");
}
