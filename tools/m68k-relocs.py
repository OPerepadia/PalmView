#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the relocation table resource for the 68K code segments.

The app's code is linked at fixed addresses: segment 1 (.vec and .text) at
0x10000000, segment 2 (.text2) at 0x20000000. At every launch, the startup
code writes each absolute address (R_68K_32) from where the segments really
are. The table only says where each address points, so it never depends on
what is in the code now. It is big-endian:

    UInt16 count[4]     relocations from segment 1 to 1, 1 to 2, 2 to 1
                        and 2 to 2
    then, for each, in that order:
    UInt16 offset       where the address is in its segment
    UInt16 target       where it points in the target segment

  m68k-relocs.py READELF ELF OUTPUT
"""

import re
import struct
import subprocess
import sys

SEGMENTS = ((".vec", ".text"), (".text2",))
SECTION = re.compile(r"^\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)")
RELOCATIONS = re.compile(r"^Relocation section '\.rela?(\S+)'")
RELOC = re.compile(r"^([0-9a-f]+)\s+[0-9a-f]+\s+R_68K_32\s+([0-9a-f]+)\s+.*?([+-])\s*([0-9a-f]+)$")


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def main(readelf, elf, output):
    sections = {}
    for line in run(readelf, "-SW", elf).splitlines():
        match = SECTION.match(line)
        if match:
            sections[match.group(1)] = (int(match.group(2), 16), int(match.group(3), 16))
    ranges = []
    for names in SEGMENTS:
        present = [sections[name] for name in names if name in sections]
        start = min((address for address, _ in present), default=0)
        end = max((address + size for address, size in present), default=0)
        if end - start > 65000:
            sys.exit(f"m68k-relocs: segment {len(ranges) + 1} is {end - start} bytes, over 65000")
        ranges.append((start, end))
    groups = {(source, target): set() for source in range(2) for target in range(2)}
    source = None
    for line in run(readelf, "-rW", elf).splitlines():
        heading = RELOCATIONS.match(line)
        if heading:
            name = heading.group(1)
            source = next((i for i, names in enumerate(SEGMENTS) if name in names), None)
            continue
        match = RELOC.match(line.strip())
        if source is None or not match:
            continue
        address = int(match.group(1), 16)
        value = int(match.group(2), 16) + int(match.group(3) + match.group(4), 16)
        target = next((i for i, (start, end) in enumerate(ranges) if start <= value < end), None)
        if address % 2:
            sys.exit(f"m68k-relocs: odd relocation address: {line.strip()}")
        if target is None:
            sys.exit(f"m68k-relocs: relocation points outside the code: {line.strip()}")
        groups[source, target].add((address - ranges[source][0], value - ranges[target][0]))
    ordered = [sorted(groups[key]) for key in sorted(groups)]
    data = struct.pack(">4H", *[len(each) for each in ordered])
    for each in ordered:
        for offset, target in each:
            data += struct.pack(">2H", offset, target)
    with open(output, "wb") as handle:
        handle.write(data)
    print(f"m68k-relocs: {sum(len(each) for each in ordered)} relocations, "
          f"segments of {' and '.join(str(end - start) for start, end in ranges)} bytes")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
