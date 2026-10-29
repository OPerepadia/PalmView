#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs the thumbnail module in the PXA270 emulator on testdata/thumbs and
checks its pixels against the reference .rgb files, the way unit_tests
checks the host build. Prints the instructions each decode takes.

  arm_thumbs.py RES_DIR
"""

import glob
import struct
import sys

from arm_emulate import Module, CALL_ADDR

JPEG_ADDR = CALL_ADDR + 0x1000
PIXELS_ADDR = CALL_ADDR + 0x10000
DECODE = 1
SIZES = {0: (120, 68), 1: (90, 54)}


def rgb(pixel):
    return ((pixel >> 11) << 3, ((pixel >> 5) & 0x3F) << 2, (pixel & 0x1F) << 3)


def main(res_dir):
    module = Module(res_dir, 0x1000, True, base=1100)
    failures = 0
    for path in sorted(glob.glob("testdata/thumbs/*.jpg")):
        jpeg = open(path, "rb").read()
        full = open(path[:-4] + ".rgb", "rb").read()
        for list_size, (width, height) in SIZES.items():
            module.uc.mem_write(JPEG_ADDR, jpeg)
            module.instructions = 0
            result, _ = module.call(DECODE, JPEG_ADDR, len(jpeg), PIXELS_ADDR, list_size)
            pixels = struct.unpack(f">{width * height}H",
                                   module.uc.mem_read(PIXELS_ADDR, width * height * 2))
            left, top = (120 - width) // 2, (68 - height) // 2
            difference = 0
            for y in range(height):
                for x in range(width):
                    want = full[((y + top) * 120 + x + left) * 3:][:3]
                    difference += sum(abs(a - b) for a, b in zip(rgb(pixels[y * width + x]), want))
            ok = result == 1 and difference <= 8 * width * height * 3
            failures += not ok
            print(f"{'ok' if ok else 'FAIL'}: {path} {width}x{height}, "
                  f"{module.instructions / 1e6:.2f} M instructions")
        module.instructions = 0
        result, _ = module.call(DECODE, JPEG_ADDR, len(jpeg) // 2, PIXELS_ADDR, 1)
        if result != 0:
            print(f"FAIL: {path} truncated decoded")
            failures += 1
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main(sys.argv[1])
