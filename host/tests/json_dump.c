/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Prints every scalar and container end with its path, one per line, so the
 * output can be compared with host/tests/json_reference.py. Input is fed in
 * pseudo-random chunk sizes to exercise chunk boundaries. */

#include <PalmOS.h>
#include "json_scan.h"

static void PrintPath(const JsonScanner *s)
{
    UInt16 i;
    for (i = 0; i < s->depth; i++) {
        if (i != 0) putchar('/');
        if (s->frames[i].isArray) printf("#%u", s->frames[i].index);
        else fputs(s->frames[i].key, stdout);
    }
}

static Boolean OnValue(void *contextP, const JsonScanner *s, UInt16 type,
                       const Char *textP, UInt16 length)
{
    static const char *names[] = { "s", "n", "t", "f", "z" };
    (void)contextP;
    (void)length;
    PrintPath(s);
    printf("\t%s\t%s\n", names[type], textP);
    return true;
}

static Boolean OnEnd(void *contextP, const JsonScanner *s)
{
    (void)contextP;
    PrintPath(s);
    printf("\tend\n");
    return true;
}

int main(int argc, char **argv)
{
    static Char value[16384];
    static UInt8 data[1 << 20];
    JsonScanner scanner;
    FILE *fileP;
    size_t size, offset = 0;
    UInt32 seed = argc > 2 ? (UInt32)atoi(argv[2]) : 1;
    UInt16 status;

    if (argc < 2 || (fileP = fopen(argv[1], "rb")) == 0) {
        fprintf(stderr, "usage: json_dump FILE [SEED]\n");
        return 2;
    }
    size = fread(data, 1, sizeof(data), fileP);
    fclose(fileP);
    JsonScanInit(&scanner, value, sizeof(value), OnValue, OnEnd, 0);
    while (offset < size) {
        size_t chunk;
        seed = seed * 1103515245UL + 12345UL;
        chunk = 1 + (seed >> 16) % 2048;
        if (chunk > size - offset) chunk = size - offset;
        status = JsonScanFeed(&scanner, data + offset, (UInt16)chunk);
        if (status != jsonOk) {
            fprintf(stderr, "feed failed: %u at %zu\n", status, offset);
            return 1;
        }
        offset += chunk;
    }
    status = JsonScanFinish(&scanner);
    if (status != jsonOk) {
        fprintf(stderr, "finish failed: %u\n", status);
        return 1;
    }
    return 0;
}
