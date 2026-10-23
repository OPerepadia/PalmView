/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The connection steps follow PalmTLS's apps/tls-tester/src/download_client.c,
 * which is tested on real devices. */

#include "net_job.h"
#include "palm_tls.h"

/* A polling job still tries TLS this often when the socket looks idle:
 * TLS may hold received data the socket no longer shows. */
#define POLL_RETRY_DIVISOR 4
/* Steps that take longer than this are logged. */
#define SLOW_STEP_MS 100

/* Log lines for the phase just finished, by NetJobPhase */
static const Char kPhaseLines[5][16] = {
    "dns % ms", "connect % ms", "tls % ms", "send % ms", "receive % ms"
};

enum {
    jobStateDns = 0,
    jobStateSocket,
    jobStateConnectStart,
    jobStateConnectWait,
    jobStateTlsOpen,
    jobStateTlsHandshake,
    jobStateSend,
    jobStateRead
};

struct NetJob {
    NetJobConfig config;
    NetJobResult *resultP;
    const Char *hostP;
    const Char *requestP;
    UInt16 requestLength;
    UInt16 sent;
    HttpBodyProc bodyProcP;
    void *bodyContextP;
    HttpParser parser;
    NetHostInfoBufType hostBuffer;
    NetSocketAddrINType address;
    UInt16 state;
    NetSocketRef socket;
    UInt32 sessionId;
    Boolean tlsOpen;
    Boolean cancelled;
    UInt32 phaseStart;
    UInt32 idleStart;
    UInt32 lastRead;        /* when the caller last asked for data */
    UInt32 lastTry;
    Boolean bufferFilled;       /* TLS may have more right away */
    Char *locationP;
    UInt16 locationCapacity;
    const Char *tagP;
    UInt8 buffer[NET_JOB_IO_CAPACITY];
};

static Boolean TimedOut(UInt32 startTicks, Int32 timeoutTicks)
{
    return TimGetTicks() - startTicks >= (UInt32)timeoutTicks;
}

static UInt32 TicksToMs(UInt32 ticks)
{
    return ticks * 1000UL / SysTicksPerSecond();
}

static void SetPhase(NetJob *jobP, UInt16 phase)
{
    if (jobP->state != jobStateDns)
        UdpLogLine(jobP->config.logP, jobP->tagP,
            kPhaseLines[jobP->resultP->phase],
            TicksToMs(TimGetTicks() - jobP->phaseStart), 0, 0);
    jobP->resultP->phase = phase;
    jobP->phaseStart = TimGetTicks();
}

/* PalmTLS needs at least a tick. A polling job's socket does not wait for
 * it. */
/* NetLib reports a non-blocking connect still under way in several ways.
 * The Palm TX says netErrSocketBusy (EINPROGRESS). */
static Boolean Connecting(Err error)
{
    return error == netErrWouldBlock || error == netErrAlreadyInProgress ||
        error == netErrSocketBusy || error == netErrTimeout;
}

static Int32 IoTimeout(const NetJob *jobP)
{
    return jobP->config.poll ? 1 : jobP->config.stepTimeoutTicks;
}

/* For polling jobs: false if TLS has nothing to work on yet. */
static Boolean ShouldTry(NetJob *jobP)
{
    NetFDSetType readSet;
    Err error = errNone;
    UInt32 now = TimGetTicks();
    if (!jobP->config.poll || jobP->bufferFilled) return true;
    netFDZero(&readSet);
    netFDSet(jobP->socket, &readSet);
    if (NetLibSelect(jobP->config.netRefNum, jobP->socket + 1, &readSet, 0, 0,
            0, &error) > 0 ||
        now - jobP->lastTry >= SysTicksPerSecond() / POLL_RETRY_DIVISOR) {
        jobP->lastTry = now;
        return true;
    }
    return false;
}

/* Forwards successful response bodies to the caller. */
static Int16 ForwardBody(void *contextP, const UInt8 *dataP, UInt16 length)
{
    NetJob *jobP = (NetJob *)contextP;
    UInt16 status = jobP->parser.statusCode;
    if (status < 200 || status > 299) return 0;
    jobP->resultP->received += length;
    return jobP->bodyProcP(jobP->bodyContextP, dataP, length);
}

static void CloseTls(NetJob *jobP, Boolean cancel)
{
    PalmTlsSessionIoResult result;
    MemSet(&result, sizeof(result), 0);
    result.structSize = sizeof(result);
    if (cancel)
        PalmTlsLibSessionCancel(jobP->config.tlsRefNum, jobP->sessionId,
            &result);
    else
        PalmTlsLibSessionClose(jobP->config.tlsRefNum, jobP->sessionId,
            &result);
}

static UInt16 Finish(NetJob **jobPP)
{
    NetJob *jobP = *jobPP;
    NetJobResult *r = jobP->resultP;
    Err error = errNone;
    Boolean cancel = r->status != netJobOk;
    UdpLogLine(jobP->config.logP, jobP->tagP, "end % http % got %", r->status,
        r->httpStatus, r->received);
    if (r->status != netJobOk && r->status != netJobCancelled)
        UdpLogLine(jobP->config.logP, jobP->tagP, "fail net $ tls %/-%",
            r->netError, r->tlsStatus,
            r->tlsError < 0 ? (UInt32)-r->tlsError : (UInt32)r->tlsError);
    if (jobP->tlsOpen) CloseTls(jobP, cancel);
    else if (jobP->sessionId != 0) CloseTls(jobP, true);
    if (jobP->socket >= 0)
        NetLibSocketClose(jobP->config.netRefNum, jobP->socket,
            jobP->config.stepTimeoutTicks, &error);
    MemPtrFree(jobP);
    *jobPP = 0;
    return netJobFinished;
}

static UInt16 NetworkFailure(NetJob **jobPP, Err error)
{
    (*jobPP)->resultP->status = netJobNetworkError;
    (*jobPP)->resultP->netError = error != errNone ? error : netErrTimeout;
    return Finish(jobPP);
}

static UInt16 TlsFailure(NetJob **jobPP, Err error, UInt16 tlsStatus,
                         Int16 tlsError, UInt16 netError)
{
    NetJobResult *resultP = (*jobPP)->resultP;
    resultP->status = netJobTlsError;
    resultP->platformError = error;
    resultP->tlsStatus = tlsStatus;
    resultP->tlsError = tlsError;
    resultP->netError = netError;
    return Finish(jobPP);
}

Boolean NetJobStart(NetJob **jobPP, const NetJobConfig *configP,
                    const Char *hostP, const Char *requestP,
                    UInt16 requestLength, HttpBodyProc bodyProcP,
                    void *bodyContextP, NetJobResult *resultP)
{
    NetJob *jobP;
    MemSet(resultP, sizeof(*resultP), 0);
    *jobPP = 0;
    jobP = (NetJob *)MemPtrNew(sizeof(NetJob));
    if (jobP == 0) {
        resultP->status = netJobNetworkError;
        resultP->platformError = memErrNotEnoughSpace;
        return false;
    }
    MemSet(jobP, sizeof(*jobP), 0);
    jobP->config = *configP;
    jobP->resultP = resultP;
    jobP->hostP = hostP;
    jobP->requestP = requestP;
    jobP->requestLength = requestLength;
    jobP->bodyProcP = bodyProcP;
    jobP->bodyContextP = bodyContextP;
    jobP->socket = -1;
    jobP->tagP = "net";
    jobP->state = jobStateDns;
    SetPhase(jobP, netPhaseDns);
    *jobPP = jobP;
    return true;
}

void NetJobCancel(NetJob *jobP)
{
    if (jobP != 0) jobP->cancelled = true;
}

Boolean NetJobReceiving(const NetJob *jobP)
{
    return jobP != 0 && jobP->state == jobStateRead;
}

void NetJobSetTag(NetJob *jobP, const Char *tagP)
{
    jobP->tagP = tagP;
}

void NetJobKeepLocation(NetJob *jobP, Char *bufferP, UInt16 capacity)
{
    jobP->locationP = bufferP;
    jobP->locationCapacity = capacity;
    if (capacity != 0) bufferP[0] = '\0';
}

/* The parser keeps only the start of Location; YouTube's are long. */
static void KeepLocation(NetJob *jobP)
{
    const Char *p = jobP->parser.headers;
    if (jobP->locationP == 0) return;
    while ((p = StrChr(p, '\n')) != 0) {
        const Char *valueP, *endP;
        p++;
        if (StrNCaselessCompare(p, "Location:", 9) != 0) continue;
        for (valueP = p + 9; *valueP == ' ' || *valueP == '\t'; valueP++) {}
        for (endP = valueP; *endP != '\0' && *endP != '\r' && *endP != '\n';
             endP++) {}
        if (endP - valueP < jobP->locationCapacity) {
            MemMove(jobP->locationP, valueP, endP - valueP);
            jobP->locationP[endP - valueP] = '\0';
        }
        return;
    }
}

static UInt16 StepConnected(NetJob **jobPP)
{
    NetJob *jobP = *jobPP;
    Int16 disabled = 0;
    Err error;
    /* A polling job's TLS reads then return at once without data, rather
     * than wait for the rest of a record. */
    if (!jobP->config.poll)
        NetLibSocketOptionSet(jobP->config.netRefNum, jobP->socket,
            netSocketOptLevelSocket, netSocketOptSockNonBlocking, &disabled,
            sizeof(disabled), jobP->config.stepTimeoutTicks, &error);
    jobP->state = jobStateTlsOpen;
    SetPhase(jobP, netPhaseTls);
    return netJobRunning;
}

static UInt16 StepRead(NetJob **jobPP)
{
    NetJob *jobP = *jobPP;
    NetJobResult *resultP = jobP->resultP;
    PalmTlsSessionIoParams params;
    PalmTlsSessionIoResult tlsResult;
    UInt16 received, offset = 0;
    UInt32 now = TimGetTicks();
    Err error;
    /* While the caller's buffer was full it did not ask: that is not idle. */
    if (now - jobP->lastRead > SysTicksPerSecond()) jobP->idleStart = now;
    jobP->lastRead = now;
    if (!ShouldTry(jobP)) {
        if (TimedOut(jobP->idleStart, jobP->config.operationTimeoutTicks))
            return NetworkFailure(jobPP, netErrTimeout);
        return netJobRunning;
    }
    MemSet(&params, sizeof(params), 0);
    MemSet(&tlsResult, sizeof(tlsResult), 0);
    params.structSize = sizeof(params);
    params.sessionId = jobP->sessionId;
    params.bufferP = jobP->buffer;
    params.length = NET_JOB_IO_CAPACITY;
    params.timeoutTicks = IoTimeout(jobP);
    params.options = PALM_TLS_IO_COOPERATIVE;
    tlsResult.structSize = sizeof(tlsResult);
    error = PalmTlsLibSessionRead(jobP->config.tlsRefNum, &params, &tlsResult);
    if (error != errNone || (tlsResult.status != palmTlsStatusOk &&
                             tlsResult.status != palmTlsStatusWouldBlock))
        return TlsFailure(jobPP, error, tlsResult.status, tlsResult.tlsError,
            tlsResult.netError);
    received = (UInt16)tlsResult.transferred;
    jobP->bufferFilled = received == NET_JOB_IO_CAPACITY;
    if (received != 0) {
        jobP->idleStart = TimGetTicks();
        /* Headers byte by byte, as in the tester, then the body in bulk. */
        while (offset < received && !jobP->parser.headersComplete) {
            if (HttpParserFeed(&jobP->parser, jobP->buffer + offset, 1) !=
                    httpOk) {
                resultP->status = netJobHttpError;
                resultP->platformError = jobP->parser.parseStatus;
                return Finish(jobPP);
            }
            offset++;
            if (jobP->parser.headersComplete) {
                resultP->httpStatus = jobP->parser.statusCode;
                if (jobP->parser.redirect) {
                    KeepLocation(jobP);
                    resultP->redirected = true;
                    resultP->status = netJobHttpError;
                    return Finish(jobPP);
                }
            }
        }
        if (offset < received &&
            HttpParserFeed(&jobP->parser, jobP->buffer + offset,
                received - offset) != httpOk) {
            resultP->status = jobP->parser.parseStatus == httpSinkFailed
                ? netJobSinkError : netJobHttpError;
            resultP->platformError = jobP->parser.parseStatus;
            return Finish(jobPP);
        }
    }
    if (jobP->parser.connectionComplete ||
        (received == 0 && tlsResult.status == palmTlsStatusOk)) {
        if (!jobP->parser.connectionComplete &&
            HttpParserFinish(&jobP->parser) != httpOk) {
            resultP->status = netJobHttpError;
            resultP->platformError = jobP->parser.parseStatus;
            return Finish(jobPP);
        }
        resultP->httpStatus = jobP->parser.statusCode;
        resultP->status = resultP->httpStatus >= 200 &&
            resultP->httpStatus <= 299 ? netJobOk : netJobHttpError;
        return Finish(jobPP);
    }
    if (TimedOut(jobP->idleStart, jobP->config.operationTimeoutTicks))
        return NetworkFailure(jobPP, netErrTimeout);
    return netJobRunning;
}

static UInt16 Step(NetJob **jobPP)
{
    NetJob *jobP;
    Err error = errNone;
    jobP = *jobPP;
    EvtResetAutoOffTimer();
    if (jobP->cancelled) {
        jobP->resultP->status = netJobCancelled;
        return Finish(jobPP);
    }
    switch (jobP->state) {
        case jobStateDns: {
            NetHostInfoPtr hostInfoP = NetLibGetHostByName(
                jobP->config.netRefNum, jobP->hostP, &jobP->hostBuffer,
                jobP->config.stepTimeoutTicks, &error);
            if (hostInfoP != 0 && hostInfoP->addrListP != 0 &&
                hostInfoP->addrListP[0] != 0) {
                MemSet(&jobP->address, sizeof(jobP->address), 0);
                jobP->address.family = netSocketAddrINET;
                jobP->address.port = NetHToNS(443);
                MemMove(&jobP->address.addr, hostInfoP->addrListP[0],
                    sizeof(jobP->address.addr));
                jobP->state = jobStateSocket;
                return netJobRunning;
            }
            if ((error == netErrTimeout || error == netErrWouldBlock) &&
                !TimedOut(jobP->phaseStart, jobP->config.operationTimeoutTicks))
                return netJobRunning;
            return NetworkFailure(jobPP, error);
        }
        case jobStateSocket:
            jobP->socket = NetLibSocketOpen(jobP->config.netRefNum,
                netSocketAddrINET, netSocketTypeStream, 0,
                jobP->config.stepTimeoutTicks, &error);
            if (jobP->socket < 0) return NetworkFailure(jobPP, error);
            jobP->state = jobStateConnectStart;
            SetPhase(jobP, netPhaseConnect);
            return netJobRunning;
        case jobStateConnectStart: {
            Int16 enabled = 1;
            if (NetLibSocketOptionSet(jobP->config.netRefNum, jobP->socket,
                    netSocketOptLevelSocket, netSocketOptSockNonBlocking,
                    &enabled, sizeof(enabled), jobP->config.stepTimeoutTicks,
                    &error) < 0)
                return NetworkFailure(jobPP, error);
            if (NetLibSocketConnect(jobP->config.netRefNum, jobP->socket,
                    (NetSocketAddrType *)&jobP->address,
                    sizeof(jobP->address), jobP->config.stepTimeoutTicks,
                    &error) == 0)
                return StepConnected(jobPP);
            if (!Connecting(error)) return NetworkFailure(jobPP, error);
            jobP->state = jobStateConnectWait;
            return netJobRunning;
        }
        case jobStateConnectWait: {
            NetFDSetType writeSet, exceptSet;
            Int16 selected;
            netFDZero(&writeSet);
            netFDZero(&exceptSet);
            netFDSet(jobP->socket, &writeSet);
            netFDSet(jobP->socket, &exceptSet);
            selected = NetLibSelect(jobP->config.netRefNum, jobP->socket + 1,
                0, &writeSet, &exceptSet,
                jobP->config.poll ? 0 : jobP->config.stepTimeoutTicks, &error);
            if (selected > 0) {
                Err socketError = errNone, optionError = errNone;
                UInt16 errorSize = sizeof(socketError);
                NetSocketAddrINType local, remote;
                Int16 localSize = sizeof(local), remoteSize = sizeof(remote);
                if (NetLibSocketOptionGet(jobP->config.netRefNum,
                        jobP->socket, netSocketOptLevelSocket,
                        netSocketOptSockErrorStatus, &socketError,
                        &errorSize, jobP->config.stepTimeoutTicks,
                        &optionError) < 0 || socketError != errNone)
                    return NetworkFailure(jobPP, optionError != errNone
                        ? optionError : socketError);
                /* NetLib may call a socket writable before it is connected;
                 * TLS must wait until it has a peer. */
                MemSet(&remote, sizeof(remote), 0);
                if (NetLibSocketAddr(jobP->config.netRefNum, jobP->socket,
                        (NetSocketAddrType *)&local, &localSize,
                        (NetSocketAddrType *)&remote, &remoteSize,
                        jobP->config.stepTimeoutTicks, &optionError) >= 0 &&
                    remote.port != 0)
                    return StepConnected(jobPP);
                if (optionError != errNone &&
                    optionError != netErrSocketNotConnected &&
                    !Connecting(optionError))
                    return NetworkFailure(jobPP, optionError);
                if (TimedOut(jobP->phaseStart, jobP->config.operationTimeoutTicks))
                    return NetworkFailure(jobPP, netErrTimeout);
                return netJobRunning;
            }
            if ((selected == 0 || Connecting(error)) &&
                !TimedOut(jobP->phaseStart, jobP->config.operationTimeoutTicks))
                return netJobRunning;
            return NetworkFailure(jobPP, error);
        }
        case jobStateTlsOpen: {
            PalmTlsSessionOpenParams params;
            PalmTlsSessionOpenResult tlsResult;
            MemSet(&params, sizeof(params), 0);
            MemSet(&tlsResult, sizeof(tlsResult), 0);
            params.structSize = sizeof(params);
            params.netRefNum = jobP->config.netRefNum;
            params.socket = jobP->socket;
            params.verifyMode = palmTlsVerifyCaStore;
            params.protocol = jobP->config.tlsProtocol;
            params.hostnameP = jobP->hostP;
            params.trustedPeerP = jobP->config.trustP;
            params.trustedPeerLength = jobP->config.trustLength;
            params.timeoutTicks = IoTimeout(jobP);
            params.options = PALM_TLS_SESSION_ALLOW_RESUME |
                PALM_TLS_SESSION_COOPERATIVE;
            tlsResult.structSize = sizeof(tlsResult);
            error = PalmTlsLibSessionOpen(jobP->config.tlsRefNum, &params,
                &tlsResult);
            if (error != errNone)
                return TlsFailure(jobPP, error, 0, 0, 0);
            jobP->sessionId = tlsResult.sessionId;
            if (tlsResult.status == palmTlsStatusWouldBlock) {
                jobP->state = jobStateTlsHandshake;
                return netJobRunning;
            }
            if (tlsResult.status != palmTlsStatusOk)
                return TlsFailure(jobPP, errNone, tlsResult.status,
                    tlsResult.tlsError, tlsResult.netError);
            jobP->tlsOpen = true;
            jobP->resultP->handshakeTicks = tlsResult.handshakeTicks;
            jobP->state = jobStateSend;
            SetPhase(jobP, netPhaseSend);
            return netJobRunning;
        }
        case jobStateTlsHandshake: {
            PalmTlsSessionIoParams params;
            PalmTlsSessionOpenResult tlsResult;
            if (!ShouldTry(jobP)) {
                if (TimedOut(jobP->phaseStart, jobP->config.operationTimeoutTicks))
                    return TlsFailure(jobPP, errNone,
                        palmTlsStatusHandshakeFailed, 0, netErrTimeout);
                return netJobRunning;
            }
            MemSet(&params, sizeof(params), 0);
            MemSet(&tlsResult, sizeof(tlsResult), 0);
            params.structSize = sizeof(params);
            params.sessionId = jobP->sessionId;
            params.timeoutTicks = IoTimeout(jobP);
            params.options = PALM_TLS_IO_COOPERATIVE;
            tlsResult.structSize = sizeof(tlsResult);
            error = PalmTlsLibSessionHandshake(jobP->config.tlsRefNum,
                &params, &tlsResult);
            if (error != errNone || (tlsResult.status != palmTlsStatusOk &&
                    tlsResult.status != palmTlsStatusWouldBlock))
                return TlsFailure(jobPP, error, tlsResult.status,
                    tlsResult.tlsError, tlsResult.netError);
            if (tlsResult.status == palmTlsStatusOk) {
                jobP->tlsOpen = true;
                jobP->resultP->handshakeTicks = tlsResult.handshakeTicks;
                jobP->state = jobStateSend;
                SetPhase(jobP, netPhaseSend);
            } else if (TimedOut(jobP->phaseStart,
                           jobP->config.operationTimeoutTicks)) {
                return TlsFailure(jobPP, errNone, palmTlsStatusHandshakeFailed,
                    0, netErrTimeout);
            }
            return netJobRunning;
        }
        case jobStateSend: {
            PalmTlsSessionIoParams params;
            PalmTlsSessionIoResult tlsResult;
            MemSet(&params, sizeof(params), 0);
            MemSet(&tlsResult, sizeof(tlsResult), 0);
            params.structSize = sizeof(params);
            params.sessionId = jobP->sessionId;
            params.bufferP = (void *)(jobP->requestP + jobP->sent);
            params.length = jobP->requestLength - jobP->sent;
            params.timeoutTicks = IoTimeout(jobP);
            params.options = PALM_TLS_IO_COOPERATIVE;
            tlsResult.structSize = sizeof(tlsResult);
            error = PalmTlsLibSessionWrite(jobP->config.tlsRefNum, &params,
                &tlsResult);
            jobP->sent += (UInt16)tlsResult.transferred;
            if (error != errNone || (tlsResult.status != palmTlsStatusOk &&
                    tlsResult.status != palmTlsStatusWouldBlock))
                return TlsFailure(jobPP, error, tlsResult.status,
                    tlsResult.tlsError, tlsResult.netError);
            if (jobP->sent == jobP->requestLength) {
                HttpParserInit(&jobP->parser, 0, ForwardBody, jobP);
                jobP->state = jobStateRead;
                jobP->idleStart = TimGetTicks();
                jobP->lastRead = jobP->idleStart;
                SetPhase(jobP, netPhaseReceive);
            } else if (TimedOut(jobP->phaseStart,
                           jobP->config.operationTimeoutTicks)) {
                return NetworkFailure(jobPP, netErrTimeout);
            }
            return netJobRunning;
        }
        case jobStateRead:
            return StepRead(jobPP);
        default:
            jobP->resultP->status = netJobNetworkError;
            jobP->resultP->platformError = sysErrParamErr;
            return Finish(jobPP);
    }
}

UInt16 NetJobStep(NetJob **jobPP)
{
    UdpLog *logP;
    const Char *tagP;
    UInt16 state;
    UInt32 start, ms;
    UInt16 progress;
    if (jobPP == 0 || *jobPP == 0) return netJobFinished;
    logP = (*jobPP)->config.logP;
    tagP = (*jobPP)->tagP;
    state = (*jobPP)->state;
    start = TimGetTicks();
    progress = Step(jobPP);
    ms = TicksToMs(TimGetTicks() - start);
    if (ms >= SLOW_STEP_MS)
        UdpLogLine(logP, tagP, "slow % ms in state %", ms, state, 0);
    return progress;
}
