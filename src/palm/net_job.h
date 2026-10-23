/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_NET_JOB_H
#define PALMVIEW_NET_JOB_H

#include <PalmOS.h>
#include "http.h"
#include "udp_log.h"

/* One HTTPS request/response, advanced a step at a time from the event
 * loop so the UI stays responsive. Based on PalmTLS's TLS Tester client, but
 * it sends a caller-built request (any method, any URL length). */

/* The most body bytes one step passes on. */
#define NET_JOB_IO_CAPACITY 4096

typedef enum NetJobStatus {
    netJobOk = 0,
    netJobCancelled,
    netJobNetworkError,
    netJobTlsError,
    netJobHttpError,
    netJobSinkError
} NetJobStatus;

typedef enum NetJobProgress {
    netJobRunning = 0,
    netJobFinished,
    netJobIdle          /* with keepAlive: done, the connection kept open */
} NetJobProgress;

typedef enum NetJobPhase {
    netPhaseDns = 0,
    netPhaseConnect,
    netPhaseTls,
    netPhaseSend,
    netPhaseReceive
} NetJobPhase;

typedef struct NetJobConfig {
    UInt16 netRefNum;
    UInt16 tlsRefNum;
    UInt16 tlsProtocol;
    const UInt8 *trustP;
    UInt32 trustLength;
    Int32 stepTimeoutTicks;
    Int32 operationTimeoutTicks;
    /* Steps after the DNS lookup do not wait for the network: the socket
     * stays non-blocking and is checked first. For jobs that run during
     * playback. */
    Boolean poll;
    /* How long a response may send nothing before the job fails; 0 for
     * operationTimeoutTicks. */
    Int32 idleTimeoutTicks;
    /* A successful response that leaves the connection reusable makes the
     * job idle instead of finishing it; NetJobSend sends the next request. */
    Boolean keepAlive;
    UdpLog *logP;               /* 0 for no log */
} NetJobConfig;

typedef struct NetJobResult {
    UInt16 status;
    UInt16 phase;
    UInt16 httpStatus;
    UInt16 tlsStatus;
    Int16 tlsError;
    UInt16 netError;
    UInt16 platformError;
    UInt32 received;
    UInt32 handshakeTicks;
    Boolean redirected;     /* a 3xx answer; see NetJobKeepLocation */
    Boolean reused;         /* sent on a connection kept open */
} NetJobResult;

typedef struct NetJob NetJob;

/* hostP and requestP must stay valid until the job finishes. The body is
 * passed to bodyProcP only for 2xx responses; a non-zero return from it
 * stops the job with netJobSinkError. Returns false if the job could not
 * start (resultP says why). */
Boolean NetJobStart(NetJob **jobPP, const NetJobConfig *configP,
                    const Char *hostP, const Char *requestP,
                    UInt16 requestLength, HttpBodyProc bodyProcP,
                    void *bodyContextP, NetJobResult *resultP);
/* Returns netJobFinished once the job is done and freed, or netJobIdle
 * while it waits for NetJobSend. */
UInt16 NetJobStep(NetJob **jobPP);
/* Sends the next request on an idle job's connection, with the same body
 * procedure and result. False if the job is not idle. */
Boolean NetJobSend(NetJob *jobP, const Char *requestP, UInt16 requestLength);
/* True once the response is being read: each step may then pass up to
 * NET_JOB_IO_CAPACITY body bytes on. */
Boolean NetJobReceiving(const NetJob *jobP);
/* Names the job in the log; tagP must stay valid. */
void NetJobSetTag(NetJob *jobP, const Char *tagP);
/* Where a redirect's Location goes, in full. Left empty if it does not
 * fit. */
void NetJobKeepLocation(NetJob *jobP, Char *bufferP, UInt16 capacity);
void NetJobCancel(NetJob *jobP);

#endif
