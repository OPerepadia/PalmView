# PalmView technical notes

- [Source layout](#source-layout)
- [Playback](#playback)
  - [Player](#player)
  - [Streaming](#streaming)
  - [240p](#240p)
  - [Full screen](#full-screen)
- [Native code](#native-code)
  - [ARM modules](#arm-modules)
  - [68K code](#68k-code)
- [Development](#development)
  - [Build](#build)
  - [Host tool](#host-tool)
  - [Tests](#tests)
  - [PalmView Bench](#palmview-bench)
  - [Network log](#network-log)
- [YouTube](#youtube)
  - [API and downloads](#api-and-downloads)
  - [When YouTube changes](#when-youtube-changes)

## Source layout

- `src/core/`: portable C for the Palm and the host. JSON scanning,
  YouTube requests and replies, MP4 merging and streaming, text encodings,
  thumbnails.
- `src/palm/`: the app. One file per screen (`main.c`, `detail.c`,
  `player.c`, `saved.c`), plus `net_job.c` (HTTPS), `stream_feed.c`
  (streaming), `playback.c` (the player) and `download.c`.
- `src/arm/`: the native ARM modules. `decoder.c` wraps FFmpeg, `rgb.c`
  converts frames, `sound.c` holds the PCM ring and callback.
- `src/bench/`: PalmView Bench.
- `host/`: the `pview` tool, a Palm OS shim and tests.
- `certs/google.pem`: the Google Trust Services issuers the app trusts.
  Refresh with `tools/update-certs.sh`.

## Playback

### Player

- A native ARM module (`src/arm/`) runs FFmpeg's H.264 and fixed-point AAC
  decoders. `src/palm/playback.c` drives it.
- The sound played sets the clock, and pictures follow it. The Palm OS
  sound stream calls a native ARM callback on its own thread, with its own
  `r9`. `SoundCallbackEntry` in `src/arm/entry.S` loads the module's GOT
  first.
- Frames are converted to RGB565 straight into the screen's framebuffer.
  144p is scaled up by area, so every video pixel gets the same width.
- When decoding falls behind with the CPU busy, quality drops a level, and
  it comes back when there is time to spare. The levels and rules are in
  `playback.c`.
- The status line shows the frame rate, CPU use, skipped frames, quality,
  sound gaps, and while streaming the download speed.

On a TX, a 25 fps 144p frame takes about 25 ms to decode at full quality
and 19 ms without deblocking.

### Streaming

`src/palm/stream_feed.c` downloads both streams into RAM rings while they
play. Its design comes from how the TX's network behaves:

- **One request at a time.** On the TX, data left unread on one connection
  starves the other: neither its data nor a new handshake gets through. So
  only one request is in flight, and it never asks for more than its ring
  can take.
- **Kept-open connections.** Each new TLS handshake blocks the player for
  about 2 s. Each stream keeps its connection open between requests.
- **Paced reads.** TLS decrypts each 16 KB record in one go. Reading at the
  full 200 KB/s makes too many frames late, so a response is read a little
  faster than the video plays.
- **ChaCha20-Poly1305.** With PalmTLS 0.1.2's AES-GCM, each 16 KB record
  takes about 100 ms to decrypt, and video drops to half rate. The fork's
  native ChaCha20-Poly1305 brings the network down to about 3% of each
  second at 144p. PalmTLS lists it first, and YouTube's servers choose it.
- **Never waiting.** Sockets stay non-blocking, and a socket is only read
  when it has data.

Seeking uses each file's segment index (`sidx`). Video goes back to the
start of the segment, which begins with a keyframe. Sound segments are
10 s long, so the sound before that point is downloaded and dropped.
Switching quality fetches the new video's index first, then seeks the
same way.

A downloaded video has no segment index. Seeking goes back to the last
keyframe before the time asked for, found by reading the sample tables
from the start (`Mp4ReaderSeek`).

### 240p

240p has 2.8 times the pixels of 144p. It always plays at the lowest
quality level: no deblocking and no frames that nothing refers to. That is
12 fps for a 25 fps video.

Measured on a TX with PalmView Bench (240p, first 20 s, every frame with deblocking)
and while streaming:

| CPU | Bus | Cache miss | Bench | Streaming at the lowest level |
| --- | --- | --- | --- | --- |
| 312 MHz | 208 MHz | 460 ns | 16.5 fps | about 7.5 fps, choppy in busy scenes |
| 520 MHz | 208 MHz | 450 ns | 19.5 fps | about 11 fps |
| 520 MHz | 260 MHz | 360 ns | 23.3 fps | 11.9 fps, CPU 40-70% |

The last row is warpSpeed at BUS 20 and CPU:BUS 4. Above BUS 20, the TX
tested reset or froze at random. A 67% faster CPU decodes only 18% faster,
while a 25% faster bus cuts every cache miss by a fifth. Prefetching with
`PLD` did not help.

The decoder needs about 2.3 MB for 240p, more than the largest free block
of a TX's heap. It takes the rest from smaller blocks. Without enough
memory, PalmView streams 144p instead.

The player shrinks 240p to 319x180 by averaging. Taking the nearest pixel
drops every fourth row and column, so edges come out jagged and flicker.

### Full screen

Full screen draws the picture turned a quarter clockwise into the portrait
screen. The TX's own landscape mode cannot be used: in it, the display
bitmap moves to a separate 480x320 copy, which the OS puts on the screen
only when it draws there itself. Bench's Screen test shows this.

Writing a turned row pixel by pixel down a screen column is slow: 144p fell
to half rate. The converter takes 16 rows at a time through a buffer in
the cache, then writes them along the screen rows.

## Native code

### ARM modules

- Code is position-independent: every global and every call to another
  file goes through the GOT (`-fPIC -msingle-pic-base -mlong-calls`).
- `tools/armlink.py` splits the code into resources under 64 KB. They run
  in place and are never modified, and the build fails if any code would
  need patching. `src/palm/arm_module.c` copies the GOT and data into one
  heap block and applies the fixups.
- The decoder module starts at resource 1000, the thumbnail module at
  1100.
- Code targets ARMv5T without `ldrd`/`strd`. Those need 8-byte alignment,
  and Palm OS only guarantees 4 bytes for resources.
- `tools/patch-ffmpeg.py` limits FFmpeg to 8-bit H.264. That halves the
  code to 1.2 MB and cuts BSS from 1.9 MB to 178 KB.

### 68K code

68K code runs PC-relative with a 32 KB reach, and the app is larger than
that. `tools/m68k-veneers.py` gives each object an absolute-jump veneer
for every function it calls in another file. So only a single file must
stay under 32 KB. The link fails if any function starts at an odd address,
since the 68000 cannot run code there.

Each code resource must stay under 65,000 bytes, so the code is split in
two. `src/palm/linker.lkr` lists the files for code 2. At launch, the
startup code fixes up the absolute addresses in both, using the `Rloc`
table from `tools/m68k-relocs.py`. Globals are not available: keep state
on the heap.

## Development

### Build

Besides the tools in the README, the host tool needs OpenSSL headers.

The ARM modules need `arm-none-eabi-gcc` with newlib (Arch:
`arm-none-eabi-gcc arm-none-eabi-newlib`; Debian/Ubuntu:
`gcc-arm-none-eabi libnewlib-arm-none-eabi`).

`fetch-toolchain.sh` needs Docker. If your user is not in the `docker`
group, run it from a `newgrp docker` shell. You can also build
[palm-toolchain](https://github.com/palmsnipe/palm-toolchain) from source
and set `PALM_TOOLCHAIN_ROOT`.

`make` builds a debug PRC in `build/debug/`, and `make dist` puts the
release PRCs in `dist/`.

### Host tool

`make host` builds `build/host/pview`. It uses the same request and
parsing code as the Palm app, with OpenSSL instead of PalmTLS:

```sh
build/host/pview search "palm tx"
build/host/pview info jNQXAC9IVRw
build/host/pview get jNQXAC9IVRw testdata/media
host/tests/compare-merge.sh build/host/pview testdata/media jNQXAC9IVRw
```

`compare-merge.sh` checks every packet and the decoded output of the merge
against ffmpeg's.

### Tests

```sh
make test             # JSON, MP4 streams, the stream feed, RGB565
make test-arm         # ARM modules in an emulator, against the host build
make test-playback    # player timing against a simulated clock and network
```

`make test-arm` runs the packaged modules in a PXA270 emulator (Unicorn),
loaded at deliberately awkward addresses. It also calls the sound callback
the way Palm OS does: on another stack, with a foreign `r9`.

To see where ARM time goes, profile by function:

```sh
deps/venv/bin/python host/tests/arm_emulate.py build/arm/res VIDEO.mp4 \
    --seconds 5 --profile build/arm/pass2.map
```

`build/host/playback_sim` tries other decode speeds, network speeds and
outages:

```sh
build/host/playback_sim FILE [LOAD] [--seek AT TO]...
build/host/playback_sim --stream testdata/media/ID.video.mp4 \
    testdata/media/ID.audio.mp4 30 [OUTAGE_AT SECONDS] [--seek AT TO]...
```

### PalmView Bench

`dist/PalmViewBench.prc` measures the decoder on files PalmView saved:

- **Info** shows the CPU and heap, and times arithmetic and memory access.
  Run it at two clock speeds to see what waits for memory.
- **Run** decodes the first 20 seconds of the chosen file and shows decode
  speed, memory and checksums. The checksums must match
  `build/host/bench FILE 20 MODE` on the PC (MODE 0 = full,
  1 = no deblocking, 2 = skip B-frames).
- **Screen test** draws straight into the display bitmap in landscape and
  lists its size and address in each orientation.

### Network log

PalmView can send a log of network and playback events over UDP to a
computer on the same network. Add a line to `client.txt` on the card:

```
log=192.168.2.10:5140
```

Then listen on that computer, for example with `nc -klu 5140`:

```
41.230 v get 1048576-5242879 of 2600000
41.410 v dns 20 ms
41.520 v connect 110 ms
42.880 v tls 1360 ms
45.100 play at 30120 ms, buffer 17800 + 18000 ms
```

Without a `log=` line nothing is sent.

## YouTube

### API and downloads

- Search and video info use YouTube's own API (`/youtubei/v1/search` and
  `/player`).
- Videos are itag 160 (256x144 H.264) or itag 133 (426x240), with itag
  140 (AAC-LC) for sound. Downloads fetch them in 4 MB ranges.
- YouTube serves the two streams as separate fragmented MP4 files.
  `src/core/mp4_merge.c` rebuilds them into one regular MP4, with the moov
  box first. Its packets match `ffmpeg -c copy` output exactly.

### When YouTube changes

YouTube changes its API from time to time, and videos may stop working.
Put a `client.txt` file in `/PalmView` on the card with new client
values. Unknown keys and lines starting with `#` are ignored:

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
