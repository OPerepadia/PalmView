#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs the packaged ARM module in a PXA270 emulator (Unicorn), loading it
the way the Palm loader does, and decodes the start of an MP4 through the
same ArmCall protocol. Prints checksums to compare with build/host/bench.

  arm_emulate.py RES_DIR FILE.mp4 [--seconds S] [--count] [--png OUT.png]
                 [--queue N] [--sound] [--profile PASS2.MAP [--top N]]

Segments are placed out of order at 4-byte (not 8-byte) aligned addresses,
which is all Palm OS guarantees for resources.

--profile counts instructions per function, using the link map armlink.py's
second pass writes (build/arm/pass2.map), and prints where video decoding,
RGB conversion and audio decoding spend them.

--sound sends the audio through the sound ring and drains it by calling the
stream callback as Palm OS would: on another stack, with a foreign r9. It
also checks that a paused ring gives silence and holds the clock.
"""

import argparse
import bisect
import collections
import os
import re
import struct
import subprocess
import sys
import time
import zlib

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_BLOCK, UcError
from unicorn.arm_const import (UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0,
                               UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_R9, UC_ARM_REG_SP, UC_CPU_ARM_PXA270)

CODE_BASE = 0x10000000
RW_BASE = 0x20000000
STACK_BASE, STACK_SIZE = 0x30000000, 0x10000
NATIVE_STACK = 0x31000000          # stands in for PceNativeCall's 4 KB stack
ARENA_BASE = 0x40000000
CALL_ADDR = 0x50000000
SAMPLE_ADDR = 0x50001000
PCM_ADDR = 0x50080000
RGB_ADDR = 0x50090000
SOUND_ADDR = 0x500C0000
SOUND_STACK = 0x31000800           # the sound thread's stack, apart from ours
RETURN_ADDR = 0x60000000
CALLBACK_FRAMES = 2048

CMD = dict(init=1, video_open=2, video_decode=3, video_info=4, video_rgb=5,
           audio_open=6, audio_decode=7, audio_info=8, stats=9, close=10,
           ping=11, video_pop=12, sound_open=13, sound_feed=14, sound_state=15,
           sound_end=16, sound_pause=17)
MAGIC = 0x50565731
DEC_CHECKSUM = 0x80
DEC_RGB_BIG_ENDIAN = 0x01


class Profile:
    """Maps emulated code addresses back to functions and object files."""

    def __init__(self, map_path, segments, sizes):
        self.segments, self.sizes = segments, sizes
        self.bases = {}
        functions = []
        pending = None
        line_re = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)")
        with open(map_path) as handle:
            for line in handle:
                seg = re.match(r"^\.seg(\d+)\s+0x([0-9a-f]+)", line)
                if seg:
                    self.bases[int(seg.group(1))] = int(seg.group(2), 16)
                    continue
                one = re.match(r"^ \.text\.(\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)",
                               line)
                if one:
                    functions.append((int(one.group(2), 16), int(one.group(3), 16),
                                      one.group(1), one.group(4)))
                    pending = None
                    continue
                head = re.match(r"^ \.text\.(\S+)\s*$", line)
                if head:
                    pending = head.group(1)
                    continue
                rest = line_re.match(line)
                if pending and rest:
                    functions.append((int(rest.group(1), 16), int(rest.group(2), 16),
                                      pending, rest.group(3)))
                pending = None
        functions.sort()
        self.starts = [f[0] for f in functions]
        self.functions = functions

    def locate(self, address):
        for i, start in enumerate(self.segments):
            if start <= address < start + self.sizes[i]:
                elf = self.bases[i] + address - start
                k = bisect.bisect_right(self.starts, elf) - 1
                if k >= 0:
                    begin, size, name, path = self.functions[k]
                    if elf < begin + size:
                        member = re.search(r"\(([^)]+)\)$", path)
                        return name, member.group(1) if member else os.path.basename(path)
                return "?", "?"
        return "?", "?"

    def report(self, title, blocks, divisor, unit, top):
        by_function = collections.Counter()
        by_object = collections.Counter()
        for address, count in blocks.items():
            name, obj = self.locate(address)
            by_function[(name, obj)] += count
            by_object[obj] += count
        total = sum(by_function.values())
        if total == 0:
            return
        print(f"profile: {title}, {total / divisor / 1e6:.2f} M instructions {unit}")
        for (name, obj), count in by_function.most_common(top):
            print(f"  {100 * count / total:5.1f}%  {count / divisor / 1e6:6.3f} M  "
                  f"{name}  ({obj})")
        print("  by object file:")
        for obj, count in by_object.most_common(top // 2):
            print(f"  {100 * count / total:5.1f}%  {count / divisor / 1e6:6.3f} M  {obj}")


def signed(value):
    return value - (1 << 32) if value & 0x80000000 else value


class Module:
    def __init__(self, res_dir, arena_size, count, profile_map=None):
        # RES_DIR is armlink.py's output directory or a finished PRC.
        if res_dir.endswith(".prc"):
            with open(res_dir, "rb") as handle:
                prc = handle.read()
            n = struct.unpack_from(">H", prc, 76)[0]
            entries = [struct.unpack_from(">4sHI", prc, 78 + i * 10) for i in range(n)]
            table = {}
            for i, (kind, rid, offset) in enumerate(entries):
                end = entries[i + 1][2] if i + 1 < n else len(prc)
                table[f"{kind.decode()}{rid}.bin"] = prc[offset:end]
            def read(name):
                return table[name]
        else:
            def read(name):
                with open(f"{res_dir}/{name}", "rb") as handle:
                    return handle.read()

        header = read("armh1000.bin")
        magic, _version, nseg, ndata, nfix, _ = struct.unpack_from(">IIHHHH", header, 0)
        if magic != 0x5054414D:
            sys.exit("bad module header")
        entry, image_size, rw_total, got_offset, fixup_count, _ = struct.unpack_from(
            ">IIIIII", header, 16)
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.ctl_set_cpu_model(UC_CPU_ARM_PXA270)
        self.uc.mem_map(CODE_BASE, 0x200000)
        self.uc.mem_map(RW_BASE, 0x100000)
        self.uc.mem_map(STACK_BASE, STACK_SIZE)
        self.uc.mem_map(NATIVE_STACK - 0x1000, 0x2000)
        self.uc.mem_map(ARENA_BASE, (arena_size + 0xFFF) & ~0xFFF)
        self.uc.mem_map(CALL_ADDR, 0x100000)
        self.uc.mem_map(RETURN_ADDR, 0x1000)
        self.segments = []
        sizes = []
        for i in range(nseg):
            code = read(f"armc{1001 + i}.bin")
            # Reverse order, misaligned by 4, 8 or 12 mod 16.
            address = CODE_BASE + (nseg - 1 - i) * 0x10000 + 4 + 4 * (i % 3)
            self.uc.mem_write(address, code)
            self.segments.append(address)
            sizes.append(len(code))
        image = b"".join(read(f"armd{2001 + i}.bin") for i in range(ndata))
        if len(image) != image_size:
            sys.exit("data image size mismatch")
        self.uc.mem_write(RW_BASE, image + bytes(rw_total - image_size))
        fixups = b"".join(read(f"armr{3001 + i}.bin") for i in range(nfix))
        if len(fixups) != fixup_count * 8:
            sys.exit("fixup count mismatch")
        for i in range(fixup_count):
            rw_offset, word = struct.unpack_from(">II", fixups, i * 8)
            region, offset = word >> 24, word & 0xFFFFFF
            base = RW_BASE if region == 0xFF else self.segments[region]
            self.uc.mem_write(RW_BASE + rw_offset, struct.pack("<I", base + offset))
        self.got = RW_BASE + got_offset
        self.entry = self.segments[0] + entry
        self.instructions = 0
        self.blocks = collections.Counter()
        self.profile = Profile(profile_map, self.segments, sizes) if profile_map else None
        if count or self.profile:
            self.uc.hook_add(UC_HOOK_BLOCK, self._count)
        print(f"module: {nseg} segments, {rw_total} bytes writable, "
              f"{fixup_count} fixups, GOT at 0x{self.got:x}")

    def _count(self, uc, address, size, user):
        self.instructions += size // 4
        if self.profile:
            self.blocks[address] += size // 4

    def call(self, command, *args):
        words = [MAGIC, self.got, STACK_BASE + STACK_SIZE, CMD[command], 0]
        words += list(args) + [0] * (8 - len(args)) + [0] * 8
        self.uc.mem_write(CALL_ADDR, struct.pack(">21I", *[w & 0xFFFFFFFF for w in words]))
        self.uc.reg_write(UC_ARM_REG_R0, 0)
        self.uc.reg_write(UC_ARM_REG_R1, CALL_ADDR)
        self.uc.reg_write(UC_ARM_REG_R2, 0)
        self.uc.reg_write(UC_ARM_REG_SP, NATIVE_STACK)
        self.uc.reg_write(UC_ARM_REG_LR, RETURN_ADDR)
        try:
            self.uc.emu_start(self.entry, RETURN_ADDR)
        except UcError as error:
            pc = self.uc.reg_read(UC_ARM_REG_PC)
            sys.exit(f"{command}: emulation error {error} at pc 0x{pc:x}")
        if self.uc.reg_read(UC_ARM_REG_SP) != NATIVE_STACK:
            sys.exit(f"{command}: stack pointer not restored")
        block = struct.unpack(">21I", self.uc.mem_read(CALL_ADDR, 84))
        return signed(block[4]), block[13:21]

    def call_native(self, address, *args):
        """Calls an ARM function the way Palm OS calls a native callback."""
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3), args):
            self.uc.reg_write(reg, value)
        self.uc.reg_write(UC_ARM_REG_R9, 0xDEADBEEF)
        self.uc.reg_write(UC_ARM_REG_SP, SOUND_STACK)
        self.uc.reg_write(UC_ARM_REG_LR, RETURN_ADDR)
        try:
            self.uc.emu_start(address, RETURN_ADDR)
        except UcError as error:
            pc = self.uc.reg_read(UC_ARM_REG_PC)
            sys.exit(f"callback: emulation error {error} at pc 0x{pc:x}")
        if (self.uc.reg_read(UC_ARM_REG_R9) != 0xDEADBEEF or
                self.uc.reg_read(UC_ARM_REG_SP) != SOUND_STACK):
            sys.exit("callback: r9 or sp not restored")
        return self.uc.reg_read(UC_ARM_REG_R0)


def adler(data, a=1, b=0):
    for byte in data:
        a = (a + byte) % 65521
        b = (b + a) % 65521
    return a, b


def packets(path, stream):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-ignore_editlist", "1", "-select_streams", stream,
         "-show_entries", "packet=pos,size,dts", "-of", "csv=p=0", path],
        check=True, capture_output=True, text=True).stdout
    rows = []
    for line in out.split():
        dts, size, pos = line.split(",")
        rows.append((int(dts), int(size), int(pos)))
    return rows


def box_payload(data, kind):
    index = data.index(kind)
    size = struct.unpack_from(">I", data, index - 4)[0]
    return data[index + 4:index - 4 + size]


def audio_config(esds):
    p = 4

    def length():
        nonlocal p
        value = 0
        for _ in range(4):
            b = esds[p]; p += 1
            value = (value << 7) | (b & 0x7F)
            if not b & 0x80:
                break
        return value

    assert esds[p] == 3; p += 1; length()
    flags = esds[p + 2]; p += 3
    if flags & 0x80: p += 2
    if flags & 0x40: p += 1 + esds[p]
    if flags & 0x20: p += 2
    assert esds[p] == 4; p += 1; length(); p += 13
    assert esds[p] == 5; p += 1
    n = length()
    return esds[p:p + n]


def write_png(path, rgb565_be, width, height):
    rows = b""
    for y in range(height):
        rows += b"\0"
        for x in range(width):
            v = struct.unpack_from(">H", rgb565_be, (y * width + x) * 2)[0]
            r, g, b = (v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3
            rows += bytes((r | r >> 5, g | g >> 6, b | b >> 5))
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(
            ">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
    with open(path, "wb") as handle:
        handle.write(png)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("res")
    parser.add_argument("mp4")
    parser.add_argument("--seconds", type=float, default=1.0)
    parser.add_argument("--skip", type=int, default=0)
    parser.add_argument("--arena", type=int, default=3 * 1024 * 1024)
    parser.add_argument("--count", action="store_true")
    parser.add_argument("--png")
    parser.add_argument("--queue", type=int, default=1)
    parser.add_argument("--sound", action="store_true")
    parser.add_argument("--profile", metavar="PASS2_MAP")
    parser.add_argument("--top", type=int, default=20)
    args = parser.parse_args()
    if args.profile:
        args.count = True

    with open(args.mp4, "rb") as handle:
        data = handle.read()
    m = Module(args.res, args.arena, args.count, args.profile)
    result, _ = m.call("ping", *range(1, 9))
    print(f"ping: {result} (expect 36)")
    result, out = m.call("init", ARENA_BASE, args.arena, STACK_BASE, STACK_SIZE)
    print(f"init: {result}, module version {out[0]}, heap free {out[1]}")

    avcc = box_payload(data, b"avcC")
    m.uc.mem_write(SAMPLE_ADDR, avcc)
    result, _ = m.call("video_open", SAMPLE_ADDR, len(avcc), args.queue)
    print(f"video open: {result}")
    if result != 0:
        sys.exit(1)
    video = packets(args.mp4, "v:0")
    timescale = int(subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=time_base", "-of", "csv=p=0", args.mp4],
        check=True, capture_output=True, text=True).stdout.strip().split("/")[1])
    limit = args.seconds * timescale
    start, n = time.time(), 0
    m.blocks.clear()
    for dts, size, pos in video:
        if dts >= limit:
            break
        before = m.instructions
        m.uc.mem_write(SAMPLE_ADDR, data[pos:pos + size] + bytes(64))
        result, _ = m.call("video_decode", SAMPLE_ADDR, size, 0, args.skip | DEC_CHECKSUM)
        if result < 0:
            sys.exit(f"video decode failed at sample {n}: {result}")
        if args.count and n < 3:
            print(f"  sample {n}: {size} bytes, {m.instructions - before} instructions")
        n += 1
    video_instructions = m.instructions
    while m.call("video_decode", 0, 0, 0, args.skip | DEC_CHECKSUM)[0] > 0:
        pass
    _, out = m.call("video_info")
    frames, checksum, width, height = out[0], out[1], out[2], out[3]
    print(f"video: {n} samples, {frames} frames {width}x{height}, checksum {checksum:08x}"
          f" ({time.time() - start:.1f} s emulated)")
    if args.count and frames:
        print(f"  {video_instructions / n / 1e6:.2f} M instructions per frame")
    if m.profile and n:
        m.profile.report("video decoding", m.blocks, n, "per frame", args.top)
    if args.png or args.count:
        # The second call shows the cost without the one-time table setup.
        for _ in range(2):
            before = m.instructions
            m.blocks.clear()
            result, _ = m.call("video_rgb", RGB_ADDR, 320 * 2, 320, 180, DEC_RGB_BIG_ENDIAN)
        print(f"rgb: {result}" + (f", {m.instructions - before} instructions for 320x180"
                                  if args.count else ""))
        if m.profile:
            m.profile.report("RGB conversion to 320x180", m.blocks, 1, "per frame",
                             args.top)
        if args.png:
            write_png(args.png, bytes(m.uc.mem_read(RGB_ADDR, 320 * 180 * 2)), 320, 180)
            print(f"wrote {args.png}")

    _, out = m.call("stats")
    print(f"stats after video: heap used {out[0]}, peak {out[1]}")

    asc = audio_config(box_payload(data, b"esds"))
    m.uc.mem_write(SAMPLE_ADDR, asc)
    result, _ = m.call("audio_open", SAMPLE_ADDR, len(asc))
    print(f"audio open: {result} (config {len(asc)} bytes)")
    audio = packets(args.mp4, "a:0")
    before, n = m.instructions, 0
    m.blocks.clear()
    if args.sound:
        result, out = m.call("sound_open", 32768, 44100)
        if result != 0:
            sys.exit(f"sound open failed: {result}")
        callback, ring, channels = out[0], out[1], out[2]
        frame_bytes = channels * 2
        sums, played = (1, 0), 0

        def drain(final=False):
            nonlocal sums, played
            while True:
                _, state = m.call("sound_state")
                buffered = state[0]
                if buffered < CALLBACK_FRAMES and not (final and buffered > 0):
                    return
                if m.call_native(callback, ring, 0, SOUND_ADDR, CALLBACK_FRAMES) != 0:
                    sys.exit("callback failed")
                _, state = m.call("sound_state")
                # Once input has ended, silence counts as played too.
                real = buffered - state[0]
                played += real
                sums = adler(bytes(m.uc.mem_read(SOUND_ADDR, real * frame_bytes)), *sums)
    for dts, size, pos in audio:
        if dts >= args.seconds * 44100:
            break
        m.uc.mem_write(SAMPLE_ADDR, data[pos:pos + size] + bytes(64))
        if args.sound:
            result, _ = m.call("sound_feed", SAMPLE_ADDR, size, DEC_CHECKSUM)
            drain()
        else:
            result, _ = m.call("audio_decode", SAMPLE_ADDR, size, PCM_ADDR, DEC_CHECKSUM)
        if result < 0:
            sys.exit(f"audio decode failed at packet {n}: {result}")
        n += 1
    if args.sound:
        m.call("sound_pause", 1)
        _, before = m.call("sound_state")
        m.uc.mem_write(SOUND_ADDR, b"\x55" * CALLBACK_FRAMES * frame_bytes)
        if m.call_native(callback, ring, 0, SOUND_ADDR, CALLBACK_FRAMES) != 0:
            sys.exit("callback failed")
        _, after = m.call("sound_state")
        silent = not any(m.uc.mem_read(SOUND_ADDR, CALLBACK_FRAMES * frame_bytes))
        if after[:3] != before[:3] or not silent:
            sys.exit("a paused ring moved the sound on")
        m.call("sound_pause", 0)
        m.call("sound_end")
        drain(final=True)
        _, state = m.call("sound_state")
        _, out = m.call("audio_info")
        ring_sum = (sums[1] << 16) | sums[0]
        print(f"sound: {played} frames through the callback, {state[2]} underruns, "
              f"checksum {ring_sum:08x}"
              + (" (matches the decoder)" if ring_sum == out[1] else " (MISMATCH)"))
        if ring_sum != out[1] or played != out[0]:
            sys.exit("sound ring output differs from the decoder output")
    _, out = m.call("audio_info")
    print(f"audio: {n} packets, {out[0]} samples, {out[2]} ch, {out[3]} Hz, "
          f"checksum {out[1]:08x}")
    if args.count and out[0]:
        seconds = out[0] / out[3]
        print(f"  {(m.instructions - before) / seconds / 1e6:.1f} M instructions per second of audio")
        if m.profile:
            m.profile.report("audio decoding", m.blocks, seconds, "per second",
                             args.top)
    _, out = m.call("stats")
    print(f"stats: heap used {out[0]}, peak {out[1]}, free {out[2]}, stack used {out[3]}")
    m.call("close")


if __name__ == "__main__":
    main()
