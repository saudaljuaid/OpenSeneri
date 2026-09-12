#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Require exact serial-log marker counts in the declared phase order."""

from __future__ import annotations

from pathlib import Path
import sys


def main() -> int:
    arguments = sys.argv[1:]
    count = 1
    if arguments[:1] == ["--count"]:
        if len(arguments) < 2 or not arguments[1].isdecimal():
            raise SystemExit("--count requires a positive decimal integer")
        count = int(arguments[1])
        if count < 1:
            raise SystemExit("--count requires a positive decimal integer")
        arguments = arguments[2:]
    if len(arguments) < 3:
        raise SystemExit(
            "usage: serial-marker-order.py [--count COUNT] "
            "LOG MARKER MARKER [MARKER ...]")
    lines = Path(arguments[0]).read_text(
        encoding="utf-8", errors="strict").splitlines()
    cursor = -1
    for marker in arguments[1:]:
        positions = [index for index, line in enumerate(lines) if line == marker]
        if len(positions) != count:
            raise SystemExit(
                f"serial marker must appear exactly {count} time(s): "
                f"{marker!r} (found {len(positions)})")
        if positions[0] <= cursor:
            raise SystemExit(f"serial marker is out of order: {marker!r}")
        cursor = positions[-1]
    print(
        f"ordered serial lifecycle passed: {len(arguments) - 1} "
        f"markers x {count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
