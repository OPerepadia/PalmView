#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Packages a native ARM module as Palm OS resources.

Code is built position-independent (-fPIC -msingle-pic-base -mlong-calls) and
split into resources of under 64 KB that run in place, unmodified. Globals
and calls between files go through the GOT, which lives with the writable
data in one heap block that the 68K loader fixes up.

  armlink.py layout MAP SCRIPT     segment the code using a first-pass map
  armlink.py package ELF OUTDIR    check the final link and write resources

Resources written by package (all integers big-endian):
  armh 1000        header
  armc 1001+i      code segment i, with any branch veneers appended
  armd 2001+i      initial writable data (GOT and .data), in chunks
  armr 3001+i      fixups: (rw offset, region << 24 | offset) pairs
"""

import os
import re
import struct
import sys

SEGMENT_LIMIT = 56000      # code bytes per segment; veneers come on top
RESOURCE_LIMIT = 64000     # Palm OS resources must stay below 64 KB
CHUNK_LIMIT = 60000
RW_REGION = 0xFF
VENEER_SIZE = 12
HEADER_MAGIC = 0x5054414D  # 'PTAM'
HEADER_VERSION = 1

SECTION_PATTERNS = ".text .text.* .rodata .rodata.*"
RW_SCRIPT = """
  . = ALIGN(16);
  __rw_start = .;
  .rw : {
    __got_start = .;
    *(.got.plt) *(.igot.plt) *(.got) *(.igot)
    __got_end = .;
    *(.data .data.* .data.rel.ro .data.rel.ro.*)
    . = ALIGN(16);
  }
  .bss (NOLOAD) : {
    *(.bss .bss.* COMMON)
    . = ALIGN(16);
  }
  __rw_end = .;
  .init_array : { *(.init_array .init_array.*) }
  .fini_array : { *(.fini_array .fini_array.*) }
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
"""


def fail(message):
    sys.exit(f"armlink: {message}")


# --- layout ---------------------------------------------------------------

def parse_map_sections(path):
    """Input sections placed in the first pass's .text, in address order."""
    sections = []
    in_text = False
    pending = None
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if re.match(r"^\.text\s", line) or line == ".text":
                in_text = True
                continue
            if in_text and re.match(r"^\S", line):
                break
            if not in_text:
                continue
            match = re.match(r"^ (\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$", line)
            if match:
                sections.append((match.group(1), int(match.group(2), 16),
                                 int(match.group(3), 16), match.group(4)))
                pending = None
                continue
            match = re.match(r"^ (\.\S+)$", line)
            if match:
                pending = match.group(1)
                continue
            match = re.match(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$", line)
            if match and pending:
                sections.append((pending, int(match.group(1), 16),
                                 int(match.group(2), 16), match.group(3)))
                pending = None
    return [s for s in sections if s[2] > 0]


def file_pattern(source):
    """ld script file pattern for a map file source."""
    match = re.match(r"^(.*)\(([^)]+)\)$", source)
    if match:
        return f"*{os.path.basename(match.group(1))}:{match.group(2)}"
    return source


def layout(map_path, script_path):
    sections = parse_map_sections(map_path)
    if not sections:
        fail(f"no .text input sections found in {map_path}")
    groups = []
    for name, _address, size, source in sections:
        if groups and groups[-1][0] == source:
            groups[-1][1].append((name, size))
        else:
            groups.append((source, [(name, size)]))

    def aligned(size):
        return (size + 15) & ~15

    segments = [[]]
    used = 0
    for source, items in groups:
        total = sum(aligned(size) for _, size in items)
        if total > SEGMENT_LIMIT:
            # Split a large file by function; veneers bridge the gaps.
            for name, size in items:
                if aligned(size) > SEGMENT_LIMIT:
                    fail(f"section {name} in {source} is {size} bytes")
                if used + aligned(size) > SEGMENT_LIMIT:
                    segments.append([])
                    used = 0
                segments[-1].append((source, [name]))
                used += aligned(size)
            continue
        if used + total > SEGMENT_LIMIT:
            segments.append([])
            used = 0
        segments[-1].append((source, [name for name, _ in items]))
        used += total

    lines = ["ENTRY(ArmEntry)", "SECTIONS", "{", "  . = 0;"]
    for index, members in enumerate(segments):
        lines.append(f"  . = ALIGN(16);")
        lines.append(f"  .seg{index} : {{")
        if index == 0:
            lines.append("    KEEP(*(.text.ArmEntry))")
        for source, names in members:
            pattern = file_pattern(source)
            lines.append(f"    {pattern}({' '.join(sorted(set(names)))})")
        lines.append("  }")
    lines.append("  . = ALIGN(16);")
    lines.append(f"  .segrest : {{ *({SECTION_PATTERNS}) *(.glue_7 .glue_7t "
                 f".vfp11_veneer .v4_bx) }}")
    lines.append(RW_SCRIPT)
    lines.append("}")
    with open(script_path, "w") as handle:
        handle.write("\n".join(lines) + "\n")
    print(f"armlink: {len(sections)} sections in {len(segments)} segments")


def first_pass_script(path):
    with open(path, "w") as handle:
        handle.write("ENTRY(ArmEntry)\nSECTIONS\n{\n  . = 0;\n")
        handle.write(f"  .text : {{ KEEP(*(.text.ArmEntry)) *({SECTION_PATTERNS}) "
                     f"*(.glue_7 .glue_7t .vfp11_veneer .v4_bx) }}\n")
        handle.write(RW_SCRIPT + "}\n")


# --- ELF reading ----------------------------------------------------------

class Elf:
    def __init__(self, path):
        with open(path, "rb") as handle:
            self.data = handle.read()
        if self.data[:4] != b"\x7fELF" or self.data[4] != 1 or self.data[5] != 1:
            fail("expected a 32-bit little-endian ELF")
        (shoff,) = struct.unpack_from("<I", self.data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", self.data, 0x2E)
        raw = [struct.unpack_from("<IIIIIIIIII", self.data, shoff + i * shentsize)
               for i in range(shnum)]
        names = raw[shstrndx]
        self.sections = []
        for index, (name, kind, flags, addr, offset, size, link, info, align,
                    entsize) in enumerate(raw):
            self.sections.append(dict(
                index=index, name=self.string(names[4], name), type=kind,
                flags=flags, addr=addr, offset=offset, size=size, link=link,
                info=info, align=align, entsize=entsize))
        self.symbols = []
        for section in self.sections:
            if section["type"] == 2:  # SHT_SYMTAB
                strtab = self.sections[section["link"]]["offset"]
                for i in range(section["size"] // 16):
                    name, value, size, info, other, shndx = struct.unpack_from(
                        "<IIIBBH", self.data, section["offset"] + i * 16)
                    self.symbols.append((self.string(strtab, name), value, shndx))

    def string(self, table, offset):
        end = self.data.index(b"\0", table + offset)
        return self.data[table + offset:end].decode()

    def section(self, name):
        for section in self.sections:
            if section["name"] == name:
                return section
        return None

    def symbol(self, name):
        for symbol_name, value, _ in self.symbols:
            if symbol_name == name:
                return value
        fail(f"missing symbol {name}")

    def contents(self, section):
        return bytearray(self.data[section["offset"]:section["offset"] + section["size"]])

    def relocations(self, target_index):
        for section in self.sections:
            if section["type"] == 9 and section["info"] == target_index:  # SHT_REL
                for i in range(section["size"] // 8):
                    offset, info = struct.unpack_from(
                        "<II", self.data, section["offset"] + i * 8)
                    yield offset, info & 0xFF, info >> 8
            elif section["type"] == 4 and section["info"] == target_index:
                fail("RELA relocations are not supported")

    def symbol_name(self, index):
        return self.symbols[index][0] if index < len(self.symbols) else f"#{index}"


# --- package --------------------------------------------------------------

R_ARM_NONE, R_ARM_PC24, R_ARM_ABS32, R_ARM_REL32 = 0, 1, 2, 3
R_ARM_GOTOFF32, R_ARM_BASE_PREL, R_ARM_GOT_BREL = 24, 25, 26
R_ARM_CALL, R_ARM_JUMP24, R_ARM_TARGET1, R_ARM_V4BX = 28, 29, 38, 40
R_ARM_PREL31 = 42
BRANCHES = {R_ARM_PC24, R_ARM_CALL, R_ARM_JUMP24}
GOT_RELATIVE = {R_ARM_GOTOFF32, R_ARM_GOT_BREL}


def package(elf_path, out_dir):
    elf = Elf(elf_path)
    segments = sorted((s for s in elf.sections if re.fullmatch(r"\.seg\d+", s["name"])),
                      key=lambda s: int(s["name"][4:]))
    rest = elf.section(".segrest")
    if rest is not None and rest["size"] != 0:
        fail(".segrest is not empty: code was not placed in a segment")
    for name in (".init_array", ".fini_array"):
        section = elf.section(name)
        if section is not None and section["size"] != 0:
            fail(f"{name} is not empty: static constructors are not supported")
    rw = elf.section(".rw")
    bss = elf.section(".bss")
    rw_start = elf.symbol("__rw_start")
    rw_end = elf.symbol("__rw_end")
    got_start = elf.symbol("__got_start")
    got_end = elf.symbol("__got_end")
    got_base = elf.symbol("_GLOBAL_OFFSET_TABLE_")
    if rw is None or rw["addr"] != rw_start:
        fail(".rw must start the writable region")
    rw_image = elf.contents(rw)

    regions = [(s["addr"], s["addr"] + s["size"]) for s in segments]

    def region_of(address):
        # A segment's end is also the next one's start: starts win.
        for index, (start, end) in enumerate(regions):
            if start <= address < end:
                return index, address - start
        if rw_start <= address < rw_end:
            return RW_REGION, address - rw_start
        for index, (start, end) in enumerate(regions):
            if address == end:
                return index, address - start
        if address == rw_end:
            return RW_REGION, address - rw_start
        return None

    def segment_of(address):
        for index, (start, end) in enumerate(regions):
            if start <= address < end:
                return index
        return None

    fixups = {}
    slots = {}            # target address -> rw offset of its slot
    rw_total = rw_end - rw_start
    codes = [elf.contents(s) for s in segments]
    veneers = [dict() for _ in segments]
    errors = []

    def add_fixup(rw_offset, address, what):
        place = region_of(address)
        if place is None:
            errors.append(f"{what}: address 0x{address:x} is outside the module")
            return
        fixups[rw_offset] = place

    for index, section in enumerate(segments):
        code = codes[index]
        base = section["addr"]
        for offset, kind, symbol in elf.relocations(section["index"]):
            place = offset - base
            word = struct.unpack_from("<I", code, place)[0]
            what = f"{section['name']}+0x{place:x} ({elf.symbol_name(symbol)})"
            if kind in (R_ARM_NONE, R_ARM_V4BX) or kind in GOT_RELATIVE:
                continue
            if kind in BRANCHES:
                if word >> 28 == 0xF:
                    errors.append(f"{what}: BLX to Thumb code")
                    continue
                displacement = ((word & 0xFFFFFF) ^ 0x800000) - 0x800000
                target = offset + 8 + displacement * 4
                target_segment = segment_of(target)
                if target_segment == index:
                    continue
                if target_segment is None:
                    errors.append(f"{what}: branch outside the code")
                    continue
                if target not in slots:
                    slots[target] = rw_total
                    rw_total += 4
                    add_fixup(slots[target], target, what)
                if target not in veneers[index]:
                    veneers[index][target] = len(code) + len(veneers[index]) * VENEER_SIZE
                veneer = veneers[index][target]
                new = (veneer - (place + 8)) >> 2
                struct.pack_into("<I", code, place, (word & 0xFF000000) | (new & 0xFFFFFF))
                continue
            if kind in (R_ARM_REL32, R_ARM_PREL31):
                target = offset + (word if kind == R_ARM_REL32 else
                                   ((word & 0x7FFFFFFF) ^ 0x40000000) - 0x40000000)
                if segment_of(target) != index:
                    errors.append(f"{what}: PC-relative reference to another segment")
                continue
            errors.append(f"{what}: relocation type {kind} in code (needs patching)")

    # Absolute pointers in writable data, and every GOT entry.
    for section in elf.sections:
        if section["flags"] & 2 == 0 or section["type"] == 9:   # SHF_ALLOC
            continue
        if section in segments or section is rest:
            continue
        for offset, kind, symbol in elf.relocations(section["index"]):
            what = f"{section['name']}+0x{offset - section['addr']:x} ({elf.symbol_name(symbol)})"
            if section is not rw:
                errors.append(f"{what}: relocation in {section['name']}")
                continue
            word = struct.unpack_from("<I", rw_image, offset - rw_start)[0]
            if kind in (R_ARM_ABS32, R_ARM_TARGET1):
                add_fixup(offset - rw_start, word, what)
            elif kind == R_ARM_REL32:
                if region_of(offset + word) != (RW_REGION, offset + word - rw_start):
                    errors.append(f"{what}: data PC-relative reference leaves .rw")
            elif kind != R_ARM_NONE:
                errors.append(f"{what}: relocation type {kind} in data")
    for address in range(got_start, got_end, 4):
        word = struct.unpack_from("<I", rw_image, address - rw_start)[0]
        if word != 0:
            add_fixup(address - rw_start, word, f"GOT entry at 0x{address:x}")

    if errors:
        for error in errors[:40]:
            print(f"armlink: {error}", file=sys.stderr)
        fail(f"{len(errors)} problem(s)")

    got_offset = got_base - rw_start
    os.makedirs(out_dir, exist_ok=True)
    resources = []
    for index, code in enumerate(codes):
        for target, veneer in veneers[index].items():
            literal = slots[target] - got_offset
            code += struct.pack("<III", 0xE59FC000,           # ldr ip, [pc]
                                0xE799F00C,                   # ldr pc, [r9, ip]
                                literal & 0xFFFFFFFF)
        while len(code) % 4:
            code.append(0)
        if len(code) > RESOURCE_LIMIT:
            fail(f"segment {index} is {len(code)} bytes with veneers")
        codes[index] = code
        resources.append(("armc", 1001 + index, bytes(code)))
    data_chunks = [rw_image[i:i + CHUNK_LIMIT] for i in range(0, len(rw_image), CHUNK_LIMIT)]
    for index, chunk in enumerate(data_chunks):
        resources.append(("armd", 2001 + index, bytes(chunk)))
    packed = b"".join(struct.pack(">II", rw_offset, (region << 24) | value)
                      for rw_offset, (region, value) in sorted(fixups.items()))
    per_chunk = CHUNK_LIMIT // 8 * 8
    fixup_chunks = [packed[i:i + per_chunk] for i in range(0, len(packed), per_chunk)]
    for index, chunk in enumerate(fixup_chunks):
        resources.append(("armr", 3001 + index, chunk))

    entry = elf.symbol("ArmEntry")
    if segment_of(entry) != 0:
        fail("ArmEntry must be in segment 0")
    header = struct.pack(">IIHHHH", HEADER_MAGIC, HEADER_VERSION, len(codes),
                         len(data_chunks), len(fixup_chunks), 0)
    header += struct.pack(">IIIIII", entry - regions[0][0], len(rw_image), rw_total,
                          got_offset, len(fixups), 0)
    for index, code in enumerate(codes):
        header += struct.pack(">II", regions[index][0], len(code))
    resources.insert(0, ("armh", 1000, header))

    lines = []
    for kind, resource_id, data in resources:
        name = f"{kind}{resource_id}.bin"
        with open(os.path.join(out_dir, name), "wb") as handle:
            handle.write(data)
        lines.append(f'DATA "{kind}" ID {resource_id} "{name}"')
    with open(os.path.join(out_dir, "arm.rcp"), "w") as handle:
        handle.write("\n".join(lines) + "\n")
    code_bytes = sum(len(c) for c in codes)
    veneer_count = sum(len(v) for v in veneers)
    print(f"armlink: {len(codes)} code segments, {code_bytes} bytes "
          f"({veneer_count} veneers), {len(rw_image)} bytes data, "
          f"{rw_total} bytes writable, {len(fixups)} fixups")


def main(argv):
    if len(argv) == 3 and argv[0] == "layout":
        layout(argv[1], argv[2])
    elif len(argv) == 2 and argv[0] == "first-pass":
        first_pass_script(argv[1])
    elif len(argv) == 3 and argv[0] == "package":
        package(argv[1], argv[2])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
