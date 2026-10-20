#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Gives each 68K object its own long-call veneers.

68000 code built with -mpcrel calls other functions PC-relatively, which
reaches only +/-32 KB. The app is larger than that, so a call from one
object to a function in another may not reach. For every function an
object calls outside itself (including libgcc and libc helpers), this adds
a 12-byte veneer to that object, and points the object's calls at it:

    lea     1f(%pc), %a0
    adda.l  #target-1f, %a0     | 32-bit PC-relative: reaches anything
    jmp     (%a0)               | a0 is a scratch register in the ABI
  1:

The veneer sits in the same object as its callers, so they always reach it.
Nothing is patched at run time. Only functions get veneers: those defined
in the objects, and those in the --lib archives (libgcc, libc). Anything
else, such as linker script symbols, is left alone.

  m68k-veneers.py TOOL_PREFIX OUT_DIR ROOT [--lib LIB.a]... OBJECT...

Each ROOT/path/name.o is written to OUT_DIR/path/name.o.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

RELOC = re.compile(r"^[0-9a-f]+\s+[0-9a-f]+\s+(R_68K_\w+)\s+[0-9a-f]+\s+(\S+)\s*([+-])\s*([0-9a-f]+)")


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def defined_symbols(prefix, path):
    """Names this object defines, and which of them are functions."""
    names, functions = set(), set()
    for line in run(prefix + "readelf", "-sW", path).splitlines():
        fields = line.split()
        if len(fields) < 8 or not fields[0].endswith(":") or fields[6] == "UND":
            continue
        names.add(fields[7])
        if fields[3] == "FUNC":
            functions.add(fields[7])
    return names, functions


def library_functions(prefix, archive):
    functions = set()
    for line in run(prefix + "nm", "--defined-only", archive).splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1] in "TW":
            functions.add(fields[2])
    return functions


def far_targets(prefix, path, own, functions):
    """Functions outside this object that it reaches PC-relatively."""
    targets = set()
    for line in run(prefix + "readelf", "-rW", path).splitlines():
        match = RELOC.match(line.strip())
        if not match or match.group(1) != "R_68K_PC16":
            continue
        name, addend = match.group(2), int(match.group(4), 16)
        if name in own or name not in functions:
            continue
        if addend != 0:
            sys.exit(f"m68k-veneers: {path}: {name} + {addend}: not a plain call")
        targets.add(name)
    return targets


def main(prefix, out_dir, root, libs, objects):
    symbols = {path: defined_symbols(prefix, path) for path in objects}
    functions = set()
    for lib in libs:
        functions |= library_functions(prefix, lib)
    for _, defined in symbols.values():
        functions |= defined
    for path in objects:
        own = symbols[path][0]
        targets = sorted(far_targets(prefix, path, own, functions))
        tag = re.sub(r"\W", "_", os.path.splitext(os.path.relpath(path, root))[0])
        out = os.path.join(out_dir, os.path.relpath(path, root))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        if not targets:
            run(prefix + "objcopy", path, out)
            continue
        with tempfile.TemporaryDirectory() as work:
            source = os.path.join(work, "veneers.s")
            with open(source, "w") as handle:
                for name in targets:
                    veneer = f"{name}__far_{tag}"
                    handle.write(
                        f"\t.section .text.{veneer},\"ax\",@progbits\n"
                        f"\t.hidden {veneer}\n\t.globl {veneer}\n{veneer}:\n"
                        f"\tlea 1f(%pc), %a0\n\tadda.l #{name}-1f, %a0\n"
                        f"\tjmp (%a0)\n1:\n")
            run(prefix + "as", "-m68000", source, "-o", os.path.join(work, "veneers.o"))
            renamed = os.path.join(work, "renamed.o")
            run(prefix + "objcopy",
                *[f"--redefine-sym={name}={name}__far_{tag}" for name in targets],
                path, renamed)
            run(prefix + "ld", "-r", renamed, os.path.join(work, "veneers.o"), "-o", out)
    print(f"m68k-veneers: {len(objects)} objects")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(usage=__doc__)
    parser.add_argument("prefix")
    parser.add_argument("out_dir")
    parser.add_argument("root")
    parser.add_argument("--lib", action="append", default=[])
    parser.add_argument("objects", nargs="+")
    args = parser.parse_args()
    main(args.prefix, args.out_dir, args.root, args.lib, args.objects)
