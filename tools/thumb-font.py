#!/usr/bin/env python3
"""Writes src/core/thumb_font.h: the digits and ":" for the duration badge.

Usage: tools/thumb-font.py [path/to/LiberationSans-Bold.ttf]

Each pixel is a coverage from 0 to 15.
"""
import sys
from PIL import Image, ImageDraw, ImageFont

FONT = "/usr/share/fonts/liberation/LiberationSans-Bold.ttf"
SIZE = 11
TOP = 2
ROWS = 8
CELL = 7
COLON_CELL = 3
CHARS = "0123456789:"

font = ImageFont.truetype(sys.argv[1] if len(sys.argv) > 1 else FONT, SIZE)
glyphs = []
for char in CHARS:
    image = Image.new("L", (CELL, ROWS))
    ImageDraw.Draw(image).text((0, -TOP), char, font=font, fill=255)
    width = COLON_CELL if char == ":" else CELL
    rows = [[image.getpixel((x, y)) * 15 // 255 for x in range(CELL)]
            for y in range(ROWS)]
    assert all(row[x] == 0 for row in rows for x in range(width, CELL))
    glyphs.append((width, rows))

out = ["/* SPDX-License-Identifier: GPL-3.0-or-later */", "",
       "/* Made by tools/thumb-font.py: Liberation Sans Bold, 11 px. */", "",
       "#define FONT_ROWS %d" % ROWS, "#define FONT_CELL %d" % CELL, "",
       "/* The digits, then \":\" */",
       "static const UInt8 kGlyphWidths[%d] = { %s };" % (
           len(CHARS), ", ".join(str(width) for width, _ in glyphs)), "",
       "static const UInt8 kGlyphs[%d][FONT_ROWS][FONT_CELL] = {" % len(CHARS)]
for char, (_, rows) in zip(CHARS, glyphs):
    out.append("    { /* %s */" % char)
    out += ["        { %s }," % ", ".join("%2d" % v for v in row) for row in rows]
    out.append("    },")
out.append("};")
open("src/core/thumb_font.h", "w").write("\n".join(out) + "\n")
