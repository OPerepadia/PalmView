/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "yt_api.h"
#include "text.h"

static void CopyField(Char *dstP, UInt16 capacity, const Char *srcP)
{
    StrNCopy(dstP, srcP, capacity - 1);
    dstP[capacity - 1] = '\0';
}

void YtClientDefaults(YtClient *clientP)
{
    MemSet(clientP, sizeof(*clientP), 0);
    StrCopy(clientP->name, "VISIONOS");
    StrCopy(clientP->version, "1.02");
    StrCopy(clientP->nameId, "101");
    StrCopy(clientP->userAgent,
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 "
        "(KHTML, like Gecko) Version/26.0 Safari/605.1.15");
    StrCopy(clientP->deviceMake, "Apple");
    StrCopy(clientP->deviceModel, "RealityDevice17,1");
    StrCopy(clientP->osName, "visionOS");
    StrCopy(clientP->osVersion, "26.5.23O471");
}

static Boolean KeyIs(const Char *startP, UInt16 length, const Char *keyP)
{
    return StrLen(keyP) == length && StrNCompare(startP, keyP, length) == 0;
}

static void SetClientField(YtClient *clientP, const Char *keyP,
                           UInt16 keyLength, const Char *valueP,
                           UInt16 valueLength)
{
    Char *fieldP = 0;
    UInt16 capacity = 0;
    if (KeyIs(keyP, keyLength, "name")) {
        fieldP = clientP->name; capacity = sizeof(clientP->name);
    } else if (KeyIs(keyP, keyLength, "version")) {
        fieldP = clientP->version; capacity = sizeof(clientP->version);
    } else if (KeyIs(keyP, keyLength, "nameId")) {
        fieldP = clientP->nameId; capacity = sizeof(clientP->nameId);
    } else if (KeyIs(keyP, keyLength, "userAgent")) {
        fieldP = clientP->userAgent; capacity = sizeof(clientP->userAgent);
    } else if (KeyIs(keyP, keyLength, "deviceMake")) {
        fieldP = clientP->deviceMake; capacity = sizeof(clientP->deviceMake);
    } else if (KeyIs(keyP, keyLength, "deviceModel")) {
        fieldP = clientP->deviceModel; capacity = sizeof(clientP->deviceModel);
    } else if (KeyIs(keyP, keyLength, "osName")) {
        fieldP = clientP->osName; capacity = sizeof(clientP->osName);
    } else if (KeyIs(keyP, keyLength, "osVersion")) {
        fieldP = clientP->osVersion; capacity = sizeof(clientP->osVersion);
    }
    if (fieldP == 0) return;
    if (valueLength >= capacity) valueLength = capacity - 1;
    MemMove(fieldP, valueP, valueLength);
    fieldP[valueLength] = '\0';
}

void YtClientParse(YtClient *clientP, const Char *textP, UInt16 length)
{
    UInt16 offset = 0;
    while (offset < length) {
        UInt16 start = offset, end, equals;
        while (offset < length && textP[offset] != '\n') offset++;
        end = offset++;
        while (end > start && (textP[end - 1] == '\r' || textP[end - 1] == ' '))
            end--;
        while (start < end && textP[start] == ' ') start++;
        if (start == end || textP[start] == '#') continue;
        for (equals = start; equals < end && textP[equals] != '='; equals++) {}
        if (equals == end) continue;
        {
            UInt16 keyEnd = equals, valueStart = equals + 1;
            while (keyEnd > start && textP[keyEnd - 1] == ' ') keyEnd--;
            while (valueStart < end && textP[valueStart] == ' ') valueStart++;
            SetClientField(clientP, textP + start, keyEnd - start,
                textP + valueStart, end - valueStart);
        }
    }
}

static Boolean AppendJsonPair(Char *bodyP, UInt16 *lengthP, const Char *keyP,
                              const Char *valueP, Boolean first)
{
    return (first || TextAppend(bodyP, YT_BODY_CAPACITY, lengthP, ",")) &&
        TextAppend(bodyP, YT_BODY_CAPACITY, lengthP, "\"") &&
        TextAppend(bodyP, YT_BODY_CAPACITY, lengthP, keyP) &&
        TextAppend(bodyP, YT_BODY_CAPACITY, lengthP, "\":\"") &&
        TextAppendJson(bodyP, YT_BODY_CAPACITY, lengthP, valueP, 0) &&
        TextAppend(bodyP, YT_BODY_CAPACITY, lengthP, "\"");
}

/* Writes {"context":{"client":{...}} without the closing brace. */
static Boolean AppendContext(const YtClient *clientP, const Char *visitorP,
                             Char *bodyP, UInt16 *lengthP)
{
    if (!TextAppend(bodyP, YT_BODY_CAPACITY, lengthP,
            "{\"context\":{\"client\":{") ||
        !AppendJsonPair(bodyP, lengthP, "clientName", clientP->name, true) ||
        !AppendJsonPair(bodyP, lengthP, "clientVersion", clientP->version,
            false) ||
        !AppendJsonPair(bodyP, lengthP, "hl", "en", false))
        return false;
    if (clientP->deviceMake[0] != '\0' &&
        !AppendJsonPair(bodyP, lengthP, "deviceMake", clientP->deviceMake,
            false)) return false;
    if (clientP->deviceModel[0] != '\0' &&
        !AppendJsonPair(bodyP, lengthP, "deviceModel", clientP->deviceModel,
            false)) return false;
    if (clientP->userAgent[0] != '\0' &&
        !AppendJsonPair(bodyP, lengthP, "userAgent", clientP->userAgent,
            false)) return false;
    if (clientP->osName[0] != '\0' &&
        !AppendJsonPair(bodyP, lengthP, "osName", clientP->osName, false))
        return false;
    if (clientP->osVersion[0] != '\0' &&
        !AppendJsonPair(bodyP, lengthP, "osVersion", clientP->osVersion,
            false)) return false;
    if (visitorP != 0 && visitorP[0] != '\0' &&
        !AppendJsonPair(bodyP, lengthP, "visitorData", visitorP, false))
        return false;
    return TextAppend(bodyP, YT_BODY_CAPACITY, lengthP, "}}");
}

static UInt16 BuildPost(const YtClient *clientP, const Char *visitorP,
                        const Char *pathP, const Char *bodyP,
                        UInt16 bodyLength, Char *requestP, UInt16 capacity)
{
    UInt16 length = 0;
    Boolean ok = TextAppend(requestP, capacity, &length, "POST ") &&
        TextAppend(requestP, capacity, &length, pathP) &&
        TextAppend(requestP, capacity, &length,
            " HTTP/1.1\r\nHost: " YT_API_HOST "\r\nUser-Agent: ") &&
        TextAppend(requestP, capacity, &length, clientP->userAgent) &&
        TextAppend(requestP, capacity, &length,
            "\r\nAccept: */*\r\nAccept-Encoding: identity\r\n"
            "Content-Type: application/json\r\n"
            "Origin: https://" YT_API_HOST "\r\nX-YouTube-Client-Name: ") &&
        TextAppend(requestP, capacity, &length, clientP->nameId) &&
        TextAppend(requestP, capacity, &length,
            "\r\nX-YouTube-Client-Version: ") &&
        TextAppend(requestP, capacity, &length, clientP->version);
    if (ok && visitorP != 0 && visitorP[0] != '\0')
        ok = TextAppend(requestP, capacity, &length,
                "\r\nX-Goog-Visitor-Id: ") &&
            TextAppend(requestP, capacity, &length, visitorP);
    ok = ok && TextAppend(requestP, capacity, &length,
            "\r\nConnection: close\r\nContent-Length: ") &&
        TextAppendUInt32(requestP, capacity, &length, bodyLength) &&
        TextAppend(requestP, capacity, &length, "\r\n\r\n");
    if (!ok || (UInt32)length + bodyLength >= capacity) return 0;
    MemMove(requestP + length, bodyP, bodyLength);
    length += bodyLength;
    requestP[length] = '\0';
    return length;
}

UInt16 YtBuildPlayerRequest(const YtClient *clientP, const Char *visitorP,
                            const Char *videoIdP, Char *bodyP,
                            Char *requestP, UInt16 capacity)
{
    UInt16 bodyLength = 0;
    if (!AppendContext(clientP, visitorP, bodyP, &bodyLength) ||
        !AppendJsonPair(bodyP, &bodyLength, "videoId", videoIdP, false) ||
        !TextAppend(bodyP, YT_BODY_CAPACITY, &bodyLength,
            ",\"contentCheckOk\":true,\"racyCheckOk\":true}"))
        return 0;
    return BuildPost(clientP, visitorP,
        "/youtubei/v1/player?prettyPrint=false", bodyP, bodyLength,
        requestP, capacity);
}

UInt16 YtBuildSearchRequest(const YtClient *clientP, const Char *visitorP,
                            const Char *queryP, const TextCodec *codecP,
                            Char *bodyP, Char *requestP, UInt16 capacity)
{
    UInt16 bodyLength = 0;
    if (!AppendContext(clientP, visitorP, bodyP, &bodyLength) ||
        !TextAppend(bodyP, YT_BODY_CAPACITY, &bodyLength, ",\"query\":\"") ||
        !TextAppendJson(bodyP, YT_BODY_CAPACITY, &bodyLength, queryP,
            codecP) ||
        !TextAppend(bodyP, YT_BODY_CAPACITY, &bodyLength, "\"}"))
        return 0;
    return BuildPost(clientP, visitorP,
        "/youtubei/v1/search?prettyPrint=false", bodyP, bodyLength,
        requestP, capacity);
}

Boolean YtSplitUrl(const Char *urlP, Char *hostP, UInt16 hostCapacity,
                   const Char **pathPP)
{
    const Char *startP, *endP;
    UInt16 length;
    if (StrNCompare(urlP, "https://", 8) != 0) return false;
    startP = urlP + 8;
    for (endP = startP; *endP != '\0' && *endP != '/' && *endP != '?' &&
         *endP != ':'; endP++) {}
    length = (UInt16)(endP - startP);
    if (length == 0 || length >= hostCapacity || *endP == ':') return false;
    MemMove(hostP, startP, length);
    hostP[length] = '\0';
    *pathPP = *endP == '\0' ? "/" : endP;
    return true;
}

UInt16 YtBuildMediaRequest(const YtClient *clientP, const Char *urlP,
                           UInt32 start, UInt32 end, Boolean keepAlive,
                           Char *requestP, UInt16 capacity)
{
    Char host[128];
    const Char *pathP;
    UInt16 length = 0;
    Boolean ok;
    if (!YtSplitUrl(urlP, host, sizeof(host), &pathP)) return 0;
    ok = TextAppend(requestP, capacity, &length, "GET ") &&
        TextAppend(requestP, capacity, &length, pathP) &&
        TextAppend(requestP, capacity, &length,
            StrChr(pathP, '?') != 0 ? "&range=" : "?range=") &&
        TextAppendUInt32(requestP, capacity, &length, start) &&
        TextAppend(requestP, capacity, &length, "-") &&
        TextAppendUInt32(requestP, capacity, &length, end) &&
        TextAppend(requestP, capacity, &length, " HTTP/1.1\r\nHost: ") &&
        TextAppend(requestP, capacity, &length, host) &&
        TextAppend(requestP, capacity, &length, "\r\nUser-Agent: ") &&
        TextAppend(requestP, capacity, &length, clientP->userAgent) &&
        TextAppend(requestP, capacity, &length,
            "\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: ") &&
        TextAppend(requestP, capacity, &length,
            keepAlive ? "keep-alive\r\n\r\n" : "close\r\n\r\n");
    return ok ? length : 0;
}

UInt16 YtBuildThumbRequest(const YtClient *clientP, const Char *videoIdP,
                           Boolean keepAlive, Char *requestP, UInt16 capacity)
{
    UInt16 length = 0;
    Boolean ok = TextAppend(requestP, capacity, &length, "GET /vi/") &&
        TextAppend(requestP, capacity, &length, videoIdP) &&
        TextAppend(requestP, capacity, &length,
            "/default.jpg HTTP/1.1\r\nHost: " YT_THUMB_HOST "\r\nUser-Agent: ") &&
        TextAppend(requestP, capacity, &length, clientP->userAgent) &&
        TextAppend(requestP, capacity, &length,
            "\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: ") &&
        TextAppend(requestP, capacity, &length,
            keepAlive ? "keep-alive\r\n\r\n" : "close\r\n\r\n");
    return ok ? length : 0;
}

Boolean YtApplyRedirect(Char *urlP, UInt16 capacity, const Char *locationP)
{
    const Char *p = StrChr(locationP, '?');
    UInt16 length;
    Boolean first = true;
    if (StrNCompare(locationP, "https://", 8) != 0 ||
        StrLen(locationP) >= capacity)
        return false;
    if (p == 0) {
        StrCopy(urlP, locationP);
        return true;
    }
    length = (UInt16)(p - locationP + 1);
    MemMove(urlP, locationP, length);
    while (*p != '\0') {
        const Char *endP;
        UInt16 size;
        p++;
        for (endP = p; *endP != '\0' && *endP != '&'; endP++) {}
        size = (UInt16)(endP - p);
        if (size != 0 && StrNCompare(p, "range=", 6) != 0) {
            if (!first) urlP[length++] = '&';
            MemMove(urlP + length, p, size);
            length += size;
            first = false;
        }
        p = endP;
    }
    if (first) length--;    /* no parameters left: drop the '?' */
    urlP[length] = '\0';
    return true;
}

/* Player responses */

static Boolean ItagRank(const UInt16 *itagsP, UInt16 itag, UInt16 *rankP)
{
    UInt16 i;
    for (i = 0; i < YT_MAX_ITAG_CHOICES && itagsP[i] != 0; i++) {
        if (itagsP[i] == itag) {
            *rankP = i;
            return true;
        }
    }
    return false;
}

static void ResetCurrentFormat(YtPlayer *p)
{
    p->curItag = 0;
    p->curLength = 0;
    p->curIndexEnd = 0;
    p->curHasUrl = false;
    p->curUnusable = false;
}

void YtPlayerInit(YtPlayer *playerP, const UInt16 *videoItagsP,
                  const UInt16 *audioItagsP, const TextCodec *codecP)
{
    UInt16 i;
    MemSet(playerP, sizeof(*playerP), 0);
    playerP->codecP = codecP;
    for (i = 0; i < YT_VIDEO_CHOICES && videoItagsP[i] != 0; i++)
        playerP->videoItags[i] = videoItagsP[i];
    for (i = 0; i < YT_MAX_ITAG_CHOICES && audioItagsP[i] != 0; i++)
        playerP->audioItags[i] = audioItagsP[i];
    playerP->audioRank = YT_MAX_ITAG_CHOICES;
}

static Boolean StringIs(const Char *textP, const Char *expectedP)
{
    return StrCompare(textP, expectedP) == 0;
}

/* Inside streamingData/adaptiveFormats/#, where most of the reply is:
 * checked once per value, not once per field. */
static Boolean InFormat(const JsonScanner *s)
{
    return s->overflowDepth == 0 && s->depth > 3 && !s->frames[0].isArray &&
        StringIs(s->frames[0].key, "streamingData") && !s->frames[1].isArray &&
        StringIs(s->frames[1].key, "adaptiveFormats") && s->frames[2].isArray;
}

/* A value of the adaptive format being read */
static void FormatValue(YtPlayer *p, const JsonScanner *s, const Char *textP,
                        UInt16 length)
{
    if (JsonPathFrom(s, 3, "itag")) {
        UInt32 itag = 0;
        TextParseUInt32(textP, &itag);
        p->curItag = (UInt16)itag;
    } else if (JsonPathFrom(s, 3, "url")) {
        if (s->valueTruncated) {
            p->curUnusable = true;
        } else {
            MemMove(p->curUrl, textP, length + 1);
            p->curHasUrl = true;
        }
    } else if (JsonPathFrom(s, 3, "contentLength")) {
        TextParseUInt32(textP, &p->curLength);
    } else if (JsonPathFrom(s, 3, "indexRange/end")) {
        if (TextParseUInt32(textP, &p->curIndexEnd)) p->curIndexEnd++;
    } else if (JsonPathFrom(s, 3, "signatureCipher") ||
               JsonPathFrom(s, 3, "drmFamilies/#")) {
        p->curUnusable = true;
    } else if (JsonPathFrom(s, 3, "type")) {
        /* OTF streams are only served in numbered segments. */
        if (StringIs(textP, "FORMAT_STREAM_TYPE_OTF")) p->curUnusable = true;
    }
}

Boolean YtPlayerValue(void *contextP, const JsonScanner *s, UInt16 type,
                      const Char *textP, UInt16 length)
{
    YtPlayer *p = (YtPlayer *)contextP;
    if (InFormat(s)) {
        FormatValue(p, s, textP, length);
    } else if (JsonPathIs(s, "playabilityStatus/status")) {
        CopyField(p->status, sizeof(p->status), textP);
    } else if (JsonPathIs(s, "playabilityStatus/reason")) {
        TextUtf8ToPalm(textP, length, p->reason, sizeof(p->reason), p->codecP);
    } else if (JsonPathIs(s, "videoDetails/title")) {
        TextUtf8ToPalm(textP, length, p->title, sizeof(p->title), p->codecP);
    } else if (JsonPathIs(s, "videoDetails/author")) {
        TextUtf8ToPalm(textP, length, p->author, sizeof(p->author), p->codecP);
    } else if (JsonPathIs(s, "videoDetails/lengthSeconds")) {
        TextParseUInt32(textP, &p->lengthSeconds);
    } else if (JsonPathIs(s, "videoDetails/isLive")) {
        p->live = type == jsonTypeTrue;
    } else if (JsonPathIs(s, "responseContext/visitorData")) {
        if (!s->valueTruncated && length < sizeof(p->visitor))
            MemMove(p->visitor, textP, length + 1);
    }
    return true;
}

static void TakeFormat(YtFormat *formatP, const YtPlayer *p)
{
    formatP->found = true;
    formatP->itag = p->curItag;
    formatP->contentLength = p->curLength;
    formatP->indexEnd = p->curIndexEnd;
    StrCopy(formatP->url, p->curUrl);
}

Boolean YtPlayerEnd(void *contextP, const JsonScanner *s)
{
    YtPlayer *p = (YtPlayer *)contextP;
    UInt16 rank, i;
    if (s->depth != 3 || !JsonPathIs(s, "streamingData/adaptiveFormats/#"))
        return true;
    /* contentLength is required: downloads are made in ranges. */
    if (p->curHasUrl && !p->curUnusable && p->curLength != 0) {
        for (i = 0; i < YT_VIDEO_CHOICES; i++)
            if (p->curItag == p->videoItags[i] && !p->video[i].found)
                TakeFormat(&p->video[i], p);
        if (ItagRank(p->audioItags, p->curItag, &rank) && rank < p->audioRank) {
            p->audioRank = rank;
            TakeFormat(&p->audio, p);
        }
    }
    ResetCurrentFormat(p);
    return true;
}

const YtFormat *YtPlayerVideo(const YtPlayer *playerP, UInt16 choice)
{
    UInt16 i;
    if (choice < YT_VIDEO_CHOICES && playerP->video[choice].found)
        return &playerP->video[choice];
    for (i = 0; i < YT_VIDEO_CHOICES; i++)
        if (playerP->video[i].found) return &playerP->video[i];
    return 0;
}

Boolean YtPlayerPlayable(const YtPlayer *playerP)
{
    return StringIs(playerP->status, "OK") && !playerP->live &&
        YtPlayerVideo(playerP, 0) != 0 && playerP->audio.found;
}

/* Search responses */

void YtSearchInit(YtSearch *searchP, const TextCodec *codecP)
{
    MemSet(searchP, sizeof(*searchP), 0);
    searchP->codecP = codecP;
}

/* Level just below the innermost video renderer key, or 0 if none. */
static UInt16 RendererLevel(const JsonScanner *s)
{
    UInt16 level = s->depth;
    while (level != 0) {
        const JsonFrame *frameP = &s->frames[--level];
        if (!frameP->isArray &&
            (StringIs(frameP->key, "compactVideoRenderer") ||
             StringIs(frameP->key, "videoRenderer")))
            return (UInt16)(level + 1);
    }
    return 0;
}

static Boolean TextField(const JsonScanner *s, UInt16 level,
                         const Char *nameP, Boolean *firstRunP)
{
    if (level >= s->depth || s->frames[level].isArray ||
        !StringIs(s->frames[level].key, nameP)) return false;
    if (JsonPathFrom(s, (UInt16)(level + 1), "simpleText")) {
        *firstRunP = true;
        return true;
    }
    if (JsonPathFrom(s, (UInt16)(level + 1), "runs/#/text")) {
        *firstRunP = JsonIndexAt(s, (UInt16)(level + 2)) == 0;
        return true;
    }
    return false;
}

static void SetText(const TextCodec *codecP, Char *dstP, UInt16 capacity,
                    const Char *textP, UInt16 length, Boolean replace)
{
    UInt16 used = replace ? 0 : StrLen(dstP);
    if (used + 1 >= capacity) return;
    TextUtf8ToPalm(textP, length, dstP + used, (UInt16)(capacity - used),
        codecP);
}

Boolean YtSearchValue(void *contextP, const JsonScanner *s, UInt16 type,
                      const Char *textP, UInt16 length)
{
    YtSearch *searchP = (YtSearch *)contextP;
    YtSearchItem *itemP = &searchP->cur;
    UInt16 level;
    Boolean first;
    (void)type;
    if (JsonPathIs(s, "responseContext/visitorData")) {
        if (!s->valueTruncated && length < sizeof(searchP->visitor))
            MemMove(searchP->visitor, textP, length + 1);
        return true;
    }
    level = RendererLevel(s);
    if (level == 0) return true;
    if (JsonPathFrom(s, level, "videoId")) {
        CopyField(itemP->videoId, sizeof(itemP->videoId), textP);
    } else if (TextField(s, level, "title", &first)) {
        /* Titles can be split into several runs; join them. */
        SetText(searchP->codecP, itemP->title, sizeof(itemP->title), textP, length, first);
    } else if (TextField(s, level, "longBylineText", &first)) {
        if (first) SetText(searchP->codecP, itemP->channel, sizeof(itemP->channel), textP,
            length, true);
    } else if (TextField(s, level, "lengthText", &first)) {
        if (first) SetText(searchP->codecP, itemP->length, sizeof(itemP->length), textP,
            length, true);
    } else if (TextField(s, level, "publishedTimeText", &first)) {
        if (first) SetText(searchP->codecP, itemP->published, sizeof(itemP->published),
            textP, length, true);
    } else if (TextField(s, level, "shortViewCountText", &first)) {
        if (first) SetText(searchP->codecP, itemP->views, sizeof(itemP->views), textP, length,
            true);
    } else if (JsonPathFrom(s, level, "badges/#/metadataBadgeRenderer/label") ||
               JsonPathFrom(s, level,
                   "thumbnailOverlays/#/thumbnailOverlayTimeStatusRenderer/style")) {
        if (StringIs(textP, "LIVE")) itemP->live = true;
    }
    return true;
}

Boolean YtSearchEnd(void *contextP, const JsonScanner *s)
{
    YtSearch *searchP = (YtSearch *)contextP;
    if (s->depth == 0) return true;
    if (s->frames[s->depth - 1].isArray ||
        (!StringIs(s->frames[s->depth - 1].key, "compactVideoRenderer") &&
         !StringIs(s->frames[s->depth - 1].key, "videoRenderer")))
        return true;
    if (searchP->cur.videoId[0] != '\0' && searchP->count < YT_SEARCH_MAX) {
        if (searchP->cur.length[0] == '\0') searchP->cur.live = true;
        searchP->items[searchP->count++] = searchP->cur;
    }
    MemSet(&searchP->cur, sizeof(searchP->cur), 0);
    return true;
}
