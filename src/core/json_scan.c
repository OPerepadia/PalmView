/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "json_scan.h"

enum {
    stValue = 0,
    stObjectStart,
    stArrayStart,
    stKeyExpected,
    stColon,
    stAfterValue,
    stString,
    stNumber,
    stLiteral,
    stDone
};

enum {
    strNormal = 0,
    strEscape,
    strHex
};

/* Containers nested deeper than JSON_MAX_DEPTH are parsed but not reported;
 * this bit stack remembers which of them are arrays. */
#define JSON_MAX_OVERFLOW 32

static Boolean IsSpace(UInt8 c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static Boolean TopIsArray(const JsonScanner *s)
{
    if (s->overflowDepth != 0)
        return (s->overflowKindsBits >> (s->overflowDepth - 1)) & 1;
    return s->depth != 0 && s->frames[s->depth - 1].isArray;
}

static Boolean Reporting(const JsonScanner *s)
{
    return s->overflowDepth == 0;
}

static void PutByte(JsonScanner *s, UInt8 c)
{
    if (s->inKey) {
        if (Reporting(s) && s->depth != 0 &&
            s->keyLength < JSON_KEY_CAPACITY - 1) {
            JsonFrame *frameP = &s->frames[s->depth - 1];
            frameP->key[s->keyLength] = (Char)c;
            frameP->key[s->keyLength + 1] = '\0';
        }
        s->keyLength++;
        return;
    }
    if (s->valueLength < s->valueCapacity - 1)
        s->valueP[s->valueLength++] = (Char)c;
    else
        s->valueTruncated = true;
}

static void PutCodePoint(JsonScanner *s, UInt32 cp)
{
    if (cp < 0x80UL) {
        PutByte(s, (UInt8)cp);
    } else if (cp < 0x800UL) {
        PutByte(s, (UInt8)(0xC0 | (cp >> 6)));
        PutByte(s, (UInt8)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000UL) {
        PutByte(s, (UInt8)(0xE0 | (cp >> 12)));
        PutByte(s, (UInt8)(0x80 | ((cp >> 6) & 0x3F)));
        PutByte(s, (UInt8)(0x80 | (cp & 0x3F)));
    } else {
        PutByte(s, (UInt8)(0xF0 | (cp >> 18)));
        PutByte(s, (UInt8)(0x80 | ((cp >> 12) & 0x3F)));
        PutByte(s, (UInt8)(0x80 | ((cp >> 6) & 0x3F)));
        PutByte(s, (UInt8)(0x80 | (cp & 0x3F)));
    }
}

static void BeginValueText(JsonScanner *s)
{
    s->valueLength = 0;
    s->valueTruncated = false;
}

static Boolean EmitValue(JsonScanner *s, UInt16 type)
{
    s->valueP[s->valueLength] = '\0';
    s->state = s->depth == 0 && s->overflowDepth == 0 ? stDone : stAfterValue;
    if (!Reporting(s) || s->valueProcP == 0) return true;
    if (!s->valueProcP(s->contextP, s, type, s->valueP, s->valueLength)) {
        s->status = jsonStopped;
        return false;
    }
    return true;
}

static Boolean OpenContainer(JsonScanner *s, Boolean isArray)
{
    if (s->overflowDepth != 0 || s->depth == JSON_MAX_DEPTH) {
        if (s->overflowDepth == JSON_MAX_OVERFLOW) {
            s->status = jsonSyntaxError;
            return false;
        }
        if (isArray) s->overflowKindsBits |= 1UL << s->overflowDepth;
        else s->overflowKindsBits &= ~(1UL << s->overflowDepth);
        s->overflowDepth++;
    } else {
        JsonFrame *frameP = &s->frames[s->depth++];
        frameP->isArray = isArray;
        frameP->index = 0;
        frameP->key[0] = '\0';
    }
    s->state = isArray ? stArrayStart : stObjectStart;
    return true;
}

static Boolean CloseContainer(JsonScanner *s, Boolean isArray)
{
    if (TopIsArray(s) != isArray ||
        (s->depth == 0 && s->overflowDepth == 0)) {
        s->status = jsonSyntaxError;
        return false;
    }
    if (s->overflowDepth != 0) {
        s->overflowDepth--;
    } else {
        /* Report the closed container at its position in the parent. */
        s->depth--;
        if (s->endProcP != 0 && !s->endProcP(s->contextP, s)) {
            s->status = jsonStopped;
            return false;
        }
    }
    s->state = s->depth == 0 && s->overflowDepth == 0 ? stDone : stAfterValue;
    return true;
}

static Boolean LiteralIs(const JsonScanner *s, const Char *textP)
{
    UInt16 i;
    for (i = 0; i < s->valueLength; i++)
        if (textP[i] != s->valueP[i]) return false;
    return textP[s->valueLength] == '\0';
}

static Boolean FinishLiteral(JsonScanner *s)
{
    if (LiteralIs(s, "true")) return EmitValue(s, jsonTypeTrue);
    if (LiteralIs(s, "false")) return EmitValue(s, jsonTypeFalse);
    if (LiteralIs(s, "null")) return EmitValue(s, jsonTypeNull);
    s->status = jsonSyntaxError;
    return false;
}

static Boolean StringByte(JsonScanner *s, UInt8 c)
{
    if (s->stringState == strEscape) {
        s->stringState = strNormal;
        switch (c) {
            case '"': case '\\': case '/': PutByte(s, c); return true;
            case 'b': PutByte(s, '\b'); return true;
            case 'f': PutByte(s, '\f'); return true;
            case 'n': PutByte(s, '\n'); return true;
            case 'r': PutByte(s, '\r'); return true;
            case 't': PutByte(s, '\t'); return true;
            case 'u':
                s->stringState = strHex;
                s->hexCount = 0;
                s->hexValue = 0;
                return true;
            default:
                s->status = jsonSyntaxError;
                return false;
        }
    }
    if (s->stringState == strHex) {
        UInt16 digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else {
            s->status = jsonSyntaxError;
            return false;
        }
        s->hexValue = (UInt16)((s->hexValue << 4) | digit);
        if (++s->hexCount < 4) return true;
        s->stringState = strNormal;
        if (s->hexValue >= 0xD800 && s->hexValue < 0xDC00) {
            s->highSurrogate = s->hexValue;
        } else if (s->hexValue >= 0xDC00 && s->hexValue < 0xE000 &&
                   s->highSurrogate != 0) {
            PutCodePoint(s, 0x10000UL +
                (((UInt32)(s->highSurrogate - 0xD800)) << 10) +
                (s->hexValue - 0xDC00));
            s->highSurrogate = 0;
        } else {
            s->highSurrogate = 0;
            PutCodePoint(s, s->hexValue);
        }
        return true;
    }
    if (c == '\\') {
        s->stringState = strEscape;
        return true;
    }
    if (c == '"') {
        if (s->inKey) {
            s->inKey = false;
            s->state = stColon;
            return true;
        }
        return EmitValue(s, jsonTypeString);
    }
    PutByte(s, c);
    return true;
}

static void BeginString(JsonScanner *s, Boolean isKey)
{
    s->state = stString;
    s->stringState = strNormal;
    s->highSurrogate = 0;
    s->inKey = isKey;
    if (isKey) {
        s->keyLength = 0;
        if (Reporting(s) && s->depth != 0)
            s->frames[s->depth - 1].key[0] = '\0';
    } else {
        BeginValueText(s);
    }
}

static Boolean StartValue(JsonScanner *s, UInt8 c)
{
    if (c == '{') return OpenContainer(s, false);
    if (c == '[') return OpenContainer(s, true);
    if (c == '"') {
        BeginString(s, false);
        return true;
    }
    BeginValueText(s);
    if (c == '-' || (c >= '0' && c <= '9')) {
        s->state = stNumber;
        PutByte(s, c);
        return true;
    }
    if (c == 't' || c == 'f' || c == 'n') {
        s->state = stLiteral;
        PutByte(s, c);
        return true;
    }
    s->status = jsonSyntaxError;
    return false;
}

/* Returns false on error or stop. *consumedP is false when the byte ended a
 * number or literal and must be processed again in the next state. */
static Boolean Step(JsonScanner *s, UInt8 c, Boolean *consumedP)
{
    *consumedP = true;
    switch (s->state) {
        case stValue:
            if (IsSpace(c)) return true;
            return StartValue(s, c);
        case stObjectStart:
            if (IsSpace(c)) return true;
            if (c == '}') return CloseContainer(s, false);
            if (c == '"') {
                BeginString(s, true);
                return true;
            }
            break;
        case stArrayStart:
            if (IsSpace(c)) return true;
            if (c == ']') return CloseContainer(s, true);
            return StartValue(s, c);
        case stKeyExpected:
            if (IsSpace(c)) return true;
            if (c == '"') {
                BeginString(s, true);
                return true;
            }
            break;
        case stColon:
            if (IsSpace(c)) return true;
            if (c == ':') {
                s->state = stValue;
                return true;
            }
            break;
        case stAfterValue:
            if (IsSpace(c)) return true;
            if (c == ',') {
                if (TopIsArray(s)) {
                    if (s->overflowDepth == 0)
                        s->frames[s->depth - 1].index++;
                    s->state = stValue;
                } else {
                    s->state = stKeyExpected;
                }
                return true;
            }
            if (c == '}') return CloseContainer(s, false);
            if (c == ']') return CloseContainer(s, true);
            break;
        case stString:
            return StringByte(s, c);
        case stNumber:
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                c == '+' || c == '-') {
                PutByte(s, c);
                return true;
            }
            *consumedP = false;
            return EmitValue(s, jsonTypeNumber);
        case stLiteral:
            if (c >= 'a' && c <= 'z') {
                PutByte(s, c);
                return true;
            }
            *consumedP = false;
            return FinishLiteral(s);
        case stDone:
            if (IsSpace(c)) return true;
            break;
        default:
            break;
    }
    s->status = jsonSyntaxError;
    return false;
}

void JsonScanInit(JsonScanner *scannerP, Char *valueP, UInt16 valueCapacity,
                  JsonValueProc valueProcP, JsonEndProc endProcP,
                  void *contextP)
{
    MemSet(scannerP, sizeof(*scannerP), 0);
    scannerP->valueP = valueP;
    scannerP->valueCapacity = valueCapacity;
    scannerP->valueProcP = valueProcP;
    scannerP->endProcP = endProcP;
    scannerP->contextP = contextP;
    scannerP->state = stValue;
}

/* Copies the plain bytes of a string value, most of a reply, in one loop
 * instead of a Step per byte. Returns how many it took. */
static UInt16 CopyPlain(JsonScanner *s, const UInt8 *dataP, UInt16 length)
{
    UInt16 i = 0, room = (UInt16)(s->valueCapacity - 1);
    for (; i < length && dataP[i] != '"' && dataP[i] != '\\'; i++) {
        if (s->valueLength < room) s->valueP[s->valueLength++] = (Char)dataP[i];
        else s->valueTruncated = true;
    }
    return i;
}

UInt16 JsonScanFeed(JsonScanner *scannerP, const UInt8 *dataP, UInt16 length)
{
    UInt16 i = 0;
    if (scannerP->status != jsonOk) return scannerP->status;
    while (i < length) {
        Boolean consumed;
        if (scannerP->state == stString && scannerP->stringState == strNormal &&
                !scannerP->inKey) {
            i += CopyPlain(scannerP, dataP + i, (UInt16)(length - i));
            if (i == length) break;
        }
        if (!Step(scannerP, dataP[i], &consumed)) return scannerP->status;
        if (consumed) i++;
    }
    return jsonOk;
}

UInt16 JsonScanFinish(JsonScanner *scannerP)
{
    if (scannerP->status != jsonOk) return scannerP->status;
    if (scannerP->state == stNumber && scannerP->depth == 0) {
        if (!EmitValue(scannerP, jsonTypeNumber)) return scannerP->status;
    } else if (scannerP->state == stLiteral && scannerP->depth == 0) {
        if (!FinishLiteral(scannerP)) return scannerP->status;
    }
    if (scannerP->state != stDone) scannerP->status = jsonSyntaxError;
    return scannerP->status;
}

static Boolean ComponentMatches(const JsonFrame *frameP, const Char *startP,
                                UInt16 length)
{
    UInt16 i;
    if (length == 1 && startP[0] == '*') return true;
    if (length == 1 && startP[0] == '#') return frameP->isArray;
    if (frameP->isArray) return false;
    /* Keys longer than the frame capacity were stored truncated. */
    for (i = 0; i < length && i < JSON_KEY_CAPACITY - 1; i++)
        if (frameP->key[i] != startP[i]) return false;
    return length >= JSON_KEY_CAPACITY - 1 || frameP->key[length] == '\0';
}

static UInt16 CountComponents(const Char *patternP)
{
    UInt16 count = 1;
    if (*patternP == '\0') return 0;
    for (; *patternP != '\0'; patternP++)
        if (*patternP == '/') count++;
    return count;
}

/* Matches the path from level to its end. Most patterns fail on their
 * first key, so nothing is counted first. */
static Boolean MatchFrom(const JsonScanner *s, UInt16 level,
                         const Char *patternP)
{
    while (*patternP != '\0') {
        const Char *endP = patternP;
        if (level >= s->depth) return false;
        while (*endP != '\0' && *endP != '/') endP++;
        if (!ComponentMatches(&s->frames[level], patternP,
                (UInt16)(endP - patternP))) return false;
        level++;
        patternP = *endP == '/' ? endP + 1 : endP;
    }
    return level == s->depth;
}

Boolean JsonPathIs(const JsonScanner *scannerP, const Char *patternP)
{
    if (scannerP->overflowDepth != 0) return false;
    return MatchFrom(scannerP, 0, patternP);
}

Boolean JsonPathEndsWith(const JsonScanner *scannerP, const Char *patternP)
{
    UInt16 count = CountComponents(patternP);
    if (scannerP->overflowDepth != 0 || count > scannerP->depth) return false;
    return MatchFrom(scannerP, (UInt16)(scannerP->depth - count), patternP);
}

Boolean JsonPathFrom(const JsonScanner *scannerP, UInt16 level,
                     const Char *patternP)
{
    if (scannerP->overflowDepth != 0) return false;
    return MatchFrom(scannerP, level, patternP);
}

UInt16 JsonIndexAt(const JsonScanner *scannerP, UInt16 level)
{
    return level < scannerP->depth ? scannerP->frames[level].index : 0;
}
