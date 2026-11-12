/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Asks YouTube for the selected video's streams when the screen opens, and
 * shows their sizes for the chosen quality. */

#include <PalmOS.h>

#include "detail.h"
#include "resource.h"
#include "saved.h"
#include "thumbs.h"

/* The details form's layout at 160 high, from app.rcp */
#define DETAIL_HEIGHT 160
#define DETAIL_LINE 11
#define DETAIL_HEAD_Y 16
/* The channel trigger. Its frame is drawn a pixel outside these bounds, so
 * its left edge lines up with the title. */
#define CHANNEL_X 66
#define CHANNEL_WIDTH 92
#define CHANNEL_MARGIN 6
#define DETAIL_INFO_Y 53
#define DETAIL_INFO_HEIGHT 33
#define DETAIL_PROGRESS_Y 94
#define DETAIL_BAR_Y 131
#define DETAIL_BUTTONS_Y 145

/* Fits the form to the screen: the info text grows by whole lines, and
 * the progress bar and buttons stay at the bottom. */
void LayoutDetailForm(void)
{
    static const UInt16 kButtonIds[] = {
        PlayButton, DownloadButton, CancelButton, BackButton
    };
    FormType *formP = FrmGetActiveForm();
    RectangleType bounds;
    Coord width, height, extra, grow;
    UInt16 i;
    if (!HasInputArea()) return;
    WinGetDisplayExtent(&width, &height);
    RctSetRectangle(&bounds, 0, 0, width, height);
    WinSetBounds(FrmGetWindowHandle(formP), &bounds);
    extra = height > DETAIL_HEIGHT ? height - DETAIL_HEIGHT : 0;
    grow = extra / DETAIL_LINE * DETAIL_LINE;
    SetObjectY(formP, FrmGetObjectIndex(formP, InfoField), DETAIL_INFO_Y,
        DETAIL_INFO_HEIGHT + grow);
    SetObjectY(formP, FrmGetObjectIndex(formP, InfoScrollBar), DETAIL_INFO_Y,
        DETAIL_INFO_HEIGHT + grow);
    SetObjectY(formP, FrmGetObjectIndex(formP, ProgressField),
        DETAIL_PROGRESS_Y + grow, 0);
    SetObjectY(formP, FrmGetObjectIndex(formP, ProgressGadget),
        DETAIL_BAR_Y + extra, 0);
    for (i = 0; i < sizeof(kButtonIds) / sizeof(kButtonIds[0]); i++)
        SetObjectY(formP, FrmGetObjectIndex(formP, kButtonIds[i]),
            DETAIL_BUTTONS_Y + extra, 0);
}

static void UpdateInfoScrollBar(void)
{
    FieldType *fieldP = (FieldType *)GetObject(InfoField);
    UInt16 position, textHeight, fieldHeight, maximum;
    FldGetScrollValues(fieldP, &position, &textHeight, &fieldHeight);
    maximum = textHeight > fieldHeight ? textHeight - fieldHeight : 0;
    SclSetScrollBar((ScrollBarType *)GetObject(InfoScrollBar),
        (Int16)position, 0, (Int16)maximum,
        (Int16)(fieldHeight > 1 ? fieldHeight - 1 : 1));
}

void ScrollInfo(Int16 lines)
{
    FieldType *fieldP = (FieldType *)GetObject(InfoField);
    if (lines > 0) FldScrollField(fieldP, (UInt16)lines, winDown);
    else if (lines < 0) FldScrollField(fieldP, (UInt16)-lines, winUp);
    UpdateInfoScrollBar();
}

/* Appends textP as a field width wide wraps it, cut with "..." after two
 * lines. */
static void AppendTwoLines(const TextCodec *codecP, Char *dstP,
                           UInt16 capacity, const Char *textP, Coord width)
{
    const Char *ellipsisP = TextEllipsis(codecP);
    UInt16 length = StrLen(dstP), first, second, ellipsis = StrLen(ellipsisP);
    FontID font = FntSetFont(stdFont);
    first = FntWordWrap(textP, (UInt16)width);
    second = StrLen(textP + first);
    if (FntWordWrap(textP + first, (UInt16)width) < second) {
        while (second > 0 && FntCharsWidth(textP + first, (Int16)second) +
                FntCharsWidth(ellipsisP, (Int16)ellipsis) > width)
            second = TextCharStart(codecP, textP + first, (UInt16)(second - 1));
        while (second > 0 && textP[first + second - 1] == ' ') second--;
        if (length + first + second + ellipsis < capacity) {
            MemMove(dstP + length, textP, first + second);
            StrCopy(dstP + length + first + second, ellipsisP);
        }
    } else {
        Append(dstP, capacity, textP);
    }
    FntSetFont(font);
}

static void AppendInfoLine(Char *infoP, UInt16 capacity, const Char *textP)
{
    if (infoP[0] != '\0') Append(infoP, capacity, "\n");
    Append(infoP, capacity, textP);
}

/* The chosen quality, or the one YouTube offers instead */
UInt16 VideoChoice(const AppState *s)
{
    const YtFormat *formatP = YtPlayerVideo(s->playerP, s->quality);
    return formatP != 0 ? (UInt16)(formatP - s->playerP->video) : quality144;
}

/* The channel's line becomes a trigger, right under the title, when the
 * list named the channel's ID. */
void ShowChannelTrigger(AppState *s)
{
    FormType *formP = FrmGetActiveForm();
    FieldType *fieldP = (FieldType *)GetObject(HeadField);
    RectangleType bounds;
    Coord lines;
    FontID font;
    if (s->searchP->items[s->selected].channelId[0] == '\0') return;
    font = FntSetFont(stdFont);
    FldGetBounds(fieldP, &bounds);
    lines = s->head[FntWordWrap(s->head, (UInt16)bounds.extent.x)] != '\0' ? 2 : 1;
    SetObjectY(formP, FrmGetObjectIndex(formP, HeadField), DETAIL_HEAD_Y,
        lines * DETAIL_LINE);
    FitText(&s->codec, s->channelLabel, s->searchP->items[s->selected].channel,
        CHANNEL_WIDTH - CHANNEL_MARGIN);
    RctSetRectangle(&bounds, CHANNEL_X, DETAIL_HEAD_Y + lines * DETAIL_LINE,
        FntCharsWidth(s->channelLabel, (Int16)StrLen(s->channelLabel)) +
        CHANNEL_MARGIN, DETAIL_LINE);
    FntSetFont(font);
    FrmSetObjectBounds(formP, FrmGetObjectIndex(formP, ChannelTrigger), &bounds);
    CtlSetLabel((ControlType *)GetObject(ChannelTrigger), s->channelLabel);
    ShowObject(ChannelTrigger, true);
}

void BuildInfo(AppState *s)
{
    const YtSearchItem *itemP = &s->searchP->items[s->selected];
    const YtPlayer *playerP = s->playerP;
    Char *infoP = s->info;
    UInt16 capacity = sizeof(s->info);
    RectangleType bounds;
    FormType *formP = FrmGetActiveForm();
    FrmGetObjectBounds(formP, FrmGetObjectIndex(formP, HeadField), &bounds);
    s->head[0] = '\0';
    AppendTwoLines(&s->codec, s->head, sizeof(s->head),
        playerP->title[0] != '\0'
        ? playerP->title : itemP->title, bounds.extent.x);
    if (itemP->channelId[0] == '\0') {
        Append(s->head, sizeof(s->head), "\n");
        Append(s->head, sizeof(s->head), playerP->author[0] != '\0'
            ? playerP->author
            : s->channelId[0] != '\0' ? s->channelName : itemP->channel);
    }
    /* The duration is on the thumbnail. */
    infoP[0] = '\0';
    Append(infoP, capacity, itemP->views);
    if (itemP->views[0] != '\0' && itemP->published[0] != '\0')
        Append(infoP, capacity, " | ");
    Append(infoP, capacity, itemP->published);
    if (YtPlayerVideo(playerP, 0) != 0 && playerP->audio.found) {
        UInt16 choice = VideoChoice(s);
        AppendInfoLine(infoP, capacity, kQualityNames[choice]);
        Append(infoP, capacity, " video ");
        AppendSize(infoP, capacity, playerP->video[choice].contentLength);
        Append(infoP, capacity, ", audio ");
        AppendSize(infoP, capacity, playerP->audio.contentLength);
    }
}

/* The quality picker, once YouTube offers both, until a download starts */
Boolean HasDetailPicker(const AppState *s)
{
    const YtPlayer *playerP = s->playerP;
    return (s->task == taskIdle || s->task == taskThumbs) &&
        YtPlayerPlayable(playerP) &&
        playerP->video[quality144].found && playerP->video[quality240].found;
}

Boolean InfoAtTop(void)
{
    UInt16 position, textHeight, fieldHeight;
    FldGetScrollValues((FieldType *)GetObject(InfoField), &position,
        &textHeight, &fieldHeight);
    return position == 0;
}

static void ShowDetailQuality(const AppState *s)
{
    Boolean picker = HasDetailPicker(s);
    if (picker) {
        CtlSetLabel((ControlType *)GetObject(DetailQualityTrigger),
            kQualityNames[VideoChoice(s)]);
        LstSetSelection((ListType *)GetObject(DetailQualityList),
            (Int16)VideoChoice(s));
    }
    ShowObject(DetailQualityTrigger, picker);
}

static Boolean IsIdle(const AppState *s)
{
    return s->task == taskIdle || s->task == taskThumbs;
}

Boolean CanPlayDetail(const AppState *s)
{
    return IsIdle(s) && (s->saved || YtPlayerPlayable(s->playerP));
}

void RefreshDetail(AppState *s)
{
    Boolean idle = IsIdle(s);
    if (!OnForm(DetailForm)) return;
    SetFieldText(HeadField, s->head);
    SetFieldText(InfoField, s->info);
    UpdateInfoScrollBar();
    SetFieldText(ProgressField, s->progress);
    ShowObject(DownloadButton, idle && YtPlayerPlayable(s->playerP) &&
        !(s->saved && s->quality == s->savedQuality));
    ShowObject(CancelButton, s->task == taskDownload);
    CtlSetEnabled((ControlType *)GetObject(ChannelTrigger),
        s->task != taskDownload);
    ShowObject(PlayButton, CanPlayDetail(s));
    ShowDetailQuality(s);
    DrawProgressBar(s);
    ShowDetailThumb(s);
}

/* "Saved: 240p, 9.8 MB.", or a note that downloading the other quality
 * replaces the saved copy */
static void DescribeSavedCopy(AppState *s)
{
    Char *dstP = s->progress;
    UInt16 capacity = sizeof(s->progress);
    dstP[0] = '\0';
    if (!s->saved) return;
    if (s->quality == s->savedQuality) {
        StrCopy(dstP, "Saved: ");
        Append(dstP, capacity, kQualityNames[s->savedQuality]);
        Append(dstP, capacity, ", ");
        AppendSize(dstP, capacity, s->savedSize);
        Append(dstP, capacity, ".");
    } else {
        StrCopy(dstP, "The ");
        Append(dstP, capacity, kQualityNames[s->savedQuality]);
        Append(dstP, capacity, " version is saved. Downloading ");
        Append(dstP, capacity, kQualityNames[VideoChoice(s)]);
        Append(dstP, capacity, " replaces it.");
    }
}

/* The stream sizes and the saved copy's line follow the quality chosen. */
void RefreshDetailChoice(AppState *s)
{
    BuildInfo(s);
    DescribeSavedCopy(s);
    RefreshDetail(s);
}

void SwitchDetailQuality(AppState *s, UInt16 choice)
{
    s->quality = choice;
    s->prefs.quality = choice;
    s->prefsChanged = true;
    RefreshDetailChoice(s);
}

void MakeOutPath(AppState *s)
{
    const YtSearchItem *itemP = &s->searchP->items[s->selected];
    Char name[64];
    TextFileName(s->playerP->title, itemP->videoId, ".mp4", name, sizeof(name),
        &s->codec);
    StoreMakePath(s->outPath, name);
}

/* Looks for outPath on the card. When it is there, its quality is chosen. */
void FindSavedCopy(AppState *s)
{
    SavedVideo video;
    s->saved = ReadSavedVideo(s->outPath, &video) && video.size != 0;
    if (!s->saved) return;
    s->savedQuality = SavedQuality(&video);
    s->savedSize = video.size;
    s->quality = s->savedQuality;
}

/* The saved copy plays when its quality is chosen. */
void PlayDetail(AppState *s)
{
    StopThumbs(s);
    if (s->saved && s->quality == s->savedQuality) StrCopy(s->playPath, s->outPath);
    else s->playPath[0] = '\0';
    FrmPopupForm(PlayForm);
}

void StartPlayer(AppState *s)
{
    const YtSearchItem *itemP = &s->searchP->items[s->selected];
    FreeBitmap(&s->detailThumbP, &s->detailBitsP);
    YtPlayerInit(s->playerP, kVideoItags, kAudioItags, &s->codec);
    s->percent = 0;
    s->quality = s->prefs.quality;
    s->saved = false;
    BuildInfo(s);
    JsonScanInit(&s->scanner, s->jsonValue, sizeof(s->jsonValue),
        YtPlayerValue, YtPlayerEnd, s->playerP);
    s->progress[0] = '\0';
    /* Where the stream sizes will be; FinishPlayer builds the info again. */
    AppendInfoLine(s->info, sizeof(s->info), "Getting stream info...");
    if (!StartApiJob(s, taskPlayer, YtBuildPlayerRequest(&s->client,
            s->prefs.visitor, itemP->videoId, s->body, s->request,
            sizeof(s->request))))
        BuildInfo(s);
}

void FinishPlayer(AppState *s)
{
    YtPlayer *playerP = s->playerP;
    if (s->jobResult.status != netJobOk ||
        JsonScanFinish(&s->scanner) != jsonOk) {
        if (s->jobResult.status == netJobOk)
            StrCopy(s->progress, "Unexpected reply from YouTube.");
        else
            DescribeFailure(s, s->progress, sizeof(s->progress));
    } else {
        SaveVisitor(s, playerP->visitor);
        s->progress[0] = '\0';
        if (YtPlayerPlayable(playerP)) {
            MakeOutPath(s);
            FindSavedCopy(s);
            DescribeSavedCopy(s);
        } else if (StrCompare(playerP->status, "OK") != 0) {
            StrCopy(s->progress, "YouTube: ");
            Append(s->progress, sizeof(s->progress), playerP->reason[0] != '\0'
                ? playerP->reason : playerP->status);
        } else if (playerP->live) {
            StrCopy(s->progress, "Live streams are not supported.");
        } else {
            StrCopy(s->progress, "No 144p or 240p H.264 + AAC streams offered.");
        }
        BuildInfo(s);
    }
}
