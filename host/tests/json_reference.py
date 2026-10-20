# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference output for json_dump.c: the same lines, produced by Python's json."""

import json
import sys

KEY_CAPACITY = 24  # JSON_KEY_CAPACITY in src/core/json_scan.h
MAX_DEPTH = 24  # JSON_MAX_DEPTH: deeper values are not reported


def path_text(path):
    return "/".join(path)


def walk(value, path, out):
    if len(path) > MAX_DEPTH:
        return
    if isinstance(value, dict):
        for key, child in value.items():
            walk(child, path + [key[: KEY_CAPACITY - 1]], out)
        if len(path) < MAX_DEPTH:
            out.append(f"{path_text(path)}\tend")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            walk(child, path + [f"#{index}"], out)
        if len(path) < MAX_DEPTH:
            out.append(f"{path_text(path)}\tend")
    elif value is True:
        out.append(f"{path_text(path)}\tt\ttrue")
    elif value is False:
        out.append(f"{path_text(path)}\tf\tfalse")
    elif value is None:
        out.append(f"{path_text(path)}\tz\tnull")
    elif isinstance(value, Number):
        out.append(f"{path_text(path)}\tn\t{value}")
    else:
        out.append(f"{path_text(path)}\ts\t{value}")


class Number(str):
    pass


def main():
    with open(sys.argv[1], "rb") as handle:
        document = json.loads(handle.read().decode("utf-8"), parse_float=Number,
                              parse_int=Number)
    out = []
    walk(document, [], out)
    sys.stdout.buffer.write(("\n".join(out) + "\n").encode("utf-8", "surrogatepass"))


main()
