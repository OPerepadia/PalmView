/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_UDP_LOG_H
#define PALMVIEW_UDP_LOG_H

#include <PalmOS.h>

/* A debug log sent over UDP to a PC on the same network, for example
 *   nc -klu 5140
 * Lines collect in memory and go out from UdpLogFlush, as one datagram, at
 * most once a second unless the buffer is filling up. Sending never waits;
 * a datagram the network cannot take is lost, and lines that do not fit
 * are dropped. Nothing is written to storage.
 *
 * Every call accepts a 0 log and does nothing. */

typedef struct UdpLog UdpLog;

/* destP is "a.b.c.d:port". NetLib must be open. Returns 0 if the address
 * is not usable or the socket did not open. */
UdpLog *UdpLogOpen(UInt16 netRefNum, const Char *destP);
/* Sends what is left and frees the log. */
void UdpLogClose(UdpLog *logP);
/* Opens a new socket once NetLib is open again after the system closed it.
 * The old one is not closed: a new socket may have its number now. */
void UdpLogReconnect(UdpLog *logP);

/* Adds "seconds.ms tag text" as a line. In formatP, each '%' becomes the
 * next of a, b and c in decimal, and each '$' the next one in hex. */
void UdpLogLine(UdpLog *logP, const Char *tagP, const Char *formatP,
                UInt32 a, UInt32 b, UInt32 c);
/* Sends the lines collected when it is time. Call it from the event loop,
 * not from inside other network calls. */
void UdpLogFlush(UdpLog *logP);

#endif
