/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Host stand-in for src/palm/stream_feed.h: what the player uses.
 * host/tests/playback_sim.c feeds the streams from files over a simulated
 * network. */

#ifndef PALMVIEW_STREAM_FEED_H
#define PALMVIEW_STREAM_FEED_H

#include <PalmOS.h>
#include "mp4_stream.h"

#define STREAM_FEED_VIDEO 0
#define STREAM_FEED_AUDIO 1

typedef struct StreamFeed StreamFeed;

UInt32 StreamFeedLength(const StreamFeed *f, UInt16 track);
void StreamFeedStart(StreamFeed *f, Mp4Stream *videoP, Mp4Stream *audioP,
                     Boolean headersFirst);
Boolean StreamFeedHeld(const StreamFeed *f);
void StreamFeedSeek(StreamFeed *f, UInt32 videoOffset, UInt32 audioOffset);
void StreamFeedStep(StreamFeed *f, Boolean playing);
void StreamFeedStop(StreamFeed *f);
const Char *StreamFeedError(const StreamFeed *f);
UInt32 StreamFeedReceived(const StreamFeed *f);

#endif
