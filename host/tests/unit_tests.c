/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <PalmOS.h>
#include "text.h"
#include "yt_api.h"

static int gFailures;

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
    TextUtf8ToPalm(srcP, (UInt16)strlen(srcP), out, sizeof(out));
    ExpectText("latin and cp1252 punctuation", out,
        "Caf\xE9 \x93quoted\x94 \x80" "5");

    srcP = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD1\x96\xD1\x82 "
        "\xD0\xA9\xD0\xB5 \xD0\x87\xD0\xB6\xD0\xB0";
    TextUtf8ToPalm(srcP, (UInt16)strlen(srcP), out, sizeof(out));
    ExpectText("cyrillic transliteration", out, "Pryvit Shche Yizha");

    srcP = "Hi \xF0\x9F\x98\x80\xEF\xB8\x8F there \xE4\xB8\xAD\xE6\x96\x87!";
    TextUtf8ToPalm(srcP, (UInt16)strlen(srcP), out, sizeof(out));
    ExpectText("emoji dropped, unknown collapsed", out, "Hi  there ?!");

    srcP = "\xFF\xC3";
    TextUtf8ToPalm(srcP, (UInt16)strlen(srcP), out, sizeof(out));
    ExpectText("invalid UTF-8", out, "?");

    TextUtf8ToPalm("abcdefgh", 8, out, 5);
    ExpectText("truncation", out, "abcd");
}

static void TestJsonEscape(void)
{
    Char out[64];
    UInt16 length = 0;
    out[0] = '\0';
    Expect("json escape fits",
        TextAppendJson(out, sizeof(out), &length, "a\"b\\c\n\xE9\x80", true));
    ExpectText("json escape", out, "a\\\"b\\\\c\\u000a\xC3\xA9\xE2\x82\xAC");
    length = 0;
    out[0] = '\0';
    Expect("json escape overflow",
        !TextAppendJson(out, 4, &length, "abcdef", false));
}

static void TestFileName(void)
{
    Char out[40];
    TextFileName("Rick Astley - Never Gonna Give You Up (Official Video)",
        "dQw4w9WgXcQ", ".mp4", out, sizeof(out));
    ExpectText("file name truncated to capacity", out,
        "Rick Astley - Never Gonna Give You.mp4");
    TextFileName("a/b:c*?\"<>|  d.", "id", ".mp4", out, sizeof(out));
    ExpectText("file name sanitized", out, "a_b_c_d.mp4");
    TextFileName("???", "VIDEOID", ".mp4", out, sizeof(out));
    ExpectText("file name fallback", out, "VIDEOID.mp4");
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
    length = YtBuildSearchRequest(&client, "", "caf\xE9 \"x\"", body, request,
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
}

int main(void)
{
    TestUtf8ToPalm();
    TestJsonEscape();
    TestFileName();
    TestClientParse();
    TestUrls();
    TestRedirects();
    TestApiRequests();
    if (gFailures != 0) {
        printf("%d unit test(s) failed\n", gFailures);
        return 1;
    }
    printf("unit tests ok\n");
    return 0;
}
