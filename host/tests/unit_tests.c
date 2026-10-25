/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <PalmOS.h>
#include <glob.h>
#include <stdlib.h>
#include "text.h"
#include "thumb.h"
#include "yt_api.h"

static int gFailures;
static const TextCodec kWestern = { textWestern, 0, 0 };

static void ExpectText(const char *nameP, const char *actualP,
                       const char *expectedP)
{
    if (strcmp(actualP, expectedP) == 0) return;
    printf("FAIL %s:\n  got      [%s]\n  expected [%s]\n", nameP, actualP,
        expectedP);
    gFailures++;
}

static void Expect(const char *nameP, int condition)
{
    if (condition) return;
    printf("FAIL %s\n", nameP);
    gFailures++;
}

static void TestUtf8ToPalm(void)
{
    Char out[64];
    const char *srcP = "Caf\xC3\xA9 \xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x82\xAC" "5";
    TextUtf8ToPalm(srcP, (UInt16)strlen(srcP), out, sizeof(out), 0);
    ExpectText("latin and cp1252 punctuation", out,
        "Caf\xE9 \x93quoted\x94 \x80" "5");

    srcP = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD1\x96\xD1\x82 "
        "\xD0\xA9\xD0\xB5 \xD0\x87\xD0\xB6\xD0\xB0";
    TextUtf8ToPalm(srcP, (UInt16)strlen(srcP), out, sizeof(out), 0);
    ExpectText("cyrillic transliteration", out, "Pryvit Shche Yizha");

    srcP = "Hi \xF0\x9F\x98\x80\xEF\xB8\x8F there \xE4\xB8\xAD\xE6\x96\x87!";
    TextUtf8ToPalm(srcP, (UInt16)strlen(srcP), out, sizeof(out), 0);
    ExpectText("emoji dropped, unknown collapsed", out, "Hi  there ?!");

    srcP = "\xFF\xC3";
    TextUtf8ToPalm(srcP, (UInt16)strlen(srcP), out, sizeof(out), 0);
    ExpectText("invalid UTF-8", out, "?");

    TextUtf8ToPalm("abcdefgh", 8, out, 5, 0);
    ExpectText("truncation", out, "abcd");
}

static void TestJsonEscape(void)
{
    Char out[64];
    UInt16 length = 0;
    out[0] = '\0';
    Expect("json escape fits",
        TextAppendJson(out, sizeof(out), &length, "a\"b\\c\n\xE9\x80", &kWestern));
    ExpectText("json escape", out, "a\\\"b\\\\c\\u000a\xC3\xA9\xE2\x82\xAC");
    length = 0;
    out[0] = '\0';
    Expect("json escape overflow",
        !TextAppendJson(out, 4, &length, "abcdef", 0));
}

/* Japanese with CJKOS, using the table in build/host/jis.bin */
static void TestJapanese(void)
{
    static UInt8 table[48 * 1024];
    TextCodec sjis;
    Char out[64], json[64];
    UInt16 length;
    UInt32 i, mismatches = 0;
    FILE *fileP = fopen("build/host/jis.bin", "rb");
    size_t size;
    if (fileP == 0) {
        Expect("jis.bin missing", 0);
        return;
    }
    size = fread(table, 1, sizeof(table), fileP);
    fclose(fileP);
    sjis.encoding = textShiftJis;
    sjis.jisP = table;
    sjis.jisCount = (UInt16)(size / 4);

    TextUtf8ToPalm("Everybody\xE2\x80\x99s caf\xC3\xA9", 19, out, sizeof(out),
        &sjis);
    ExpectText("japanese punctuation and accents", out, "Everybody's cafe");
    TextUtf8ToPalm("\xE9\xAC\xBC\xE9\xA6\x96 \xEF\xBD\xB1", 10, out, sizeof(out),
        &sjis);
    ExpectText("shift-jis", out, "\x8B\x53\x8E\xF1 \xB1");
    TextUtf8ToPalm("\xD0\x9F\xD1\x80 \xEA\xB0\x80", 8, out, sizeof(out),
        &sjis);
    ExpectText("japanese keeps transliteration", out, "Pr ?");
    TextUtf8ToPalm("ab\xE9\xAC\xBC", 5, out, 4, &sjis);
    ExpectText("no half character at the end", out, "ab");

    length = 0;
    json[0] = '\0';
    TextAppendJson(json, sizeof(json), &length, "\x95\x5C\\", &sjis);
    ExpectText("shift-jis trail byte is not a backslash", json,
        "\xE8\xA1\xA8\\\\");
    TextFileName("\x8B\x53" "Song", "id", ".mp4", out, sizeof(out), &sjis);
    ExpectText("japanese file name", out, "Song.mp4");
    Expect("char start", TextCharStart(&sjis, "a\x95\x5C" "b", 2) == 1 &&
        TextCharStart(&sjis, "a\x95\x5C" "b", 3) == 3 &&
        TextCharStart(0, "a\x95\x5C" "b", 2) == 2);

    /* Every code survives the trip back to UTF-8, as searches take it. A
     * few characters share a code, so the character itself may change. */
    for (i = 0; i < sjis.jisCount; i++) {
        UInt16 cp = (UInt16)((table[i * 4] << 8) | table[i * 4 + 1]);
        Char utf8[8];
        utf8[0] = (Char)(cp < 0x800 ? 0xC0 | (cp >> 6) : 0xE0 | (cp >> 12));
        utf8[1] = (Char)(cp < 0x800 ? 0x80 | (cp & 0x3F)
            : 0x80 | ((cp >> 6) & 0x3F));
        utf8[2] = (Char)(cp < 0x800 ? '\0' : 0x80 | (cp & 0x3F));
        utf8[3] = '\0';
        TextUtf8ToPalm(utf8, cp < 0x800 ? 2 : 3, out, sizeof(out), &sjis);
        if (out[0] == '?' || (UInt8)out[0] < 0x80) continue;
        length = 0;
        json[0] = '\0';
        TextAppendJson(json, sizeof(json), &length, out, &sjis);
        TextUtf8ToPalm(json, length, utf8, sizeof(utf8), &sjis);
        /* Unless it is punctuation that deliberately becomes ASCII */
        if (strcmp(utf8, out) != 0 && (UInt8)utf8[0] >= 0x80) mismatches++;
    }
    Expect("shift-jis round trip", mismatches == 0);
}

static void TestFileName(void)
{
    Char out[40];
    TextFileName("Rick Astley - Never Gonna Give You Up (Official Video)",
        "dQw4w9WgXcQ", ".mp4", out, sizeof(out), 0);
    ExpectText("file name truncated to capacity", out,
        "Rick Astley - Never Gonna Give You.mp4");
    TextFileName("a/b:c*?\"<>|  d.", "id", ".mp4", out, sizeof(out), 0);
    ExpectText("file name sanitized", out, "a_b_c_d.mp4");
    TextFileName("???", "VIDEOID", ".mp4", out, sizeof(out), 0);
    ExpectText("file name fallback", out, "VIDEOID.mp4");
}

static void TestShorten(void)
{
    static const char *const kCases[][2] = {
        { "1.8B views", "1.8B" },
        { "1 view", "1" },
        { "No views", "No views" },
        { "1.2K watching", "1.2K watching" },
        { "10 years ago", "10y ago" },
        { "1 year ago", "1y ago" },
        { "2 months ago", "2mo ago" },
        { "3 weeks ago", "3w ago" },
        { "1 day ago", "1d ago" },
        { "4 hours ago", "4h ago" },
        { "30 minutes ago", "30min ago" },
        { "5 seconds ago", "5s ago" },
        { "Streamed 2 years ago", "2y ago" },
        { "Premiered 1 day ago", "Premiered 1 day ago" },
        { "2 decades ago", "2 decades ago" },
        { "10 yearsago", "10 yearsago" },
        { "", "" }
    };
    Char out[20];
    UInt16 i;
    for (i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        TextShorten(kCases[i][0], out, sizeof(out));
        ExpectText(kCases[i][0], out, kCases[i][1]);
    }
    TextShorten("10 years ago", out, 4);
    ExpectText("shorten truncated", out, "10y");
}

static void TestClientParse(void)
{
    YtClient client;
    const char *textP = "# comment\r\nname = TVHTML5\nversion=7.1\n"
        "nameId=7\nunknown=1\nuserAgent=Test Agent/1.0  \n";
    YtClientDefaults(&client);
    YtClientParse(&client, textP, (UInt16)strlen(textP));
    ExpectText("client name", client.name, "TVHTML5");
    ExpectText("client version", client.version, "7.1");
    ExpectText("client name id", client.nameId, "7");
    ExpectText("client user agent", client.userAgent, "Test Agent/1.0");
    ExpectText("client device kept", client.deviceMake, "Apple");
}

static void TestUrls(void)
{
    YtClient client;
    Char host[64], request[YT_REQUEST_CAPACITY];
    const Char *pathP;
    UInt16 length;
    Expect("split url",
        YtSplitUrl("https://rr1---sn-abc.googlevideo.com/videoplayback?a=1",
            host, sizeof(host), &pathP) &&
        strcmp(host, "rr1---sn-abc.googlevideo.com") == 0 &&
        strcmp(pathP, "/videoplayback?a=1") == 0);
    Expect("reject http", !YtSplitUrl("http://example.com/", host,
        sizeof(host), &pathP));
    Expect("reject port", !YtSplitUrl("https://example.com:8443/", host,
        sizeof(host), &pathP));
    YtClientDefaults(&client);
    length = YtBuildMediaRequest(&client,
        "https://rr1---sn-abc.googlevideo.com/videoplayback?a=1", 100, 4194403,
        false, request, sizeof(request));
    Expect("media request built", length != 0 &&
        strncmp(request, "GET /videoplayback?a=1&range=100-4194403 HTTP/1.1\r\n"
            "Host: rr1---sn-abc.googlevideo.com\r\n", 85) == 0 &&
        strstr(request, "Connection: close\r\n\r\n") ==
            request + length - strlen("Connection: close\r\n\r\n"));
    length = YtBuildMediaRequest(&client,
        "https://rr1---sn-abc.googlevideo.com/videoplayback?a=1", 0, 99,
        true, request, sizeof(request));
    Expect("keep-alive request built", length != 0 &&
        strstr(request, "Connection: keep-alive\r\n\r\n") ==
            request + length - strlen("Connection: keep-alive\r\n\r\n"));
}

static void TestRedirects(void)
{
    Char url[96];
    strcpy(url, "https://old.example/x?a=1");
    Expect("redirect applied", YtApplyRedirect(url, sizeof(url),
        "https://rr3---sn-x.googlevideo.com/videoplayback?expire=1&range=0-4194303&id=ab"));
    ExpectText("redirect drops range", url,
        "https://rr3---sn-x.googlevideo.com/videoplayback?expire=1&id=ab");
    YtApplyRedirect(url, sizeof(url), "https://a.example/v?range=5-9&b=2");
    ExpectText("redirect drops leading range", url, "https://a.example/v?b=2");
    YtApplyRedirect(url, sizeof(url), "https://a.example/v?b=2&range=5-9");
    ExpectText("redirect drops trailing range", url, "https://a.example/v?b=2");
    YtApplyRedirect(url, sizeof(url), "https://a.example/v?range=5-9");
    ExpectText("redirect without parameters", url, "https://a.example/v");
    YtApplyRedirect(url, sizeof(url), "https://a.example/v?arange=1&rangex=2");
    ExpectText("redirect keeps similar names", url,
        "https://a.example/v?arange=1&rangex=2");
    Expect("redirect to http refused", !YtApplyRedirect(url, sizeof(url),
        "http://a.example/v"));
    Expect("redirect too long refused", !YtApplyRedirect(url, 16,
        "https://a.example/v"));
    ExpectText("refused redirect leaves the url", url,
        "https://a.example/v?arange=1&rangex=2");
}

static void TestApiRequests(void)
{
    YtClient client;
    Char body[YT_BODY_CAPACITY], request[YT_REQUEST_CAPACITY];
    Char visitor[YT_VISITOR_CAPACITY];
    const char *bodyP;
    UInt16 length;
    YtClientDefaults(&client);
    length = YtBuildSearchRequest(&client, "", "caf\xE9 \"x\"", &kWestern, body, request,
        sizeof(request));
    bodyP = strstr(request, "\r\n\r\n");
    Expect("search request built", length != 0 && bodyP != 0);
    if (bodyP != 0) {
        char expectedLength[48];
        snprintf(expectedLength, sizeof(expectedLength), "Content-Length: %zu",
            strlen(bodyP + 4));
        Expect("search content length", strstr(request, expectedLength) != 0);
        Expect("search query escaped",
            strstr(bodyP, "\"query\":\"caf\xC3\xA9 \\\"x\\\"\"}") != 0);
        Expect("no visitor header", strstr(request, "X-Goog-Visitor-Id") == 0);
    }
    memset(visitor, 'V', 450);
    visitor[450] = '\0';
    length = YtBuildPlayerRequest(&client, visitor, "dQw4w9WgXcQ", body,
        request, sizeof(request));
    Expect("player request with long visitor fits", length != 0 &&
        strstr(request, "X-Goog-Visitor-Id: VVVV") != 0 &&
        strstr(request, "\"videoId\":\"dQw4w9WgXcQ\"") != 0);
    Expect("player request too small buffer", YtBuildPlayerRequest(&client,
        visitor, "dQw4w9WgXcQ", body, request, 600) == 0);
    length = YtBuildThumbRequest(&client, "dQw4w9WgXcQ", true, request,
        sizeof(request));
    Expect("thumbnail request", length != 0 && strncmp(request,
        "GET /vi/dQw4w9WgXcQ/default.jpg HTTP/1.1\r\nHost: i.ytimg.com\r\n",
        58) == 0 && strstr(request, "Connection: keep-alive\r\n\r\n") != 0);
}

static long ReadSample(const char *pathP, UInt8 *dataP, long capacity)
{
    FILE *fileP = fopen(pathP, "rb");
    long length;
    if (fileP == 0) return -1;
    length = (long)fread(dataP, 1, (size_t)capacity, fileP);
    fclose(fileP);
    return length;
}

/* Each quality's itag gets its own slot, with where its index ends. */
static void TestPlayerFormats(void)
{
    static const UInt16 kVideo[] = { 160, 133, 0 }, kAudio[] = { 140, 139, 0 };
    static YtPlayer player;
    static Char value[YT_JSON_VALUE_CAPACITY];
    JsonScanner scanner;
    long length, done;
    UInt8 *dataP = (UInt8 *)malloc(1 << 20);
    length = dataP != 0 ? ReadSample("testdata/player-visionos.json", dataP, 1 << 20) : -1;
    Expect("player reply readable", length > 0);
    if (length <= 0) {
        free(dataP);
        return;
    }
    YtPlayerInit(&player, kVideo, kAudio, &kWestern);
    JsonScanInit(&scanner, value, sizeof(value), YtPlayerValue, YtPlayerEnd, &player);
    for (done = 0; done < length; done += 4096)
        JsonScanFeed(&scanner, dataP + done,
            (UInt16)(length - done < 4096 ? length - done : 4096));
    free(dataP);
    Expect("player reply parsed", JsonScanFinish(&scanner) == jsonOk);
    Expect("144p found", player.video[0].found && player.video[0].itag == 160 &&
        player.video[0].contentLength == 2058142UL && player.video[0].indexEnd == 1227);
    Expect("240p found", player.video[1].found && player.video[1].itag == 133 &&
        player.video[1].contentLength == 4310122UL && player.video[1].indexEnd == 1228);
    Expect("preferred audio", player.audio.found && player.audio.itag == 140 &&
        player.audio.indexEnd == 1019);
    Expect("choice offered", YtPlayerVideo(&player, 1) == &player.video[1]);
    player.video[1].found = false;
    Expect("missing choice falls back", YtPlayerVideo(&player, 1) == &player.video[0]);
}

/* Sum of channel differences between RGB565 pixels and 8-bit RGB */
static long Difference(const UInt16 *pixelsP, const UInt8 *rgbP, long count)
{
    long error = 0, j;
    for (j = 0; j < count; j++) {
        UInt16 p = pixelsP[j];
        error += labs((long)((p >> 8) & 0xF8) - (rgbP[3 * j] & 0xF8));
        error += labs((long)((p >> 3) & 0xFC) - (rgbP[3 * j + 1] & 0xFC));
        error += labs((long)((p << 3) & 0xF8) - (rgbP[3 * j + 2] & 0xF8));
    }
    return error;
}

/* Thumbnails in testdata/thumbs/, each with Pillow's pixels for the same
 * rows in a .rgb file next to it. The decoders round and upsample color
 * differently, so only the average difference is checked: about 5 for
 * saturated pictures, 25 or more if rows are off by one. */
static void TestThumbnails(void)
{
    static UInt8 jpeg[THUMB_JPEG_CAPACITY];
    static UInt8 reference[THUMB_WIDTH * THUMB_HEIGHT * 3];
    static UInt8 small[THUMB_SMALL_WIDTH * THUMB_SMALL_HEIGHT * 3];
    static UInt16 pixels[THUMB_WIDTH * THUMB_HEIGHT];
    static UInt8 work[THUMB_WORK_SIZE];
    const long fullCount = THUMB_WIDTH * THUMB_HEIGHT;
    const long smallCount = THUMB_SMALL_WIDTH * THUMB_SMALL_HEIGHT;
    glob_t found;
    size_t i;
    memset(jpeg, 0x5A, sizeof(jpeg));
    Expect("not a JPEG", !ThumbDecode(jpeg, sizeof(jpeg), pixels, work, false));
    if (glob("testdata/thumbs/*.jpg", 0, 0, &found) != 0) return;
    for (i = 0; i < found.gl_pathc; i++) {
        const char *pathP = found.gl_pathv[i];
        char referencePath[256];
        long length = ReadSample(pathP, jpeg, sizeof(jpeg));
        int x, y, c;
        snprintf(referencePath, sizeof(referencePath), "%.*s.rgb",
            (int)(strlen(pathP) - 4), pathP);
        if (length <= 0 || ReadSample(referencePath, reference,
                sizeof(reference)) != (long)sizeof(reference)) {
            Expect(pathP, 0);
            continue;
        }
        Expect("thumbnail truncated", !ThumbDecode(jpeg, (UInt16)(length / 2),
            pixels, work, false));
        Expect(pathP, ThumbDecode(jpeg, (UInt16)length, pixels, work, false) &&
            Difference(pixels, reference, fullCount) <= 8 * fullCount * 3);
        /* Small row y averages full rows 2y + 1 and 2y + 2. */
        for (y = 0; y < THUMB_SMALL_HEIGHT; y++)
            for (x = 0; x < THUMB_SMALL_WIDTH; x++)
                for (c = 0; c < 3; c++) {
                    const UInt8 *p = reference +
                        ((2 * y + 1) * THUMB_WIDTH + 2 * x) * 3 + c;
                    small[(y * THUMB_SMALL_WIDTH + x) * 3 + c] = (UInt8)
                        ((p[0] + p[3] + p[THUMB_WIDTH * 3] +
                          p[THUMB_WIDTH * 3 + 3] + 2) / 4);
                }
        /* A row off gives 7 or more here. */
        Expect("small thumbnail", ThumbDecode(jpeg, (UInt16)length, pixels,
            work, true) && Difference(pixels, small, smallCount) <= 6 * smallCount * 3);
    }
    globfree(&found);
}

int main(void)
{
    TestUtf8ToPalm();
    TestJsonEscape();
    TestFileName();
    TestJapanese();
    TestShorten();
    TestClientParse();
    TestUrls();
    TestRedirects();
    TestApiRequests();
    TestPlayerFormats();
    TestThumbnails();
    if (gFailures != 0) {
        printf("%d unit test(s) failed\n", gFailures);
        return 1;
    }
    printf("unit tests ok\n");
    return 0;
}
