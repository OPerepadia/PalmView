# PalmView

YouTube for the Palm TX, with no helper server. PalmView searches YouTube
and plays a video's 144p H.264 and AAC streams with sound. It can stream
them over HTTPS, or download and merge them into one MP4 on the memory
card. You can also play saved files in CorePlayer.

Status: downloading, playback with sound, and streaming work on a Palm TX.

## How it works

1. Search and video info use YouTube's own API (`/youtubei/v1/search` and
   `/player`), posing as the visionOS app. That client currently gets direct
   stream URLs, with no JavaScript challenge and no PO token.
2. The app downloads itag 160 (256x144 H.264) and itag 140 (AAC-LC) in 4 MB
   ranges. HTTPS comes from [PalmTLS](https://github.com/palmsnipe/palm-tls).
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
2. Tap a result. PalmView fetches the stream info.
3. Tap **Play** to stream it, or **Download** to save it. Download progress
   shows the speed and TLS handshake time.
4. A downloaded video is saved as `/PalmView/<title>.mp4` on the card. Tap
   **Play** to watch it, or open it in CorePlayer.

**Saved** on the main screen lists the videos on the card; tap one to play
it. In the player, tap the picture, **Pause** or the 5-way centre to pause.

While streaming, the bar under the video shows the position in red and
what has downloaded in dark gray. Tap or drag on it to jump there, or
press left and right on the 5-way to jump about 10 seconds. Playback goes
on from the start of the piece of video (about 5 seconds) that holds that
time, after a few seconds of loading.

Press up and down on the 5-way navigator to change the volume. PalmView
remembers it.

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

- 144p only, and only videos that offer H.264 and AAC streams.
- No live streams, "made for kids", age-restricted or members-only videos.
- Titles are converted to the Palm's Latin-1 text; Cyrillic is
  transliterated.
- Merging runs in one go and cannot be cancelled.
- The built-in player cannot seek yet.

## Player

`src/arm/` holds a native ARM module with FFmpeg's H.264 and fixed-point AAC
decoders. `src/palm/playback.c` drives it:

- The decoder keeps up to three frames ready, decoding ahead while there is
  time before the next one is due.
- Each frame is converted to RGB565 and scaled (256x144 to 320x180) straight
  into the screen's framebuffer, at its presentation time.
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
  at no more than 32 KB/s, a little above both streams' bitrate together.
  A response comes at up to 200 KB/s, and each 16 KB TLS record is
  decrypted in one go: read that fast, too many frames come late. Below
  half, for example at the start, it is read at full speed.
- A response that sends nothing for 5 s is asked for again on a new
  connection. A dropped connection starts again where it stopped. Redirects
  to another YouTube server are followed, for downloads too.
- The two rings share up to 512 KB of the heap, split by bitrate. At 144p
  (about 26 KB/s for both streams) that is about 20 seconds.
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
- Network calls never wait during playback. The sockets stay non-blocking,
  so TLS does not wait for the rest of a 16 KB record, and a socket is only
  read when it has data. The network gets a turn at least every 20 ms.
- Smooth streaming needs PalmTLS's faster ARM AES-GCM (the
  `faster-aes-gcm` branch, not released yet). With PalmTLS 0.1.2, each
  16 KB record takes about 100 ms to decrypt, and video drops to half rate.
  `tools/fetch-deps.sh` still pins 0.1.2.

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

### PalmView Bench

`PalmView Bench` measures the decoder on files PalmView saved.

Install `dist/PalmViewBench.prc` (handheld memory or card), then:

- **Info** loads the module, checks a round trip into ARM code, and shows
  the CPU and heap.
- **Run** decodes the first 20 seconds of the chosen file's video, then its
  audio, in the chosen mode. It shows decode speed, file read time, color
  conversion speed, memory, and checksums.

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
tools/fetch-deps.sh        # SDK 5r4 and PalmTLS, pinned
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
  names).
- `src/palm/`: the app. `main.c` (UI and flow), `net_job.c` (cooperative
  HTTPS, based on PalmTLS's TLS Tester), `stream_feed.c` (streaming
  downloads), `store.c` (memory card), `arm_module.c` (native code loader),
  `playback.c` (the player), `udp_log.c` (the network log).
- `src/arm/`: the native ARM module. `decoder.c` (FFmpeg wrapper),
  `sound.c` (the PCM ring and stream callback), `dispatch.c` and `entry.S`
  (the 68K-to-ARM call), `libc.c` (heap and the C library bits FFmpeg
  needs).
- `src/bench/`: PalmView Bench.
- `host/`: the `pview` tool, a Palm OS shim and tests.
- `certs/google.pem`: the Google Trust Services WR2 and WE2 issuers the app
  trusts. Refresh with `tools/update-certs.sh`.

68K code runs PC-relative with a 32 KB reach, and the app is larger than
that. `tools/m68k-veneers.py` gives each object a small long-call veneer for
every function it calls in another file, so only a single file must stay
under 32 KB. The 68000 cannot run code at an odd address, so the link fails
if any function, veneers included, starts at one. The code resource as a
whole must stay under 64 KB, so debug builds are optimized for size like
release builds. Globals are not available: keep state on the heap.

## License

GPL-3.0-or-later, because PalmTLS and its HTTP component are GPLv3.
The ARM module also contains FFmpeg (LGPL-2.1-or-later, built without GPL
parts), musl's libm (MIT) and parts of newlib.
Downloading from YouTube may break its terms of service; this is for
personal use.
