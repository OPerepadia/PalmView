/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_TEXT_H
#define PALMVIEW_TEXT_H

#include <PalmOS.h>

/* Converts UTF-8 to the Palm's Windows-1252 text. Cyrillic is
 * transliterated; other unsupported characters become '?'. Returns the
 * output length. The output is always NUL-terminated. */
UInt16 TextUtf8ToPalm(const Char *srcP, UInt16 srcLength, Char *dstP,
                      UInt16 capacity);

/* Appends srcP to a buffer as UTF-8 JSON string content (no quotes). The
 * source is Windows-1252 when fromPalm is true, otherwise UTF-8/ASCII.
 * Returns false if it does not fit. */
Boolean TextAppendJson(Char *dstP, UInt16 capacity, UInt16 *lengthP,
                       const Char *srcP, Boolean fromPalm);

/* Appends a literal string. Returns false if it does not fit. */
Boolean TextAppend(Char *dstP, UInt16 capacity, UInt16 *lengthP,
                   const Char *srcP);
Boolean TextAppendUInt32(Char *dstP, UInt16 capacity, UInt16 *lengthP,
                         UInt32 value);

/* Parses leading decimal digits. Returns false if there are none. */
Boolean TextParseUInt32(const Char *textP, UInt32 *valueP);

/* Shortens YouTube's view counts and ages: "1.8B views" becomes "1.8B",
 * "10 years ago" and "Streamed 10 years ago" become "10y ago". Other text,
 * such as "No views", is copied as it is. */
void TextShorten(const Char *srcP, Char *dstP, UInt16 capacity);

/* Makes a FAT-safe ASCII file name from a Palm-encoded title. */
void TextFileName(const Char *titleP, const Char *fallbackP,
                  const Char *extensionP, Char *dstP, UInt16 capacity);

#endif
