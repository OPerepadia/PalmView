/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The player: streams or plays a saved file under the picture's seek bar,
 * and turns the picture to full screen. */

#include <PalmOS.h>
#include <PenInputMgr.h>

#include "player.h"
#include "resource.h"
#include "saved.h"
#include "thumbs.h"

/* 5-way left and right skip about this far, once no press has come for
 * SKIP_DELAY_TICKS: presses in a row add up to one seek. */
#define SKIP_MS 10000UL
#define SKIP_DELAY_TICKS (SysTicksPerSecond() / 2)
/* Leaves room for the title's tab around the text, and beside it for the
 * quality picker when there is one. */
#define PLAY_TITLE_WIDTH 150
#define PLAY_TITLE_SHORT_WIDTH 112

/* 3 dB apart; 1024 is unity gain. */
static const UInt16 kVolumeGain[VOLUME_LEVELS] = {
    0, 45, 64, 91, 128, 181, 256, 362, 512, 724, 1024
};

/* "1:05" or "1:02:05" */
static void AppendClock(Char *dstP, UInt16 capacity, UInt32 ms)
{
    UInt32 seconds = ms / 1000UL;
    if (seconds >= 3600UL) {
        AppendNumber(dstP, capacity, seconds / 3600UL);
        Append(dstP, capacity, seconds % 3600UL < 600UL ? ":0" : ":");
        seconds %= 3600UL;
    }
    AppendNumber(dstP, capacity, seconds / 60UL);
    Append(dstP, capacity, seconds % 60UL < 10UL ? ":0" : ":");
    AppendNumber(dstP, capacity, seconds % 60UL);
}

void GetVideoBounds(RectangleType *boundsP)
{
    FormType *formP = FrmGetActiveForm();
    FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, PlayVideoGadget), boundsP);
}

void ClearVideoArea(void)
{
    RectangleType bounds;
    if (!OnForm(PlayForm)) return;
    GetVideoBounds(&bounds);
    WinDrawRectangle(&bounds, 0);
}

static void GetSeekBounds(RectangleType *boundsP)
{
    FormType *formP = FrmGetActiveForm();
    FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, PlaySeekGadget), boundsP);
}

/* value / total of width, for times of many hours too. */
static Coord BarX(UInt32 value, UInt32 total, Coord width)
{
    if (total == 0) return 0;
    if (value > total) value = total;
    while (total > 0x003FFFFFUL) {
        total >>= 1;
        value >>= 1;
    }
    return (Coord)(value * (UInt32)width / total);
}

static void FillBar(RectangleType *rP, Coord width, UInt8 gray)
{
    RGBColorType color;
    color.r = gray;
    color.g = gray;
    color.b = gray;
    WinSetForeColorRGB(&color, 0);
    rP->extent.x = width;
    WinDrawRectangle(rP, 0);
}

/* In the color theme's title bar color */
static void FillThemeBar(RectangleType *rP, Coord width)
{
    WinSetForeColor(UIColorGetTableEntryIndex(UIFormFrame));
    rP->extent.x = width;
    WinDrawRectangle(rP, 0);
}

/* The bar under the video: played, downloaded, and not loaded yet. While
 * the pen drags it, it shows where the pen points. */
void DrawSeekBar(const AppState *s)
{
    const Playback *p = s->playbackP;
    RectangleType bounds, bar;
    UInt32 duration = 0, position = 0, loaded = 0;
    Coord played;
    if (!OnForm(PlayForm)) return;
    GetSeekBounds(&bounds);
    WinEraseRectangle(&bounds, 0);
    if (p == 0 || (duration = PlaybackDurationMs(p)) == 0) return;
    position = s->seekDragging || s->skipPending ? s->seekTargetMs
        : PlaybackPositionMs(p);
    loaded = duration;
    if (s->feedP != 0) {
        UInt32 video = PlaybackBufferedMs(p, STREAM_FEED_VIDEO);
        UInt32 audio = PlaybackBufferedMs(p, STREAM_FEED_AUDIO);
        loaded = PlaybackPositionMs(p) + (video < audio ? video : audio);
    }
    played = BarX(position, duration, bounds.extent.x - 4);
    WinPushDrawState();
    RctSetRectangle(&bar, bounds.topLeft.x, bounds.topLeft.y + 2, 0, 2);
    FillBar(&bar, bounds.extent.x, 0xC8);
    FillBar(&bar, BarX(loaded, duration, bounds.extent.x), 0x80);
    FillThemeBar(&bar, played);
    RctSetRectangle(&bar, bounds.topLeft.x + played, bounds.topLeft.y + 1, 0, 4);
    FillThemeBar(&bar, 4);
    WinPopDrawState();
}

/* The time under the pen's x on the seek bar */
static UInt32 SeekBarMs(const AppState *s, Coord penX)
{
    RectangleType bounds;
    UInt32 duration = PlaybackDurationMs(s->playbackP), ms;
    GetSeekBounds(&bounds);
    if (penX <= bounds.topLeft.x) return 0;
    ms = (UInt32)(penX - bounds.topLeft.x) * (duration / 1000UL) /
        (UInt32)bounds.extent.x * 1000UL;
    return ms < duration ? ms : duration;
}

static void GetVolumeBounds(RectangleType *boundsP)
{
    FormType *formP = FrmGetActiveForm();
    FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, PlayVolumeGadget), boundsP);
}

/* Ten bars rising left to right; the colored ones show the volume. */
void DrawVolume(const AppState *s)
{
    RectangleType bounds, bar;
    UInt16 coordinates, level;
    Coord step, shortest;
    if (!OnForm(PlayForm)) return;
    GetVolumeBounds(&bounds);
    WinPushDrawState();
    coordinates = WinSetCoordinateSystem(kCoordinatesNative);
    WinScaleRectangle(&bounds);
    WinEraseRectangle(&bounds, 0);
    step = bounds.extent.x / (VOLUME_LEVELS - 1);
    shortest = bounds.extent.y / 4;
    for (level = 1; level < VOLUME_LEVELS; level++) {
        bar.extent.y = shortest + (bounds.extent.y - shortest) * (level - 1) /
            (VOLUME_LEVELS - 2);
        bar.topLeft.x = bounds.topLeft.x + (level - 1) * step;
        bar.topLeft.y = bounds.topLeft.y + bounds.extent.y - bar.extent.y;
        if (level <= s->prefs.volume) FillThemeBar(&bar, step * 2 / 3);
        else FillBar(&bar, step * 2 / 3, 0xC8);
    }
    WinSetCoordinateSystem(coordinates);
    WinPopDrawState();
}

static void SetVolume(AppState *s, UInt16 level)
{
    if (level == s->prefs.volume) return;
    s->prefs.volume = level;
    s->prefsChanged = true;
    if (s->playbackP != 0)
        PlaybackSetVolume(s->playbackP, kVolumeGain[level]);
    DrawVolume(s);
}

/* Sets the volume to the bar under the pen. Left of the bars mutes. */
Boolean TrackVolume(AppState *s, const EventType *eventP)
{
    RectangleType bounds;
    Coord step;
    UInt16 level;
    GetVolumeBounds(&bounds);
    if (eventP->eType == penDownEvent) {
        /* Take the whole height of the button row. */
        bounds.topLeft.y -= 2;
        bounds.extent.y += 3;
        if (!RctPtInRectangle(eventP->screenX, eventP->screenY, &bounds))
            return false;
        s->volumeDragging = true;
    } else if (!s->volumeDragging) {
        return false;
    }
    if (eventP->eType == penUpEvent) s->volumeDragging = false;
    step = bounds.extent.x / (VOLUME_LEVELS - 1);
    if (eventP->screenX < bounds.topLeft.x) level = 0;
    else level = (UInt16)((eventP->screenX - bounds.topLeft.x) / step) + 1;
    SetVolume(s, level < VOLUME_LEVELS ? level : VOLUME_LEVELS - 1);
    return true;
}

static void UpdatePlayStatus(AppState *s)
{
    static const Char kQuality[PLAYBACK_LEVELS][12] = {
        "Full", "Fast B", "Fast", "Half rate"
    };
    const Playback *p = s->playbackP;
    Char *textP = s->playStatus;
    UInt16 capacity = sizeof(s->playStatus);
    if (!OnForm(PlayForm)) return;
    textP[0] = '\0';
    if (PlaybackError(p) != 0) {
        Append(textP, capacity, PlaybackError(p));
        /* What dropped the stream, then how the last retry failed. */
        if (s->feedP != 0 && StreamFeedFailure(s->feedP) != 0) {
            const NetJobResult *dropP = StreamFeedDrop(s->feedP);
            const NetJobResult *lastP = StreamFeedFailure(s->feedP);
            if (dropP != 0 && MemCmp(dropP, lastP, sizeof(*lastP)) != 0) {
                Append(textP, capacity, "\n");
                DescribeResult(dropP, false, textP, capacity);
            }
            Append(textP, capacity, "\n");
            DescribeResult(lastP, false, textP, capacity);
        }
        SetFieldText(PlayStatusField, textP);
        return;
    }
    AppendClock(textP, capacity, s->seekDragging || s->skipPending
        ? s->seekTargetMs : PlaybackPositionMs(p));
    Append(textP, capacity, " / ");
    AppendClock(textP, capacity, PlaybackDurationMs(p));
    if (s->seekDragging || s->skipPending) Append(textP, capacity, "  Seek");
    else if (p->ended) Append(textP, capacity, "  Ended");
    else if (!p->playing) Append(textP, capacity, "  Paused");
    else if (p->buffering || (s->feedP != 0 && !p->started))
        Append(textP, capacity, "  Buffering");
    Append(textP, capacity, "\n");
    AppendNumber(textP, capacity, p->fps10 / 10);
    Append(textP, capacity, ".");
    AppendNumber(textP, capacity, p->fps10 % 10);
    Append(textP, capacity, " fps, CPU ");
    AppendNumber(textP, capacity, p->cpuPercent);
    Append(textP, capacity, "%, skipped ");
    AppendNumber(textP, capacity, p->skipped);
    if (p->soundError != 0) {
        Append(textP, capacity, "\nNo sound: ");
        Append(textP, capacity, p->soundError);
    } else {
        Append(textP, capacity, "\n");
        Append(textP, capacity, kQuality[p->level]);
        Append(textP, capacity, ", ");
        AppendNumber(textP, capacity, p->soundUnderruns);
        Append(textP, capacity, " gaps");
    }
    if (s->feedP != 0) {
        Append(textP, capacity, ", ");
        AppendNumber(textP, capacity, p->netKBps);
        Append(textP, capacity, " KB/s");
    }
    if (p->decodeErrors + p->audioErrors != 0) {
        Append(textP, capacity, ", errors ");
        AppendNumber(textP, capacity, p->decodeErrors + p->audioErrors);
    }
    SetFieldText(PlayStatusField, textP);
}

static void ShowPauseLabel(const AppState *s)
{
    const Playback *p = s->playbackP;
    if (!OnForm(PlayForm) || p == 0) return;
    CtlSetLabel((ControlType *)GetObject(PlayPauseButton),
        p->ended ? "Replay" : p->playing ? "Pause" : "Play");
}

void ClosePlayback(AppState *s)
{
    if (s->playbackP != 0) {
        PlaybackClose(s->playbackP);
        MemPtrFree(s->playbackP);
        s->playbackP = 0;
    }
    StreamFeedFree(s->feedP);
    s->feedP = 0;
    s->playWait = evtWaitForever;
    s->resumeOnEnter = false;
}

/* Streams the selected video from startMs when playPath is empty. When
 * 240p cannot start, for want of memory, it streams 144p. */
static const Char *StartPlayback(AppState *s, UInt32 startMs)
{
    UInt16 choice = VideoChoice(s);
    const Char *errorP;
    s->playbackP = (Playback *)MemPtrNew(sizeof(Playback));
    if (s->playbackP == 0) return "Out of memory.";
    if (s->playPath[0] != '\0') return PlaybackOpen(s->playbackP, s->playPath);
    if (EnsureNetwork(s) != errNone) {
        MemSet(s->playbackP, sizeof(Playback), 0);
        return "Wi-Fi unavailable.";
    }
    for (;;) {
        s->feedP = StreamFeedNew(&s->netConfig, &s->client,
            &s->playerP->video[choice], &s->playerP->audio,
            s->playerP->lengthSeconds);
        if (s->feedP == 0) {
            MemSet(s->playbackP, sizeof(Playback), 0);
            return "Out of memory.";
        }
        s->playChoice = choice;
        errorP = PlaybackOpenStream(s->playbackP, s->feedP, choice != quality144,
            startMs);
        if (errorP == 0 || choice == quality144 ||
            !s->playerP->video[quality144].found)
            return errorP;
        UdpLogLine(s->logP, "play", "240p: no memory, playing 144p", 0, 0, 0);
        PlaybackClose(s->playbackP);
        StreamFeedFree(s->feedP);
        s->feedP = 0;
        choice = quality144;
    }
}

/* The quality picker shows what is streaming. */
static void ShowPlayQuality(const AppState *s)
{
    CtlSetLabel((ControlType *)GetObject(PlayQualityTrigger),
        kQualityNames[s->playChoice]);
    LstSetSelection((ListType *)GetObject(PlayQualityList), (Int16)s->playChoice);
}

/* Draws the video into PlayForm's gadget. */
const Char *AttachPlayScreen(AppState *s)
{
    FormType *formP = FrmGetActiveForm();
    RectangleType bounds, formBounds;
    GetVideoBounds(&bounds);
    WinGetBounds(FrmGetWindowHandle(formP), &formBounds);
    bounds.topLeft.x += formBounds.topLeft.x;
    bounds.topLeft.y += formBounds.topLeft.y;
    return PlaybackAttachScreen(s->playbackP, &bounds, false);
}

/* Opens playback from startMs, and plays it if play is set. */
static void StartPlayAt(AppState *s, UInt32 startMs, Boolean play)
{
    const Char *errorP = StartPlayback(s, startMs);
    if (errorP == 0) {
        PlaybackSetVolume(s->playbackP, kVolumeGain[s->prefs.volume]);
        errorP = AttachPlayScreen(s);
    }
    if (errorP != 0) {
        ClosePlayback(s);
        StrCopy(s->playStatus, errorP);
        SetFieldText(PlayStatusField, s->playStatus);
        ShowObject(PlayPauseButton, false);
        return;
    }
    if (s->feedP != 0) ShowPlayQuality(s);
    if (s->logP != 0) {
        const Playback *p = s->playbackP;
        UInt32 freeBytes, maxChunk;
        MemHeapFreeBytes(0, &freeBytes, &maxChunk);
        UdpLogLine(s->logP, "mem", "before: heap % KB free, largest % KB",
            p->heapFree / 1024, p->heapChunk / 1024, 0);
        UdpLogLine(s->logP, "mem", "arena % KB, rings % + % KB",
            p->arenaSize / 1024, p->ringSizes[0] / 1024, p->ringSizes[1] / 1024);
        if (p->extraSize != 0)
            UdpLogLine(s->logP, "mem", "extra % KB", p->extraSize / 1024, 0, 0);
        UdpLogLine(s->logP, "mem", "after: heap % KB free, thumbnails % KB",
            freeBytes / 1024, ThumbBytes(s) / 1024, 0);
    }
    if (play) PlaybackPlay(s->playbackP);
    ShowPauseLabel(s);
    ShowObject(PlayPauseButton, true);
    s->playWait = 0;
    s->playStatusTicks = TimGetTicks();
    DrawSeekBar(s);
}

void OpenPlayForm(AppState *s)
{
    FormType *formP = FrmGetActiveForm();
    const Char *nameP = s->playPath, *p;
    UInt16 length;
    Coord width = PLAY_TITLE_WIDTH;
    Boolean picker = s->playPath[0] == '\0' &&
        s->playerP->video[quality144].found && s->playerP->video[quality240].found;
    FontID font;
    for (p = s->playPath; *p != '\0'; p++)
        if (*p == '/') nameP = p + 1;
    if (s->playPath[0] == '\0')
        nameP = s->playerP->title[0] != '\0' ? s->playerP->title
            : s->searchP->items[s->selected].title;
    length = StrLen(nameP);
    if (s->playPath[0] != '\0' && length > 4) length -= 4;    /* ".mp4" */
    if (length >= sizeof(s->playTitle))
        length = TextCharStart(&s->codec, nameP, sizeof(s->playTitle) - 1);
    MemMove(s->playTitle, nameP, length);
    s->playTitle[length] = '\0';
    if (picker) width = PLAY_TITLE_SHORT_WIDTH;
    font = FntSetFont(boldFont);
    if (FntCharsWidth(s->playTitle, (Int16)length) > width) {
        const Char *ellipsisP = TextEllipsis(&s->codec);
        UInt16 ellipsis = StrLen(ellipsisP);
        while (length > 0 && (length + ellipsis >= sizeof(s->playTitle) ||
                FntCharsWidth(s->playTitle, (Int16)length) +
                FntCharsWidth(ellipsisP, (Int16)ellipsis) > width))
            length = TextCharStart(&s->codec, s->playTitle, (UInt16)(length - 1));
        StrCopy(s->playTitle + length, ellipsisP);
    }
    FntSetFont(font);
    FrmSetTitle(formP, s->playTitle);
    StrCopy(s->playStatus, s->playPath[0] != '\0' ? "Loading..." : "Connecting...");
    FldSetTextPtr((FieldType *)GetObject(PlayStatusField), s->playStatus);
    s->seekDragging = false;
    s->volumeDragging = false;
    s->skipPending = false;
    if (picker) {
        s->playChoice = VideoChoice(s);
        ShowPlayQuality(s);
        ShowObject(PlayQualityTrigger, true);
    }
    FrmDrawForm(formP);
    ClearVideoArea();
    DrawVolume(s);
    StartPlayAt(s, 0, true);
}

/* Streams the chosen quality from where the other one got to. */
void SwitchPlayQuality(AppState *s, UInt16 choice)
{
    Playback *p = s->playbackP;
    UInt32 ms;
    Boolean play;
    s->prefs.quality = choice;
    s->prefsChanged = true;
    if (p == 0 || s->feedP == 0 || choice == s->playChoice) return;
    ms = s->skipPending ? s->seekTargetMs : PlaybackPositionMs(p);
    play = p->playing;
    s->skipPending = false;
    ClosePlayback(s);
    ClearVideoArea();
    StrCopy(s->playStatus, "Switching to ");
    Append(s->playStatus, sizeof(s->playStatus), kQualityNames[choice]);
    Append(s->playStatus, sizeof(s->playStatus), "...");
    SetFieldText(PlayStatusField, s->playStatus);
    StartPlayAt(s, ms, play);
}

/* Full screen: the input area and status bar go, and the picture is
 * drawn turned, for the TX held in landscape with the 5-way on the right.
 * The display's landscape mode cannot be used: the OS then keeps the
 * screen in a separate bitmap that it turns itself, and only when it
 * draws. */
void FillFullScreen(AppState *s)
{
    RectangleType bounds;
    Coord w, h;
    WinGetDisplayExtent(&w, &h);
    RctSetRectangle(&bounds, 0, 0, w, h);
    WinSetBounds(FrmGetWindowHandle(FrmGetActiveForm()), &bounds);
    WinDrawRectangle(&bounds, 0);
    if (s->playbackP != 0) PlaybackAttachScreen(s->playbackP, &bounds, true);
    s->playWait = 0;
}

static void ClearPicture(AppState *s)
{
    if (OnForm(FullForm)) FillFullScreen(s);
    else ClearVideoArea();
}

void EnterFullScreen(AppState *s)
{
    UInt32 version;
    if (s->playbackP == 0 || FtrGet(pinCreator, pinFtrAPIVersion, &version) != errNone)
        return;
    s->fullScreen = true;
    PlaybackDetachScreen(s->playbackP);
    FrmPopupForm(FullForm);
}

void OpenFullForm(AppState *s)
{
    FormType *formP = FrmGetActiveForm();
    FrmSetDIAPolicyAttr(formP, frmDIAPolicyCustom);
    WinSetConstraintsSize(FrmGetWindowHandle(formP), 160, 240, 240, 160, 160, 160);
    s->inputArea = PINGetInputAreaState();
    PINSetInputAreaState(pinInputAreaClosed);
    PINSetInputTriggerState(pinInputTriggerDisabled);
    StatHide();
    FrmDrawForm(formP);
    FillFullScreen(s);
}

void ShowSystemBars(const AppState *s)
{
    StatShow();
    PINSetInputTriggerState(pinInputTriggerEnabled);
    PINSetInputAreaState(s->inputArea);
}

void ExitFullScreen(AppState *s)
{
    s->fullScreen = false;
    if (s->playbackP != 0) PlaybackDetachScreen(s->playbackP);
    ShowSystemBars(s);
    FrmReturnToForm(PlayForm);
    FrmDrawForm(FrmGetActiveForm());
    ClearVideoArea();
    DrawVolume(s);
    if (s->playbackP != 0) AttachPlayScreen(s);
    ShowPauseLabel(s);
    UpdatePlayStatus(s);
    DrawSeekBar(s);
    s->playWait = 0;
}

/* After a seek: an ended video plays again from there. */
static void FinishSeek(AppState *s, Boolean wasEnded)
{
    Playback *p = s->playbackP;
    if (wasEnded) PlaybackPlay(p);
    ShowPauseLabel(s);
    s->playWait = 0;
    UpdatePlayStatus(s);
    DrawSeekBar(s);
}

void TogglePause(AppState *s)
{
    Playback *p = s->playbackP;
    if (p == 0) return;
    if (p->playing) {
        PlaybackPause(p);
    } else {
        /* Wi-Fi may have gone off with the screen. */
        if (s->feedP != 0 && !NetworkOpen(s)) {
            StreamFeedReconnect(s->feedP);
            if (EnsureNetwork(s) != errNone) return;
        }
        /* After a lost connection, go on from where it stopped. */
        if (s->feedP != 0 && p->ended && PlaybackError(p) != 0 &&
                PlaybackCanSeek(p)) {
            ClearPicture(s);
            PlaybackSeek(p, PlaybackPositionMs(p));
            FinishSeek(s, true);
            return;
        }
        if (p->ended) ClearPicture(s);
        PlaybackPlay(p);
    }
    ShowPauseLabel(s);
    s->playWait = 0;
    UpdatePlayStatus(s);
    DrawSeekBar(s);
}

/* Moves the target; StepPlayback seeks once the presses stop. */
static void Skip(AppState *s, Boolean forward)
{
    Playback *p = s->playbackP;
    UInt32 now = TimGetTicks(), from, duration = PlaybackDurationMs(p);
    /* One press may arrive as two events. */
    if (now - s->lastSkip < SysTicksPerSecond() / 4 || !PlaybackCanSeek(p))
        return;
    s->lastSkip = now;
    from = s->skipPending ? s->seekTargetMs : PlaybackPositionMs(p);
    s->seekTargetMs = forward ? (from + SKIP_MS < duration ? from + SKIP_MS : duration)
        : from > SKIP_MS ? from - SKIP_MS : 0;
    s->skipPending = true;
    UpdatePlayStatus(s);
    DrawSeekBar(s);
}

static void FinishSkip(AppState *s)
{
    Playback *p = s->playbackP;
    UInt32 position = PlaybackPositionMs(p);
    Boolean wasEnded = p->ended, forward = s->seekTargetMs > position;
    s->skipPending = false;
    PlaybackSkip(p, forward, forward ? s->seekTargetMs - position
        : position - s->seekTargetMs);
    FinishSeek(s, wasEnded);
}

/* The pen on the seek bar: dragging shows where it points, lifting it
 * seeks there. Returns true if the event was the bar's. */
Boolean TrackSeekBar(AppState *s, const EventType *eventP)
{
    RectangleType bounds;
    Boolean wasEnded;
    if (eventP->eType == penDownEvent) {
        GetSeekBounds(&bounds);
        /* The bar is thin: take the gap above and below it too. */
        bounds.topLeft.y -= 1;
        bounds.extent.y += 2;
        if (!RctPtInRectangle(eventP->screenX, eventP->screenY, &bounds) ||
            !PlaybackCanSeek(s->playbackP))
            return false;
        s->seekDragging = true;
    } else if (!s->seekDragging) {
        return false;
    }
    s->seekTargetMs = SeekBarMs(s, eventP->screenX);
    if (eventP->eType == penUpEvent) {
        s->seekDragging = false;
        wasEnded = s->playbackP->ended;
        PlaybackSeek(s->playbackP, s->seekTargetMs);
        FinishSeek(s, wasEnded);
        return true;
    }
    UpdatePlayStatus(s);
    DrawSeekBar(s);
    return true;
}

/* The player form at 160 high, from app.rcp */
#define PLAY_HEIGHT 160
#define PLAY_LINE 11
#define PLAY_STATUS_Y 111
#define PLAY_STATUS_HEIGHT 33
#define PLAY_BUTTONS_Y 145
#define PLAY_VOLUME_Y 147

/* Fits the form to the screen: the picture and seek bar stay at the top,
 * the status text gets the extra lines, and the controls move to the
 * bottom. */
void LayoutPlayForm(void)
{
    static const UInt16 kButtonIds[] = { PlayPauseButton, PlayBackButton };
    FormType *formP = FrmGetActiveForm();
    RectangleType bounds;
    Coord width, height, extra;
    UInt16 i;
    if (!HasInputArea()) return;
    WinGetDisplayExtent(&width, &height);
    RctSetRectangle(&bounds, 0, 0, width, height);
    WinSetBounds(FrmGetWindowHandle(formP), &bounds);
    extra = height > PLAY_HEIGHT ? height - PLAY_HEIGHT : 0;
    SetObjectY(formP, FrmGetObjectIndex(formP, PlayStatusField), PLAY_STATUS_Y,
        PLAY_STATUS_HEIGHT + extra / PLAY_LINE * PLAY_LINE);
    SetObjectY(formP, FrmGetObjectIndex(formP, PlayVolumeGadget),
        PLAY_VOLUME_Y + extra, 0);
    for (i = 0; i < sizeof(kButtonIds) / sizeof(kButtonIds[0]); i++)
        SetObjectY(formP, FrmGetObjectIndex(formP, kButtonIds[i]),
            PLAY_BUTTONS_Y + extra, 0);
}

void ReturnFromPlay(AppState *s)
{
    ClosePlayback(s);
    FrmReturnToForm(0);
    if (OnForm(DetailForm)) LayoutDetailForm();
    else if (OnForm(SavedForm)) LayoutSavedForm();
    FrmDrawForm(FrmGetActiveForm());
    if (OnForm(DetailForm)) {
        /* The sizes are for the quality chosen in the player. */
        BuildInfo(s);
        RefreshDetail(s);
        StartThumbs(s);
    }
}

/* Runs playback after each event while PlayForm is up. */
void StepPlayback(AppState *s)
{
    Playback *p = s->playbackP;
    Boolean wasPlaying;
    UInt32 now;
    if (p == 0) return;
    /* Another form is on top; winEnterEvent wakes playback up again. */
    if (!OnForm(PlayForm) && !OnForm(FullForm)) {
        s->playWait = evtWaitForever;
        return;
    }
    wasPlaying = p->playing;
    s->playWait = PlaybackStep(p);
    now = TimGetTicks();
    if (s->skipPending) {
        UInt32 waited = now - s->lastSkip;
        if (waited >= SKIP_DELAY_TICKS) {
            FinishSkip(s);
        } else if (s->playWait < 0 ||
                   (UInt32)s->playWait > SKIP_DELAY_TICKS - waited) {
            s->playWait = (Int32)(SKIP_DELAY_TICKS - waited);
        }
    }
    if (wasPlaying && p->ended) {
        ShowPauseLabel(s);
        UpdatePlayStatus(s);
        DrawSeekBar(s);
    } else if ((p->playing || (s->feedP != 0 && !p->ended)) &&
               now - s->playStatusTicks >= SysTicksPerSecond()) {
        /* Paused streams keep downloading: the bar shows it. */
        s->playStatusTicks = now;
        if (s->feedP != 0) {
            const Char *tagP = !p->playing ? "pause"
                : p->buffering || !p->started ? "wait" : "play";
            UdpLogLine(s->logP, tagP, "at % ms, buffer % + % ms",
                PlaybackPositionMs(p), PlaybackBufferedMs(p, STREAM_FEED_VIDEO),
                PlaybackBufferedMs(p, STREAM_FEED_AUDIO));
            UdpLogLine(s->logP, tagP, "net % KB/s, cpu %, fps x10 %",
                p->netKBps, p->cpuPercent, p->fps10);
            UdpLogLine(s->logP, tagP, "level %, late %, peak net % ms",
                p->level, p->lateTotal,
                p->peakNet * 1000UL / SysTicksPerSecond());
            UdpLogLine(s->logP, tagP, "net busy %", p->netPercent, 0, 0);
            if (s->logP != 0) {
                UInt32 used, peak;
                PlaybackArenaUse(p, &used, &peak);
                UdpLogLine(s->logP, tagP, "arena % KB used, peak % KB",
                    used / 1024, peak / 1024, 0);
            }
        }
        if (p->playing) EvtResetAutoOffTimer();
        if (!s->seekDragging && !s->skipPending) {
            UpdatePlayStatus(s);
            DrawSeekBar(s);
        }
    }
}

/* The window the picture is in */
Boolean IsPlayWindow(const AppState *s, WinHandle windowH)
{
    FormType *formP = FrmGetFormPtr(s->fullScreen ? FullForm : PlayForm);
    return formP != 0 && windowH == FrmGetWindowHandle(formP);
}

/* The 5-way in the player. Full screen, the TX lies on its side with the
 * 5-way on the right: up points left and skips back, and right turns the
 * volume up. */
Boolean HandlePlayKey(AppState *s, const EventType *eventP)
{
    WChar c = eventP->data.keyDown.chr;
    Boolean up = c == vchrPageUp || c == vchrRockerUp;
    Boolean down = c == vchrPageDown || c == vchrRockerDown;
    Boolean left = c == vchrRockerLeft || NavPressed(eventP, navBitLeft, navChangeLeft);
    Boolean right = c == vchrRockerRight ||
        NavPressed(eventP, navBitRight, navChangeRight);
    Boolean turned = s->fullScreen;
    if (turned ? right : up) {
        if (s->prefs.volume < VOLUME_LEVELS - 1) SetVolume(s, s->prefs.volume + 1);
        return true;
    }
    if (turned ? left : down) {
        if (s->prefs.volume > 0) SetVolume(s, s->prefs.volume - 1);
        return true;
    }
    if (turned ? up : left) {
        Skip(s, false);
        return true;
    }
    if (turned ? down : right) {
        Skip(s, true);
        return true;
    }
    /* One press may arrive as both of these. */
    if (c == vchrRockerCenter || NavPressed(eventP, navBitSelect, navChangeSelect)) {
        UInt32 now = TimGetTicks();
        if (now - s->lastCentre >= SysTicksPerSecond() / 4) TogglePause(s);
        s->lastCentre = now;
        return true;
    }
    return false;
}
