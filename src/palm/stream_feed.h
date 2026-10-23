/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PALMVIEW_STREAM_FEED_H
#define PALMVIEW_STREAM_FEED_H

#include <PalmOS.h>
#include "mp4_stream.h"
#include "net_job.h"
#include "yt_api.h"

/* Downloads a video's two DASH streams (video, audio) over HTTPS into two
 * Mp4Streams while it plays. One request at a time, for the stream with
 * the emptier ring, asks for no more than fits. Each stream keeps its own
 * connection open between requests. A connection that drops starts again
 * where it stopped. Nothing is written to storage. */

#define STREAM_FEED_VIDEO 0
#define STREAM_FEED_AUDIO 1

typedef struct StreamFeed StreamFeed;

/* The formats must stay valid while the feed exists. Redirects rewrite
 * their URLs. Returns 0 if out of memory. */
StreamFeed *StreamFeedNew(const NetJobConfig *configP, const YtClient *clientP,
                          YtFormat *videoP, YtFormat *audioP);
void StreamFeedFree(StreamFeed *f);

/* The rest is what the player uses. */

UInt32 StreamFeedLength(const StreamFeed *f, UInt16 track);
/* Downloads from the start into the streams, opened with the lengths
 * above. */
void StreamFeedStart(StreamFeed *f, Mp4Stream *videoP, Mp4Stream *audioP);
/* Goes on from these offsets, where the streams have been moved to. A
 * request in flight is dropped with its connection; idle ones are kept. */
void StreamFeedSeek(StreamFeed *f, UInt32 videoOffset, UInt32 audioOffset);
/* Moves what has arrived into the rings. Call it often. */
void StreamFeedStep(StreamFeed *f);
/* Closes the connections. */
void StreamFeedStop(StreamFeed *f);
/* 0 while streaming can go on, or why it cannot. */
const Char *StreamFeedError(const StreamFeed *f);
/* Bytes received so far */
UInt32 StreamFeedReceived(const StreamFeed *f);
/* How the last failed connection ended, or 0 if none has. */
const NetJobResult *StreamFeedFailure(const StreamFeed *f);
/* The failure that began the last run of retries: what dropped the
 * stream. */
const NetJobResult *StreamFeedDrop(const StreamFeed *f);

#endif
