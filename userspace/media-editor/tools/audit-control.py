#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Prove the ELF audit is capable of refusing.

R-1.7: an invariant no test can break is not an invariant. This mutates a copy
of a built image in the two ways that matter most - making it a
position-independent executable, and putting an SSE instruction in its text -
and requires `tools/elf-audit.py` to refuse each one. The original image is
never touched.
"""

from __future__ import annotations

import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

AUDIT = Path(__file__).resolve().parent / "elf-audit.py"

# movaps %xmm0,%xmm0 - three bytes, valid, and exactly what Phipia's own scan
# of a userspace image is looking for.
MOVAPS = bytes((0x0F, 0x28, 0xC0))


def audit(path: Path) -> tuple[int, str]:
    finished = subprocess.run(
        [sys.executable, str(AUDIT), str(path)], capture_output=True, text=True
    )
    return finished.returncode, finished.stdout + finished.stderr


def entry_offset(raw: bytes) -> int:
    """Where the entry point lives in the file, through the load segments."""
    (entry,) = struct.unpack_from("<Q", raw, 24)
    (phoff,) = struct.unpack_from("<Q", raw, 32)
    phentsize, phnum = struct.unpack_from("<HH", raw, 54)
    for index in range(phnum):
        base = phoff + index * phentsize
        kind, _flags = struct.unpack_from("<II", raw, base)
        offset, virtual_address, _physical = struct.unpack_from("<QQQ", raw, base + 8)
        (file_size,) = struct.unpack_from("<Q", raw, base + 32)
        if kind == 1 and virtual_address <= entry < virtual_address + file_size:
            return offset + (entry - virtual_address)
    raise ValueError("the entry point is not in a loadable segment")


def control(name: str, mutate, expected: re.Pattern[str], image: Path) -> bool:
    raw = bytearray(image.read_bytes())
    mutate(raw)
    with tempfile.TemporaryDirectory() as directory:
        mutated = Path(directory) / image.name
        mutated.write_bytes(raw)
        code, output = audit(mutated)
    if code == 0:
        print(f"CONTROL FAILED: {name} was accepted", file=sys.stderr)
        return False
    if not expected.search(output):
        print(
            f"CONTROL FAILED: {name} was refused, but not by name:\n{output}",
            file=sys.stderr,
        )
        return False
    print(f"control    {name}: refused, as it must be")
    return True


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: audit-control.py <image>", file=sys.stderr)
        return 2
    image = Path(sys.argv[1])
    if not image.exists():
        print(f"no such image: {image}", file=sys.stderr)
        return 2

    code, output = audit(image)
    if code != 0:
        print(f"the unmutated image does not pass the audit:\n{output}", file=sys.stderr)
        return 1

    def make_position_independent(raw: bytearray) -> None:
        struct.pack_into("<H", raw, 16, 3)

    def add_an_sse_instruction(raw: bytearray) -> None:
        offset = entry_offset(bytes(raw))
        raw[offset : offset + len(MOVAPS)] = MOVAPS

    passed = control(
        "a position-independent image",
        make_position_independent,
        re.compile(r"not ET_EXEC"),
        image,
    )
    passed &= control(
        "an SSE instruction in the text",
        add_an_sse_instruction,
        re.compile(r"floating-point, MMX, SSE, or AVX"),
        image,
    )
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
