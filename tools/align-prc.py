#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Word-aligns the native code resources ('armc') in a PRC file.

ARM code runs in place from its resources, so each must start on a 4-byte
boundary. The armc resources are multiples of 4 bytes and stored back to
back, so a padding resource ('armP', sorted just before them) aligns them
all. PalmTLS does the same for its ARMlet.

  align-prc.py FILE.prc
"""

import struct
import sys

HEADER = 78
ENTRY = 10
GAP = 2
PAD_TYPE = b"armP"


def read(path):
    with open(path, "rb") as handle:
        data = handle.read()
    count = struct.unpack_from(">H", data, 76)[0]
    entries = [struct.unpack_from(">4sHI", data, HEADER + i * ENTRY) for i in range(count)]
    resources = []
    for i, (kind, rid, offset) in enumerate(entries):
        end = entries[i + 1][2] if i + 1 < count else len(data)
        resources.append((kind, rid, data[offset:end]))
    return data[:HEADER], resources


def layout(resources):
    offset = HEADER + len(resources) * ENTRY + GAP
    placed = []
    for kind, rid, body in resources:
        placed.append((kind, rid, offset, body))
        offset += len(body)
    return placed


def main(path):
    header, resources = read(path)
    resources = [r for r in resources if r[0] != PAD_TYPE]
    if any(len(body) % 4 for kind, _, body in resources if kind == b"armc"):
        sys.exit("align-prc: armc resources must be multiples of 4 bytes")
    for size in (0, 1, 2, 3, 4):
        candidate = resources + ([(PAD_TYPE, 0, b"\0" * size)] if size else [])
        candidate.sort(key=lambda r: (r[0], r[1]))
        placed = layout(candidate)
        if all(offset % 4 == 0 for kind, _, offset, _ in placed if kind == b"armc"):
            break
    else:
        sys.exit("align-prc: could not align the armc resources")
    out = bytearray(header)
    struct.pack_into(">H", out, 76, len(placed))
    for kind, rid, offset, _ in placed:
        out += struct.pack(">4sHI", kind, rid, offset)
    out += b"\0" * GAP
    for _, _, _, body in placed:
        out += body
    with open(path, "wb") as handle:
        handle.write(out)
    print(f"align-prc: {sum(1 for p in placed if p[0] == b'armc')} armc resources "
          f"word-aligned with {size} padding byte(s)")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
