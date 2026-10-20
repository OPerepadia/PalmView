/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "text.h"

#define FILE_NAME_MAX_TITLE 48

/* Unicode code points for Windows-1252 bytes 0x80-0x9F; 0 is unassigned. */
static const UInt16 kCp1252High[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178
};

/* Uppercase А..Я (U+0410..U+042F), Ukrainian-style transliteration.
 * Fixed-width rows: a pointer table would need relocated .data, which
 * Palm OS 68K code resources do not have. */
static const Char kCyrillic[32][5] = {
    "A", "B", "V", "H", "D", "E", "Zh", "Z", "Y", "Y", "K", "L", "M", "N",
    "O", "P", "R", "S", "T", "U", "F", "Kh", "Ts", "Ch", "Sh", "Shch", "",
    "Y", "", "E", "Yu", "Ya"
};

static Boolean PutChar(Char *dstP, UInt16 capacity, UInt16 *lengthP, Char c)
{
    if (*lengthP + 1 >= capacity) return false;
    dstP[(*lengthP)++] = c;
    dstP[*lengthP] = '\0';
    return true;
}

static const Char *CyrillicLatin(UInt32 cp, Boolean *lowerP)
{
    *lowerP = false;
    if (cp >= 0x0410 && cp <= 0x042F) return kCyrillic[cp - 0x0410];
    if (cp >= 0x0430 && cp <= 0x044F) {
        *lowerP = true;
        return kCyrillic[cp - 0x0430];
    }
    if (cp >= 0x0450 && cp <= 0x045F) *lowerP = true;
    switch (cp) {
        case 0x0401: case 0x0451: return "Yo";
        case 0x0404: case 0x0454: return "Ye";
        case 0x0406: case 0x0456: return "I";
        case 0x0407: case 0x0457: return "Yi";
        case 0x040E: case 0x045E: return "U";
        case 0x0490: return "G";
        case 0x0491: *lowerP = true; return "G";
        default: return 0;
    }
}

static UInt8 PalmByteFor(UInt32 cp)
{
    UInt16 i;
    if (cp < 0x80UL || (cp >= 0xA0UL && cp <= 0xFFUL)) return (UInt8)cp;
    for (i = 0; i < 32; i++)
        if (kCp1252High[i] == cp) return (UInt8)(0x80 + i);
    return 0;
}

/* Characters that carry no text on their own and are dropped. */
static Boolean IsInvisible(UInt32 cp)
{
    return cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF ||
        (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x1F000UL && cp <= 0x1FFFFUL);
}

/* Decodes one UTF-8 sequence. Invalid input yields U+FFFD and one byte. */
static UInt32 DecodeUtf8(const UInt8 *p, UInt16 remaining, UInt16 *usedP)
{
    UInt32 cp;
    UInt16 count, i;
    if (p[0] < 0x80) {
        *usedP = 1;
        return p[0];
    }
    if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; count = 2; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; count = 3; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; count = 4; }
    else { *usedP = 1; return 0xFFFDUL; }
    if (count > remaining) { *usedP = 1; return 0xFFFDUL; }
    for (i = 1; i < count; i++) {
        if ((p[i] & 0xC0) != 0x80) { *usedP = 1; return 0xFFFDUL; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *usedP = count;
    return cp;
}

UInt16 TextUtf8ToPalm(const Char *srcP, UInt16 srcLength, Char *dstP,
                      UInt16 capacity)
{
    const UInt8 *p = (const UInt8 *)srcP;
    UInt16 length = 0;
    UInt16 offset = 0;
    Boolean lastUnknown = false;
    if (capacity == 0) return 0;
    dstP[0] = '\0';
    while (offset < srcLength) {
        UInt16 used;
        UInt32 cp = DecodeUtf8(p + offset, srcLength - offset, &used);
        UInt8 byte;
        Boolean lower;
        const Char *latinP;
        offset += used;
        if (IsInvisible(cp)) continue;
        if (cp == '\n' || cp == '\r' || cp == '\t') cp = ' ';
        byte = PalmByteFor(cp);
        if (byte >= 0x20) {
            if (!PutChar(dstP, capacity, &length, (Char)byte)) break;
            lastUnknown = false;
            continue;
        }
        latinP = CyrillicLatin(cp, &lower);
        if (latinP != 0) {
            for (; *latinP != '\0'; latinP++) {
                Char c = *latinP;
                if (lower && c >= 'A' && c <= 'Z') c = (Char)(c + ('a' - 'A'));
                if (!PutChar(dstP, capacity, &length, c)) return length;
            }
            lastUnknown = false;
            continue;
        }
        if (!lastUnknown && !PutChar(dstP, capacity, &length, '?')) break;
        lastUnknown = true;
    }
    return length;
}

Boolean TextAppend(Char *dstP, UInt16 capacity, UInt16 *lengthP,
                   const Char *srcP)
{
    for (; *srcP != '\0'; srcP++)
        if (!PutChar(dstP, capacity, lengthP, *srcP)) return false;
    return true;
}

Boolean TextAppendUInt32(Char *dstP, UInt16 capacity, UInt16 *lengthP,
                         UInt32 value)
{
    Char digits[11];
    UInt16 count = 0;
    do {
        digits[count++] = (Char)('0' + (UInt16)(value % 10UL));
        value /= 10UL;
    } while (value != 0);
    while (count != 0)
        if (!PutChar(dstP, capacity, lengthP, digits[--count])) return false;
    return true;
}

static Boolean AppendUtf8(Char *dstP, UInt16 capacity, UInt16 *lengthP,
                          UInt32 cp)
{
    if (cp < 0x80UL) return PutChar(dstP, capacity, lengthP, (Char)cp);
    if (cp < 0x800UL)
        return PutChar(dstP, capacity, lengthP, (Char)(0xC0 | (cp >> 6))) &&
            PutChar(dstP, capacity, lengthP, (Char)(0x80 | (cp & 0x3F)));
    return PutChar(dstP, capacity, lengthP, (Char)(0xE0 | (cp >> 12))) &&
        PutChar(dstP, capacity, lengthP, (Char)(0x80 | ((cp >> 6) & 0x3F))) &&
        PutChar(dstP, capacity, lengthP, (Char)(0x80 | (cp & 0x3F)));
}

Boolean TextAppendJson(Char *dstP, UInt16 capacity, UInt16 *lengthP,
                       const Char *srcP, Boolean fromPalm)
{
    static const Char hex[] = "0123456789abcdef";
    const UInt8 *p = (const UInt8 *)srcP;
    for (; *p != 0; p++) {
        UInt8 c = *p;
        Boolean ok;
        if (c == '"' || c == '\\') {
            ok = PutChar(dstP, capacity, lengthP, '\\') &&
                PutChar(dstP, capacity, lengthP, (Char)c);
        } else if (c < 0x20) {
            ok = TextAppend(dstP, capacity, lengthP, "\\u00") &&
                PutChar(dstP, capacity, lengthP, hex[c >> 4]) &&
                PutChar(dstP, capacity, lengthP, hex[c & 0x0F]);
        } else if (c >= 0x80 && fromPalm) {
            UInt32 cp = c < 0xA0 ? kCp1252High[c - 0x80] : c;
            ok = cp == 0 || AppendUtf8(dstP, capacity, lengthP, cp);
        } else {
            ok = PutChar(dstP, capacity, lengthP, (Char)c);
        }
        if (!ok) return false;
    }
    return true;
}

Boolean TextParseUInt32(const Char *textP, UInt32 *valueP)
{
    UInt32 value = 0;
    Boolean found = false;
    while (*textP >= '0' && *textP <= '9') {
        value = value * 10UL + (UInt16)(*textP++ - '0');
        found = true;
    }
    if (found) *valueP = value;
    return found;
}

static Boolean FileNameChar(Char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' ||
        c == '.' || c == '(' || c == ')' || c == ',' || c == '&' ||
        c == '\'' || c == '!';
}

void TextFileName(const Char *titleP, const Char *fallbackP,
                  const Char *extensionP, Char *dstP, UInt16 capacity)
{
    UInt16 length = 0;
    UInt16 limit = (UInt16)(capacity - StrLen(extensionP) - 1);
    if (limit > FILE_NAME_MAX_TITLE) limit = FILE_NAME_MAX_TITLE;
    dstP[0] = '\0';
    for (; *titleP != '\0' && length < limit; titleP++) {
        Char c = FileNameChar(*titleP) ? *titleP : '_';
        if ((c == ' ' || c == '_') &&
            (length == 0 || dstP[length - 1] == ' ' || dstP[length - 1] == '_'))
            continue;
        dstP[length++] = c;
    }
    while (length != 0 && (dstP[length - 1] == ' ' ||
                           dstP[length - 1] == '.' ||
                           dstP[length - 1] == '_'))
        length--;
    dstP[length] = '\0';
    if (length == 0) {
        StrNCopy(dstP, fallbackP, limit);
        dstP[limit] = '\0';
        length = StrLen(dstP);
    }
    TextAppend(dstP, capacity, &length, extensionP);
}
