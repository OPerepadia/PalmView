/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_JSON_SCAN_H
#define PALMVIEW_JSON_SCAN_H

#include <PalmOS.h>

/* Streaming JSON scanner. Feed it response bytes in any chunk size and it
 * reports every scalar value with its path, so callers can pick out a few
 * fields from a large document without buffering it. */

#define JSON_MAX_DEPTH 24
#define JSON_KEY_CAPACITY 24

typedef enum JsonValueType {
    jsonTypeString = 0,
    jsonTypeNumber,
    jsonTypeTrue,
    jsonTypeFalse,
    jsonTypeNull
} JsonValueType;

typedef enum JsonStatus {
    jsonOk = 0,
    jsonSyntaxError,
    jsonStopped
} JsonStatus;

typedef struct JsonFrame {
    Boolean isArray;
    UInt16 index;
    Char key[JSON_KEY_CAPACITY];
} JsonFrame;

typedef struct JsonScanner JsonScanner;

/* Called for each scalar. Strings arrive unescaped as UTF-8 and are
 * NUL-terminated. Return false to stop scanning. */
typedef Boolean (*JsonValueProc)(void *contextP, const JsonScanner *scannerP,
                                 UInt16 type, const Char *textP,
                                 UInt16 length);
/* Called when an object or array closes; the path still includes it. */
typedef Boolean (*JsonEndProc)(void *contextP, const JsonScanner *scannerP);

struct JsonScanner {
    JsonValueProc valueProcP;
    JsonEndProc endProcP;
    void *contextP;
    Char *valueP;
    UInt16 valueCapacity;
    UInt16 valueLength;
    Boolean valueTruncated;
    UInt16 depth;
    UInt16 overflowDepth;
    UInt32 overflowKindsBits;
    UInt16 status;
    UInt8 state;
    UInt8 stringState;
    Boolean inKey;
    UInt8 hexCount;
    UInt16 hexValue;
    UInt16 highSurrogate;
    UInt16 keyLength;
    JsonFrame frames[JSON_MAX_DEPTH];
};

/* valueP must hold valueCapacity bytes; longer strings are truncated and
 * flagged in valueTruncated while the callback runs. */
void JsonScanInit(JsonScanner *scannerP, Char *valueP, UInt16 valueCapacity,
                  JsonValueProc valueProcP, JsonEndProc endProcP,
                  void *contextP);
UInt16 JsonScanFeed(JsonScanner *scannerP, const UInt8 *dataP, UInt16 length);
/* Returns jsonOk only when a complete top-level value was read. */
UInt16 JsonScanFinish(JsonScanner *scannerP);

/* Path matching. A pattern is keys separated by '/', where '#' matches any
 * array index and '*' matches any single key or index.
 * Example: "streamingData/adaptiveFormats/#/url". */
Boolean JsonPathIs(const JsonScanner *scannerP, const Char *patternP);
Boolean JsonPathEndsWith(const JsonScanner *scannerP, const Char *patternP);
/* Matches the path from the given level to the end. */
Boolean JsonPathFrom(const JsonScanner *scannerP, UInt16 level,
                     const Char *patternP);
/* Index of the array element at the given path level, counted from root. */
UInt16 JsonIndexAt(const JsonScanner *scannerP, UInt16 level);

#endif
