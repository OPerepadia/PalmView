#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the Unicode to Shift-JIS table for Japanese text with CJKOS.

Shift-JIS here is cp932, the Windows variant that Palm Japanese systems use.
Each entry is 4 bytes, big-endian: a UCS-2 character, then its code, with
a high byte of 0 for one-byte codes (half-width katakana). Entries are
sorted by character. ASCII is left out: it maps to itself.

  jis-table.py OUTPUT
"""

import struct
import sys


def main(output):
    entries = []
    for cp in range(0x80, 0x10000):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        try:
            code = chr(cp).encode("cp932")
        except UnicodeEncodeError:
            continue
        # One-byte codes other than half-width katakana are control codes.
        if len(code) == 1 and not 0xA1 <= code[0] <= 0xDF:
            continue
        entries.append((cp, int.from_bytes(code, "big")))
    with open(output, "wb") as handle:
        for cp, code in entries:
            handle.write(struct.pack(">HH", cp, code))
    print(f"jis-table: {len(entries)} characters, {len(entries) * 4} bytes")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
