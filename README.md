# PalmView

YouTube for the Palm TX, with no helper server. PalmView searches YouTube
and plays a video's 144p or 240p H.264 and AAC streams with sound. It can
stream them over HTTPS, or download and merge them into one MP4 on the
memory card. You can also play saved files in CorePlayer.

Status: downloading, playback with sound, and streaming work on a Palm TX.

## How it works

1. Search and video info use YouTube's own API (`/youtubei/v1/search` and
   `/player`), posing as the visionOS app. That client currently gets direct
   stream URLs, with no JavaScript challenge and no PO token.
2. The app downloads itag 160 (256x144 H.264), or itag 133 (426x240) for
   240p, and itag 140 (AAC-LC) in 4 MB ranges. HTTPS comes from
   [PalmTLS](https://github.com/palmsnipe/palm-tls).
3. YouTube serves the two streams as separate fragmented MP4 files.
   `src/core/mp4_merge.c` rebuilds them into one regular MP4, with the moov
   box first. Its packets match `ffmpeg -c copy` output exactly.
4. Streaming reads both files at once into RAM while they play. Nothing is
   written to the card or to internal storage.

## Install on the Palm

You need Palm OS 5, Wi-Fi and an SD card. On the TX:

1. Make sure Wi-Fi works. The TX only speaks 802.11b; for WPA2, install the
   community "Palm TX WPA2 Enterprise Security Upgrade".
2. Install `dist/PalmTLSArm.prc` into handheld memory, not the card. Shared
   libraries cannot load from the card.
3. Install `dist/PalmView.prc` into handheld memory. It is 1.4 MB, most of
   it the video decoder.
4. Optionally, install CorePlayer.

## Use

1. Type a search and tap **Search** (or write a return stroke). The ▼ next
   to the search field lists your last 8 searches; pick one to search again.
   **Options > Clear Search History** forgets them.
2. Each result shows its thumbnail with the duration, title, views, age and
   channel.
   Thumbnails load after the results, those in view first. Tap a result, or
   move to it with 5-way up and down and press the centre, and PalmView
   fetches the stream info.
3. Tap **Play** to stream it, or **Download** to save it. Download progress
   shows the speed and TLS handshake time.
4. A downloaded video is saved as `/PalmView/<title>.mp4` on the card. Tap
   **Play** to watch it, or open it in CorePlayer.

**Saved** on the main screen lists the videos on the card. Tap one to select
it, then tap **Play** or **Delete**. In the player, tap **Pause** or the
5-way centre to pause.

While streaming, the bar under the video shows the position in red and
what has downloaded in dark gray. Tap or drag on it to jump there, or
press left and right on the 5-way to jump about 10 seconds. Playback goes
on from the start of the piece of video (about 5 seconds) that holds that
time, after a few seconds of loading.

Press up and down on the 5-way navigator to change the volume. PalmView
remembers it.

When YouTube offers both, the trigger at the top right of the player
switches between 144p and 240p. Playback goes on from the start of the
piece of video that holds the current time, as when jumping. PalmView
remembers the choice for the next video and for downloads. 240p needs an
overclocked TX; see [240p](#240p).

Tap the picture for full screen, and hold the TX on its side with the
5-way on the right, as in its landscape mode. 240p shows at its own size,
426x240; 144p is scaled up to 480x270. The 5-way turns with the picture:
as you hold it, left and right jump, and up and down change the volume.
Tap anywhere to go back. See [Full screen](#full-screen).

**Cancel** stops a download; tap **Download** again to resume it. Partial
streams stay in `/PalmView` as `<id>.video.mp4` and `<id>.audio.mp4` until
the merge succeeds.

## When YouTube changes

YouTube regularly breaks the clients that tools like this use. If videos stop
working, put a `client.txt` file in `/PalmView` on the card with new values.
Unknown keys and lines starting with `#` are ignored:

```
name=VISIONOS
version=1.02
nameId=101
userAgent=Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15
deviceMake=Apple
deviceModel=RealityDevice17,1
osName=visionOS
osVersion=26.5.23O471
```

Good values come from yt-dlp's `INNERTUBE_CLIENTS` in
`yt_dlp/extractor/youtube/_base.py`. Pick a client with
`'REQUIRE_JS_PLAYER': False` and no required GVS PO token. `nameId` is its
`INNERTUBE_CONTEXT_CLIENT_NAME`.

If YouTube says "Sign in to confirm you're not a bot", try again, or use
**Options > Reset YouTube ID**.

## Limitations

- 144p and 240p only, and only videos that offer H.264 and AAC streams.
- No live streams, "made for kids", age-restricted or members-only videos.
- Titles are converted to the Palm's Latin-1 text; Cyrillic is
  transliterated. With CJKOS on, set to Shift-JIS, titles and searches are
  in Japanese instead.
- Merging runs in one go and cannot be cancelled.
- The built-in player cannot seek yet.

## Player

`src/arm/` holds a native ARM module with FFmpeg's H.264 and fixed-point AAC
decoders. `src/palm/playback.c` drives it:

- The decoder keeps up to three frames ready, decoding ahead while there is
  time before the next one is due.
- Each frame is converted to RGB565 straight into the screen's framebuffer,
  at its presentation time. 144p grows to 320x180 by area: each screen
  pixel takes the video pixels it overlaps, in proportion. Edges stay
  sharp, and every video pixel gets the same width, which nearest-pixel
  scaling cannot give at 1.25 times. 240p is shrunk to 319x180 (see
  [240p](#240p)).
- Sound is decoded into a PCM ring of 0.74 s. A native ARM callback, run
  by the Palm OS sound stream on its own thread, copies it out. The frames
  it has played are the clock the pictures follow. A small assembly entry
  (`SoundCallbackEntry` in `src/arm/entry.S`) loads the module's GOT, since
  the OS calls in with its own `r9`.
- Frames more than 120 ms late are not drawn. A second is late when more
  than 10% of its frames, and at least 3, are late. After a late second
  with decoding keeping the CPU at least 80% busy, quality drops a level:
  first no deblocking in B-frames ("fast B"), then no deblocking ("fast").
  Dropping further, to no B-frames (half the frame rate), takes two such
  seconds in a row, since half rate looks much worse than an odd late
  second. Late frames with time to spare come from stalls that lower
  quality would not fix. Quality comes back after three calm seconds: under
  70% CPU and not late. If it drops again soon, it waits twice as long, up
  to 12 seconds. More than 1.5 s behind, video skips to the next keyframe.

The status line shows the frame rate, CPU use, skipped frames, quality,
sound gaps (the ring ran dry), and while streaming the download speed. The
network log below has more.

### Streaming

- `src/palm/stream_feed.c` keeps one HTTPS connection per stream, open
  between requests. Only one request is in flight at a time, for the stream
  whose ring is emptier, and it asks for no more than that ring has room
  for (at most 128 KB). A stream asks again once a quarter of its ring is
  free. So every byte that arrives can be read at once. On the TX, data left
  unread on one connection starves the other: neither its data nor a new
  handshake gets through. Keeping connections open matters too: each new
  TLS handshake blocks the player for about 2 s.
- While a stream has at least half its ring buffered, its response is read
  at a quarter above both streams' bitrate together, and at least 32 KB/s:
  32 KB/s at 144p and about 45 KB/s at 240p. A response comes at up to
  200 KB/s, and each 16 KB TLS record is decrypted in one go: read that
  fast, too many frames come late. Below half it is read at twice that
  while video plays, as after a seek, and at full speed while nothing
  plays, as at the start.
- A response that sends nothing for 5 s is asked for again on a new
  connection. A dropped connection starts again where it stopped. Redirects
  to another YouTube server are followed, for downloads too.
- The two rings share up to 512 KB of the heap, split by bitrate. At 144p
  (about 26 KB/s for both streams) that is about 20 seconds. At 240p the
  decoder's second block leaves less, about 10 seconds.
- `src/core/mp4_stream.c` takes each sample out of its ring once all of it
  has arrived. Only the current moof is kept besides the ring.
- Playback starts with 3 seconds of both streams in. When either runs dry,
  the clock stops ("Buffering") until both have 5 seconds again.
- Seeking uses the segment index (`sidx`) at the start of each DASH file.
  Video goes back to the start of the segment holding the time asked for,
  which begins with a keyframe, and sound to the segment holding that
  point. Sound before it is downloaded and dropped: YouTube's sound
  segments are 10 s long. The request in flight, if any, is dropped with
  its connection; idle connections are reused. The decoders then start
  again as they do at the start.
- Switching quality streams the other video from the same point. The new
  streams first ask only for their moov and segment index (where YouTube's
  `indexRange` says it ends), then seek, so no connection is dropped.
- Network calls never wait during playback. The sockets stay non-blocking,
  so TLS does not wait for the rest of a 16 KB record, and a socket is only
  read when it has data. The network gets a turn at least every 20 ms.
- Smooth streaming needs PalmTLS's faster ARM AES-GCM. It is not merged
  yet, so `tools/fetch-deps.sh` fetches PalmTLS from the `faster-aes-gcm`
  branch of [a fork](https://github.com/OPerepadia/palm-tls). With PalmTLS
  0.1.2, each 16 KB record takes about 100 ms to decrypt, and video drops
  to half rate.

### Network log

For debugging, PalmView can send a log of network events over UDP to a
computer on the same network. Add a line to `client.txt` on the card with
the computer's address and a port:

```
log=192.168.2.10:5140
```

Then listen on that computer, for example with `nc -klu 5140`. Each
connection's phases and their times, how it ended, slow steps (over
100 ms), retries, and a playback summary each second arrive as text lines:

```
41.230 v get 1048576-5242879 of 2600000
41.410 v dns 20 ms
41.520 v connect 110 ms
42.880 v tls 1360 ms
45.100 play at 30120 ms, buffer 17800 + 18000 ms
```

Lines go out at most once a second, as one datagram. Without a `log=` line
nothing is sent.

On a Palm TX, a 25 fps 144p video takes about 25 ms per frame to decode at
full quality and 19 ms without deblocking (measured with PalmView Bench).

### 240p

240p has 2.8 times the pixels of 144p. PalmView plays it at the lowest
quality level only: without deblocking, and without the frames nothing
else refers to. That is about half the frames, 12 fps for a 25 fps video.

At the stock 312 MHz, a TX cannot keep up even then, and busy scenes drop
to a few frames a second. Most of the decoding time goes to waiting for
memory, which runs off the bus clock, so raising only the CPU clock helps
little. With the warpSpeed overclocker at BUS 20 and CPU:BUS 4 (bus
260 MHz, CPU 520 MHz), 240p plays at its full 12 fps with the CPU about
55% busy. Showing every frame would need about twice that.

Measured on a TX with PalmView Bench (Rick Astley, 240p, first 20 s, every
frame with deblocking) and while streaming it:

| CPU | Bus | Cache miss | Bench | Streaming at the lowest level |
| --- | --- | --- | --- | --- |
| 312 MHz | 208 MHz | 460 ns | 16.5 fps | about 7.5 fps, choppy in busy scenes |
| 520 MHz | 208 MHz | 450 ns | 19.5 fps | about 11 fps |
| 520 MHz | 260 MHz | 360 ns | 23.3 fps | 11.9 fps, CPU 40-70% |

A 67% faster CPU decodes only 18% faster; a 25% faster bus cuts every
cache miss by a fifth. That splits a 240p frame at stock speed into about
23 ms of work and 37 ms of waiting. Prefetching does not help: `PLD`
reads a megabyte 2.7 times as fast, but FFmpeg's own prefetches (its
ARMv5TE build) left decoding as fast as before.

The decoder needs about 2.3 MB for 240p, more than the largest free block
of a TX's heap. It takes a second block from elsewhere. When there is
not enough memory for that, PalmView streams 144p instead.

In the player, 240p is shrunk by 4 to 3, from 426x240 to 319x180. Each
picture pixel averages the luma of the source pixels it covers, with
weights of 3:1, 1:1 or 1:3 across and down. Taking the nearest pixel
instead drops every fourth row and column: edges come out jagged and
flicker in motion. One word holds four source pixels, and the even and
odd columns are blended as two 16-bit halves at once. That takes 1.8 M
instructions a frame against 1.5 M for the nearest pixel. On the
overclocked TX, both keep the CPU about 53% busy.

### Full screen

Full screen hides the input area and status bar, and PalmView draws the
picture turned a quarter clockwise into the portrait screen. The TX's
own landscape mode cannot be used. In it, the display bitmap moves to a
separate 480x320 copy (at 0x50000000 instead of 0x5004B000 on a TX),
which the OS turns onto the screen only when it draws there itself.
Bench's Screen test mode shows this.

A turned picture row runs down a screen column. Written that way, every
pixel lands in another screen row and is a memory write of its own: 144p
at 480x270 fell to half rate. The converter instead takes 16 rows at a
time through a buffer in the cache, then writes them out along the
screen rows, two pixels a write. On the overclocked TX, full screen 240p
keeps the CPU 50-70% busy, and 144p plays at 23-25 fps.

### PalmView Bench

`PalmView Bench` measures the decoder on files PalmView saved.

Install `dist/PalmViewBench.prc` (handheld memory or card), then:

- **Info** loads the module, checks a round trip into ARM code, and shows
  the CPU and heap. It then times plain arithmetic, reads and writes in
  and out of the cache, and loads that each wait for the last. Run it at
  two clock speeds to see what waits for memory: on a TX, a cache miss
  takes about 450 ns at the stock bus speed, whatever the CPU clock.
- **Run** decodes the first 20 seconds of the chosen file's video, then its
  audio, in the chosen mode. It shows decode speed, file read time, color
  conversion speed, memory, and checksums.
- In the **Screen test** mode, **Run** turns the screen to landscape for
  4 seconds and draws straight into the display bitmap, as the player
  does, then lists the bitmap's size and address in each orientation.

Correct decoding gives the same checksums as `build/host/bench FILE 20 MODE`
on the PC (MODE 0 = full, 1 = no deblocking, 2 = skip B-frames).

How the module is built and loaded:

- Code is compiled position-independent: every global and every call to
  another file goes through the GOT (`-fPIC -msingle-pic-base -mlong-calls`).
- `tools/armlink.py` splits the code into resources of under 64 KB. They run
  in place and are never modified. Its checks fail the build if any code
  would need patching.
- `src/palm/arm_module.c` locks the code resources, copies the GOT and
  data into one heap block, and applies the fixups.
- `tools/patch-ffmpeg.py` limits FFmpeg to 8-bit H.264 and small integer
  transforms. That halves the code to 1.2 MB and cuts BSS from 1.9 MB to
  178 KB.
- Code targets ARMv5T, without `ldrd`/`strd`: those need 8-byte alignment
  and Palm OS only guarantees 4 bytes for resources.

`make test-arm` runs the packaged module in a PXA270 emulator (Unicorn),
loaded the same way at deliberately awkward addresses. It checks that it
decodes the test videos exactly like the host build.

`make test-arm` also sends the sound through the ring and calls the stream
callback the way Palm OS would: on another stack, with a foreign `r9`. The
sound that comes out must match the decoder's checksum.

To see where the time goes, profile by function:

```
deps/venv/bin/python host/tests/arm_emulate.py build/arm/res VIDEO.mp4 \
    --seconds 5 --profile build/arm/pass2.map
```

It prints instruction counts per function and per object file, separately
for video decoding, RGB conversion and audio. Video decoding includes a
checksum the emulator asks for and the player does not.

`make test-playback` runs `playback.c` on the PC against a simulated Palm
clock, with decode costs from PalmView Bench and a simulated sound stream.
It checks that every frame is shown or skipped, that all the sound plays,
that pausing holds the position, and that playback ends. It also reports how
far each picture was from the sound heard at that moment. It then streams
each pair of downloaded DASH files over a simulated 40 KB/s network that
drops out for 15 seconds, and again seeking forward, back and past the end,
showing how long each seek takes to play again.
`build/host/playback_sim FILE [LOAD]` shows how quality adapts when decoding
is LOAD times slower. To try other network speeds and outages:

```sh
build/host/playback_sim --stream testdata/media/ID.video.mp4 \
    testdata/media/ID.audio.mp4 30 [OUTAGE_AT SECONDS] [--seek AT TO]...
```

`make test` also reads each DASH file through `mp4_stream.c` in random
pieces and small rings, and checks every sample against the merged file,
then seeks to random segments and checks the samples after each. It also
runs `stream_feed.c` against a fake network that serves the two files,
reuses or drops connections, seeks now and then, and checks that only one
request is in flight, none asks for more than fits, reads are paced, and
both streams arrive whole.

`make test` also checks the RGB565 conversion pixel by pixel against a
floating-point reference, for every scale of small sizes, both byte orders
and padded rows.

## Build

Linux or macOS. You need Node.js, ImageMagick, Clang, autotools, OpenSSL
headers (for the host tool) and Python 3 (for tests).

```sh
tools/fetch-toolchain.sh   # prebuilt palm-toolchain from its Docker image
tools/fetch-deps.sh        # SDK 5r4, PalmTLS and TJpgDec, pinned
tools/build-arm-deps.sh    # FFmpeg and musl for the ARM module, pinned
make                       # build/debug/PalmView.prc
make test                  # host tests
make test-arm              # ARM module in an emulator vs. the host
make test-playback         # player timing against a simulated clock
make dist                  # PalmView, PalmTLSArm and PalmViewBench PRCs
```

The ARM module needs `arm-none-eabi-gcc` with newlib (Arch:
`arm-none-eabi-gcc arm-none-eabi-newlib`; Debian/Ubuntu:
`gcc-arm-none-eabi libnewlib-arm-none-eabi`).

`fetch-toolchain.sh` needs Docker. If your user is not in the `docker`
group, run it from a `newgrp docker` shell. You can also build
[palm-toolchain](https://github.com/palmsnipe/palm-toolchain) from source
and set `PALM_TOOLCHAIN_ROOT`.

### Host tool

`make host` builds `build/host/pview`. It uses the same request and parsing
code as the Palm app, with OpenSSL instead of PalmTLS:

```sh
build/host/pview search "palm tx"
build/host/pview info jNQXAC9IVRw
build/host/pview get jNQXAC9IVRw testdata/media
host/tests/compare-merge.sh build/host/pview testdata/media jNQXAC9IVRw
```

`compare-merge.sh` checks every packet and the decoded output of our merge
against ffmpeg's.

## Source layout

- `src/core/`: portable C, built for both the Palm and the host.
  `json_scan` (streaming JSON), `yt_api` (YouTube requests and responses),
  `mp4_merge`, `mp4_read` (saved files), `mp4_stream` (DASH streams as they
  arrive), `mp4_frag` (fragment parsing for both), `text` (encodings, file
  names), `thumb` (thumbnails, decoded with TJpgDec).
- `src/palm/`: the app. `main.c` (UI and flow), `net_job.c` (cooperative
  HTTPS, based on PalmTLS's TLS Tester), `stream_feed.c` (streaming
  downloads), `store.c` (memory card), `arm_module.c` (native code loader),
  `playback.c` (the player), `udp_log.c` (the network log).
- `src/arm/`: the native ARM module. `decoder.c` (FFmpeg wrapper),
  `rgb.c` (frames to RGB565, scaled and turned), `sound.c` (the PCM ring
  and stream callback), `dispatch.c` and `entry.S`
  (the 68K-to-ARM call), `libc.c` (heap and the C library bits FFmpeg
  needs).
- `src/bench/`: PalmView Bench.
- `host/`: the `pview` tool, a Palm OS shim and tests.
- `certs/google.pem`: the Google Trust Services WR2 and WE2 issuers the app
  trusts. Refresh with `tools/update-certs.sh`.

68K code runs PC-relative with a 32 KB reach, and the app is larger than
that. `tools/m68k-veneers.py` gives each object a small veneer, an absolute
jump, for every function it calls in another file. So only a single file
must stay under 32 KB. The 68000 cannot run code at an odd address, so the
link fails if any function, veneers included, starts at one.

Each code resource must stay under 65,000 bytes, so the code is split in
two. `src/palm/linker.lkr` lists the files that go into code 2; everything
else goes into code 1. A file's code always stays in one resource. At each
launch, the startup code locks code 2 and fixes up the absolute addresses
in both resources. It uses the table that `tools/m68k-relocs.py` builds
(the `Rloc` resource). Debug builds are optimized for size like release
builds. Globals are not available: keep state on the heap.

## License

GPL-3.0-or-later, because PalmTLS and its HTTP component are GPLv3.
The app also contains TJpgDec by ChaN (its own permissive license).
The ARM module also contains FFmpeg (LGPL-2.1-or-later, built without GPL
parts), musl's libm (MIT) and parts of newlib.
Downloading from YouTube may break its terms of service; this is for
personal use.
