/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_TEXT_H
#define PALMVIEW_TEXT_H

#include <PalmOS.h>

/* How the Palm shows text: Windows-1252, or Shift-JIS with CJKOS */
enum { textWestern = 0, textShiftJis, textAscii };

/* Palm text in an encoding. Shift-JIS needs the table that
 * tools/jis-table.py builds: jisCount entries of 4 bytes. A 0 codec
 * means Western. */
typedef struct TextCodec {
    UInt16 encoding;
    const UInt8 *jisP;
    UInt16 jisCount;
} TextCodec;

/* Converts UTF-8 to Palm text. Cyrillic is transliterated. In Japanese,
 * curly quotes and dashes become ASCII and accented letters lose their
 * accents. Other unsupported characters become '?'. Returns the output
 * length. The output is always NUL-terminated. */
UInt16 TextUtf8ToPalm(const Char *srcP, UInt16 srcLength, Char *dstP,
                      UInt16 capacity, const TextCodec *codecP);

/* Appends srcP to a buffer as UTF-8 JSON string content (no quotes). The
 * source is Palm text in codecP, or UTF-8 when codecP is 0. Returns false
 * if it does not fit. */
Boolean TextAppendJson(Char *dstP, UInt16 capacity, UInt16 *lengthP,
                       const Char *srcP, const TextCodec *codecP);

/* The start of the character at or before offset, so a cut there does not
 * split a Japanese character. */
UInt16 TextCharStart(const TextCodec *codecP, const Char *textP,
                     UInt16 offset);
/* "..." as one character in the codec, or three dots without one */
const Char *TextEllipsis(const TextCodec *codecP);

/* Appends a literal string. Returns false if it does not fit. */
Boolean TextAppend(Char *dstP, UInt16 capacity, UInt16 *lengthP,
                   const Char *srcP);
Boolean TextAppendUInt32(Char *dstP, UInt16 capacity, UInt16 *lengthP,
                         UInt32 value);

/* Parses leading decimal digits. Returns false if there are none. */
Boolean TextParseUInt32(const Char *textP, UInt32 *valueP);
/* Compares dotted versions such as "0.10.2" part by part: negative, 0 or
 * positive. Missing parts count as 0, and anything after a part that is
 * not a dot is ignored. */
Int16 TextCompareVersions(const Char *aP, const Char *bP);

/* Shortens YouTube's view counts and ages: "1.8B views" becomes "1.8B",
 * "10 years ago" and "Streamed 10 years ago" become "10y ago". Other text,
 * such as "No views", is copied as it is. */
void TextShorten(const Char *srcP, Char *dstP, UInt16 capacity);

/* Makes a FAT-safe ASCII file name from a title in codecP. */
void TextFileName(const Char *titleP, const Char *fallbackP,
                  const Char *extensionP, Char *dstP, UInt16 capacity,
                  const TextCodec *codecP);

#endif
