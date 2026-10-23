/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Host test client. It sends the same request bytes as the Palm app and
 * parses the replies with the same code; only the TLS transport differs.
 *
 *   pview search QUERY
 *   pview info VIDEO_ID
 *   pview get VIDEO_ID OUT_DIR       download video and audio, then merge
 *   pview merge VIDEO AUDIO OUT      merge two downloaded streams
 *
 * The visitor ID YouTube hands out is kept in $PVIEW_VISITOR
 * (default testdata/visitor.txt). */

#include <PalmOS.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http.h"
#include "json_scan.h"
#include "mp4_merge.h"
#include "text.h"
#include "yt_api.h"

#define CHUNK_BYTES (4UL * 1024UL * 1024UL)

static const UInt16 kVideoItags[] = { 160, 0 };
static const UInt16 kAudioItags[] = { 140, 139, 0 };

static YtClient gClient;
static Char gVisitor[YT_VISITOR_CAPACITY];

static const char *VisitorPath(void)
{
    const char *pathP = getenv("PVIEW_VISITOR");
    return pathP != 0 ? pathP : "testdata/visitor.txt";
}

static void LoadVisitor(void)
{
    FILE *fileP = fopen(VisitorPath(), "r");
    if (fileP == 0) return;
    if (fgets(gVisitor, sizeof(gVisitor), fileP) != 0)
        gVisitor[strcspn(gVisitor, "\r\n")] = '\0';
    fclose(fileP);
}

static void SaveVisitor(const Char *visitorP)
{
    FILE *fileP;
    if (visitorP[0] == '\0' || strcmp(visitorP, gVisitor) == 0) return;
    StrCopy(gVisitor, visitorP);
    if ((fileP = fopen(VisitorPath(), "w")) == 0) return;
    fprintf(fileP, "%s\n", gVisitor);
    fclose(fileP);
}

typedef struct Exchange {
    HttpParser parser;
    UInt16 status;
} Exchange;

/* Sends a request over TLS and feeds the reply to the HTTP parser, which
 * passes the body to bodyProcP. Returns the HTTP status or 0 on failure. */
static UInt16 HttpsExchange(const char *hostP, const Char *requestP,
                            UInt16 length, HttpBodyProc bodyProcP,
                            void *contextP)
{
    static SSL_CTX *ctxP;
    struct addrinfo hints, *addressP = 0;
    Exchange exchange;
    SSL *sslP = 0;
    int fd = -1;
    UInt8 buffer[2048];
    UInt16 status = 0;

    if (ctxP == 0) {
        ctxP = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_default_verify_paths(ctxP);
        SSL_CTX_set_verify(ctxP, SSL_VERIFY_PEER, 0);
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostP, "443", &hints, &addressP) != 0) {
        fprintf(stderr, "cannot resolve %s\n", hostP);
        return 0;
    }
    fd = socket(addressP->ai_family, addressP->ai_socktype, 0);
    if (fd < 0 || connect(fd, addressP->ai_addr, addressP->ai_addrlen) != 0) {
        fprintf(stderr, "cannot connect to %s\n", hostP);
        goto done;
    }
    sslP = SSL_new(ctxP);
    SSL_set_fd(sslP, fd);
    SSL_set_tlsext_host_name(sslP, hostP);
    SSL_set1_host(sslP, hostP);
    if (SSL_connect(sslP) != 1 || SSL_write(sslP, requestP, length) != length) {
        ERR_print_errors_fp(stderr);
        goto done;
    }
    HttpParserInit(&exchange.parser, 0, bodyProcP, contextP);
    while (!exchange.parser.connectionComplete) {
        int received = SSL_read(sslP, buffer, sizeof(buffer));
        int offset = 0;
        if (received <= 0) {
            if (HttpParserFinish(&exchange.parser) != httpOk) goto done;
            break;
        }
        /* Like the Palm client: headers byte by byte, then the body. */
        while (offset < received && !exchange.parser.headersComplete)
            if (HttpParserFeed(&exchange.parser, buffer + offset++, 1) != httpOk)
                goto done;
        if (offset < received &&
            HttpParserFeed(&exchange.parser, buffer + offset,
                (UInt16)(received - offset)) != httpOk) {
            fprintf(stderr, "HTTP parse status %u\n",
                exchange.parser.parseStatus);
            goto done;
        }
    }
    status = exchange.parser.statusCode;
done:
    if (sslP != 0) SSL_free(sslP);
    if (fd >= 0) close(fd);
    freeaddrinfo(addressP);
    return status;
}

typedef struct JsonSink {
    JsonScanner scanner;
    Char value[YT_JSON_VALUE_CAPACITY];
    UInt32 bytes;
} JsonSink;

static Int16 FeedJson(void *contextP, const UInt8 *dataP, UInt16 length)
{
    JsonSink *sinkP = (JsonSink *)contextP;
    sinkP->bytes += length;
    return JsonScanFeed(&sinkP->scanner, dataP, length) == jsonOk ? 0 : 1;
}

static Boolean ApiCall(const Char *requestP, UInt16 length,
                       JsonValueProc valueProcP, JsonEndProc endProcP,
                       void *contextP)
{
    static JsonSink sink;
    UInt16 status;
    if (length == 0) {
        fprintf(stderr, "request does not fit\n");
        return false;
    }
    JsonScanInit(&sink.scanner, sink.value, sizeof(sink.value), valueProcP,
        endProcP, contextP);
    sink.bytes = 0;
    status = HttpsExchange(YT_API_HOST, requestP, length, FeedJson, &sink);
    if (status != 200 || JsonScanFinish(&sink.scanner) != jsonOk) {
        fprintf(stderr, "API call failed: HTTP %u, JSON status %u\n", status,
            sink.scanner.status);
        return false;
    }
    fprintf(stderr, "[%u bytes of JSON]\n", sink.bytes);
    return true;
}

static int Search(const char *queryP)
{
    static Char request[YT_REQUEST_CAPACITY], body[YT_BODY_CAPACITY];
    static YtSearch search;
    UInt16 i;
    YtSearchInit(&search);
    if (!ApiCall(request, YtBuildSearchRequest(&gClient, gVisitor, queryP,
            body, request, sizeof(request)), YtSearchValue, YtSearchEnd,
            &search)) return 1;
    SaveVisitor(search.visitor);
    for (i = 0; i < search.count; i++) {
        const YtSearchItem *itemP = &search.items[i];
        printf("%s  %-8s %s\n             %s | %s | %s%s\n", itemP->videoId,
            itemP->length, itemP->title, itemP->channel, itemP->published,
            itemP->views, itemP->live ? " | LIVE" : "");
    }
    return 0;
}

static Boolean LoadPlayer(const char *videoIdP, YtPlayer *playerP)
{
    static Char request[YT_REQUEST_CAPACITY], body[YT_BODY_CAPACITY];
    YtPlayerInit(playerP, kVideoItags, kAudioItags);
    if (!ApiCall(request, YtBuildPlayerRequest(&gClient, gVisitor, videoIdP,
            body, request, sizeof(request)), YtPlayerValue, YtPlayerEnd,
            playerP)) return false;
    SaveVisitor(playerP->visitor);
    printf("status: %s %s\ntitle: %s\nauthor: %s\nlength: %lu s%s\n",
        playerP->status, playerP->reason, playerP->title, playerP->author,
        (unsigned long)playerP->lengthSeconds, playerP->live ? " (live)" : "");
    if (playerP->video.found)
        printf("video: itag %u, %lu bytes\n", playerP->video.itag,
            (unsigned long)playerP->video.contentLength);
    if (playerP->audio.found)
        printf("audio: itag %u, %lu bytes\n", playerP->audio.itag,
            (unsigned long)playerP->audio.contentLength);
    return true;
}

static int Info(const char *videoIdP)
{
    static YtPlayer player;
    if (!LoadPlayer(videoIdP, &player)) return 1;
    return YtPlayerPlayable(&player) ? 0 : 3;
}

static Int16 WriteFile(void *contextP, const UInt8 *dataP, UInt16 length)
{
    return fwrite(dataP, 1, length, (FILE *)contextP) == length ? 0 : 1;
}

static Boolean Download(const YtFormat *formatP, const char *pathP)
{
    static Char request[YT_REQUEST_CAPACITY];
    Char host[128];
    const Char *pathPartP;
    FILE *fileP;
    UInt32 offset = 0;
    if (!YtSplitUrl(formatP->url, host, sizeof(host), &pathPartP) ||
        (fileP = fopen(pathP, "wb")) == 0) return false;
    while (offset < formatP->contentLength) {
        UInt32 end = offset + CHUNK_BYTES - 1;
        UInt16 length, status;
        if (end >= formatP->contentLength) end = formatP->contentLength - 1;
        length = YtBuildMediaRequest(&gClient, formatP->url, offset, end,
            false, request, sizeof(request));
        status = HttpsExchange(host, request, length, WriteFile, fileP);
        if (status != 200 && status != 206) {
            fprintf(stderr, "download failed: HTTP %u at %lu\n", status,
                (unsigned long)offset);
            fclose(fileP);
            return false;
        }
        offset = (UInt32)ftell(fileP);
        fprintf(stderr, "\r%s: %lu / %lu", pathP, (unsigned long)offset,
            (unsigned long)formatP->contentLength);
    }
    fprintf(stderr, "\n");
    fclose(fileP);
    return true;
}

/* Host implementation of the merge I/O interface. */
static Err HostRead(void *fileP, UInt32 offset, void *bufferP, UInt32 length)
{
    if (fseek((FILE *)fileP, (long)offset, SEEK_SET) != 0) return 1;
    return fread(bufferP, 1, length, (FILE *)fileP) == length ? errNone : 1;
}

static Err HostWrite(void *fileP, const void *bufferP, UInt32 length)
{
    return fwrite(bufferP, 1, length, (FILE *)fileP) == length ? errNone : 1;
}

static Boolean HostProgress(void *contextP, UInt32 done, UInt32 total)
{
    (void)contextP;
    fprintf(stderr, "\rmerging %lu / %lu", (unsigned long)done,
        (unsigned long)total);
    return true;
}

static int Merge(const char *videoPathP, const char *audioPathP,
                 const char *outPathP)
{
    FILE *videoP = fopen(videoPathP, "rb");
    FILE *audioP = fopen(audioPathP, "rb");
    FILE *outP = fopen(outPathP, "wb");
    Mp4Io io;
    Mp4MergeResult result;
    if (videoP == 0 || audioP == 0 || outP == 0) {
        fprintf(stderr, "cannot open files\n");
        return 1;
    }
    fseek(videoP, 0, SEEK_END);
    fseek(audioP, 0, SEEK_END);
    io.readProcP = HostRead;
    io.writeProcP = HostWrite;
    io.progressProcP = HostProgress;
    io.contextP = 0;
    io.videoP = videoP;
    io.audioP = audioP;
    io.outP = outP;
    io.videoSize = (UInt32)ftell(videoP);
    io.audioSize = (UInt32)ftell(audioP);
    Mp4Merge(&io, &result);
    fprintf(stderr, "\n");
    fclose(videoP);
    fclose(audioP);
    fclose(outP);
    if (result.status != mp4MergeOk) {
        fprintf(stderr, "merge failed: status %u, detail %lu\n", result.status,
            (unsigned long)result.detail);
        return 1;
    }
    printf("merged: %lu video + %lu audio samples, %lu bytes\n",
        (unsigned long)result.videoSamples, (unsigned long)result.audioSamples,
        (unsigned long)result.outputSize);
    return 0;
}

static int Get(const char *videoIdP, const char *dirP)
{
    static YtPlayer player;
    char videoPath[512], audioPath[512], outPath[640];
    Char name[80];
    if (!LoadPlayer(videoIdP, &player)) return 1;
    if (!YtPlayerPlayable(&player)) {
        fprintf(stderr, "not playable\n");
        return 3;
    }
    snprintf(videoPath, sizeof(videoPath), "%s/%s.video.mp4", dirP, videoIdP);
    snprintf(audioPath, sizeof(audioPath), "%s/%s.audio.mp4", dirP, videoIdP);
    TextFileName(player.title, videoIdP, ".mp4", name, sizeof(name));
    snprintf(outPath, sizeof(outPath), "%s/%s", dirP, name);
    if (!Download(&player.video, videoPath) ||
        !Download(&player.audio, audioPath)) return 1;
    printf("output: %s\n", outPath);
    return Merge(videoPath, audioPath, outPath);
}

int main(int argc, char **argv)
{
    YtClientDefaults(&gClient);
    LoadVisitor();
    if (argc == 3 && strcmp(argv[1], "search") == 0) return Search(argv[2]);
    if (argc == 3 && strcmp(argv[1], "info") == 0) return Info(argv[2]);
    if (argc == 4 && strcmp(argv[1], "get") == 0) return Get(argv[2], argv[3]);
    if (argc == 5 && strcmp(argv[1], "merge") == 0)
        return Merge(argv[2], argv[3], argv[4]);
    fprintf(stderr, "usage: pview search QUERY | info ID | get ID DIR | "
        "merge VIDEO AUDIO OUT\n");
    return 2;
}
