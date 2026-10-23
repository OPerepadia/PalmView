/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "udp_log.h"
#include "text.h"

#define UDP_LOG_CAPACITY 1024
/* Room kept for one line, so a line never has to be split. */
#define LINE_CAPACITY 128
/* NetLib can block a send while the network stalls, which freezes the
 * player. A send that takes longer than SLOW_SEND_MS stops sending for
 * PAUSE_SECONDS. */
#define SLOW_SEND_MS 200
#define PAUSE_SECONDS 20

/* Allocated on its own: NetLib's native code reads the address, and this
 * keeps it aligned like NetJob's, which is known to work. */
struct UdpLog {
    UInt16 netRefNum;
    NetSocketRef socket;
    NetSocketAddrINType address;
    UInt32 startTicks;
    UInt32 sentTicks;
    UInt32 pausedTicks;     /* when sending stopped after a slow send, or 0 */
    UInt16 length;
    Char buffer[UDP_LOG_CAPACITY];
};

static void Send(UdpLog *logP)
{
    UInt32 start = TimGetTicks(), ms;
    Err error;
    if (logP->length != 0)
        NetLibSend(logP->netRefNum, logP->socket, logP->buffer, logP->length,
            0, 0, 0, 1, &error);
    logP->length = 0;
    logP->sentTicks = TimGetTicks();
    ms = (logP->sentTicks - start) * 1000UL / SysTicksPerSecond();
    if (ms > SLOW_SEND_MS) {
        logP->pausedTicks = logP->sentTicks;
        UdpLogLine(logP, "log", "send took % ms, pausing % s", ms,
            PAUSE_SECONDS, 0);
    }
}

UdpLog *UdpLogOpen(UInt16 netRefNum, const Char *destP)
{
    UdpLog *logP;
    Char host[16];
    const Char *colonP = StrChr(destP, ':');
    UInt32 port = 5140;
    Int16 enabled = 1;
    UInt16 length;
    Err error;
    length = colonP != 0 ? (UInt16)(colonP - destP) : StrLen(destP);
    if (length == 0 || length >= sizeof(host)) return 0;
    MemMove(host, destP, length);
    host[length] = '\0';
    if (colonP != 0 && (!TextParseUInt32(colonP + 1, &port) || port == 0 ||
            port > 65535UL))
        return 0;
    logP = (UdpLog *)MemPtrNew(sizeof(UdpLog));
    if (logP == 0) return 0;
    MemSet(logP, sizeof(*logP), 0);
    logP->netRefNum = netRefNum;
    logP->address.family = netSocketAddrINET;
    logP->address.port = NetHToNS((UInt16)port);
    logP->address.addr = NetLibAddrAToIN(netRefNum, host);
    logP->socket = -1;
    if (logP->address.addr != (NetIPAddr)-1)
        logP->socket = NetLibSocketOpen(netRefNum, netSocketAddrINET,
            netSocketTypeDatagram, 0, SysTicksPerSecond(), &error);
    /* Connected, so sends need no address. */
    if (logP->socket < 0 ||
        NetLibSocketConnect(netRefNum, logP->socket,
            (NetSocketAddrType *)&logP->address, sizeof(logP->address),
            SysTicksPerSecond(), &error) < 0) {
        if (logP->socket >= 0)
            NetLibSocketClose(netRefNum, logP->socket, SysTicksPerSecond(),
                &error);
        MemPtrFree(logP);
        return 0;
    }
    NetLibSocketOptionSet(netRefNum, logP->socket, netSocketOptLevelSocket,
        netSocketOptSockNonBlocking, &enabled, sizeof(enabled),
        SysTicksPerSecond(), &error);
    logP->startTicks = TimGetTicks();
    UdpLogLine(logP, "log", "start", 0, 0, 0);
    return logP;
}

void UdpLogClose(UdpLog *logP)
{
    Err error;
    if (logP == 0) return;
    Send(logP);
    NetLibSocketClose(logP->netRefNum, logP->socket, SysTicksPerSecond(),
        &error);
    MemPtrFree(logP);
}

static void AppendHex(Char *dstP, UInt16 capacity, UInt16 *lengthP,
                      UInt32 value)
{
    static const Char kDigits[] = "0123456789ABCDEF";
    Char text[11];
    UInt16 i;
    text[0] = '0';
    text[1] = 'x';
    for (i = 0; i < 8; i++)
        text[2 + i] = kDigits[(value >> (28 - 4 * i)) & 0x0F];
    text[10] = '\0';
    /* 0x1212, not 0x00001212 */
    for (i = 2; i < 6 && text[i] == '0'; i++) {}
    MemMove(text + 2, text + i, 11 - i);
    TextAppend(dstP, capacity, lengthP, text);
}

void UdpLogLine(UdpLog *logP, const Char *tagP, const Char *formatP,
                UInt32 a, UInt32 b, UInt32 c)
{
    UInt32 values[3];
    UInt32 ms;
    UInt16 next = 0, length, capacity;
    Char *lineP;
    Char text[2];
    if (logP == 0 || logP->length + LINE_CAPACITY > UDP_LOG_CAPACITY) return;
    values[0] = a;
    values[1] = b;
    values[2] = c;
    lineP = logP->buffer + logP->length;
    capacity = LINE_CAPACITY - 1;
    length = 0;
    lineP[0] = '\0';
    ms = (TimGetTicks() - logP->startTicks) * 1000UL / SysTicksPerSecond();
    TextAppendUInt32(lineP, capacity, &length, ms / 1000);
    TextAppend(lineP, capacity, &length, ".");
    TextAppend(lineP, capacity, &length, ms % 1000 < 100 ?
        (ms % 1000 < 10 ? "00" : "0") : "");
    TextAppendUInt32(lineP, capacity, &length, ms % 1000);
    TextAppend(lineP, capacity, &length, " ");
    TextAppend(lineP, capacity, &length, tagP);
    TextAppend(lineP, capacity, &length, " ");
    text[1] = '\0';
    for (; *formatP != '\0'; formatP++) {
        if ((*formatP == '%' || *formatP == '$') && next < 3) {
            if (*formatP == '%')
                TextAppendUInt32(lineP, capacity, &length, values[next++]);
            else
                AppendHex(lineP, capacity, &length, values[next++]);
        } else {
            text[0] = *formatP;
            TextAppend(lineP, capacity, &length, text);
        }
    }
    lineP[length++] = '\n';
    logP->length += length;
}

void UdpLogFlush(UdpLog *logP)
{
    if (logP == 0 || logP->length == 0) return;
    if (logP->pausedTicks != 0) {
        if (TimGetTicks() - logP->pausedTicks <
                (UInt32)PAUSE_SECONDS * SysTicksPerSecond())
            return;
        logP->pausedTicks = 0;
    }
    if (logP->length > UDP_LOG_CAPACITY / 2 ||
        TimGetTicks() - logP->sentTicks >= SysTicksPerSecond())
        Send(logP);
}
