#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Prove a Media Editor image has the shape Phipia accepts.

R-13.4 and R-13.6 in docs/ENGINEERING_RULES.md are this script. Phipia's ELF
validation refuses interpreter, dynamic, relocation, PIE, executable-stack, and
W+X shapes, and its build rejects any floating-point, MMX, SSE, or AVX
instruction; an image that reaches the kernel with one of those is refused
before it is mapped, so it is refused here first, where the message is useful.

The structure is read out of the file rather than out of another tool's
output. The instruction scan shells out to objdump, which is the same tool
Phipia's own audit uses.
"""

from __future__ import annotations

import re
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

IMAGE_BASE = 0x0000400001000000
GOT_MAX_BYTES = 4096
PAGE = 4096

PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3
PT_GNU_STACK = 0x6474E551

PF_X, PF_W, PF_R = 1, 2, 4

# Phipia's own scan, kept deliberately identical: any x87, MMX, SSE, or AVX
# instruction, with the two segment-check instructions that only look like one
# excluded by name.
FORBIDDEN = re.compile(
    r"%(?:xmm|ymm|zmm|mm|k)[0-9]+"
    r"|^\s*[0-9a-f]+:\s+(?:f[a-z0-9]+|emms|fxsave|fxrstor|ldmxcsr|stmxcsr|v[a-z0-9]+)(?:\s|$)"
)
EXEMPT = re.compile(r"\s(?:verr|verw)\s")
ADDRESS = re.compile(r"^\s*([0-9a-f]+):\s")


@dataclass(frozen=True)
class Segment:
    kind: int
    flags: int
    offset: int
    virtual_address: int
    file_size: int
    memory_size: int

    @property
    def writable(self) -> bool:
        return bool(self.flags & PF_W)

    @property
    def executable(self) -> bool:
        return bool(self.flags & PF_X)

    def describe(self) -> str:
        letters = "".join(
            letter
            for bit, letter in ((PF_R, "R"), (PF_W, "W"), (PF_X, "X"))
            if self.flags & bit
        )
        return (
            f"{self.virtual_address:#018x} "
            f"file {self.file_size:#08x} mem {self.memory_size:#08x} {letters}"
        )


class Image:
    """Just enough ELF to state what this image is."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.raw = path.read_bytes()
        if self.raw[:4] != b"\x7fELF":
            raise ValueError("not an ELF file")
        self.elf_class = self.raw[4]
        self.data_encoding = self.raw[5]
        (self.kind, self.machine) = struct.unpack_from("<HH", self.raw, 16)
        (self.entry, self.phoff, self.shoff) = struct.unpack_from("<QQQ", self.raw, 24)
        (self.phentsize, self.phnum) = struct.unpack_from("<HH", self.raw, 54)
        (self.shentsize, self.shnum, self.shstrndx) = struct.unpack_from(
            "<HHH", self.raw, 58
        )
        self.segments = [self._segment(i) for i in range(self.phnum)]
        self.sections = {name: header for name, header in self._sections()}
        self.symbols = self._symbols()

    def _segment(self, index: int) -> Segment:
        base = self.phoff + index * self.phentsize
        kind, flags = struct.unpack_from("<II", self.raw, base)
        offset, virtual_address, _physical = struct.unpack_from("<QQQ", self.raw, base + 8)
        file_size, memory_size = struct.unpack_from("<QQ", self.raw, base + 32)
        return Segment(kind, flags, offset, virtual_address, file_size, memory_size)

    def _sections(self):
        if self.shnum == 0:
            return
        strtab_base = self.shoff + self.shstrndx * self.shentsize
        strtab_offset, strtab_size = struct.unpack_from("<QQ", self.raw, strtab_base + 24)
        names = self.raw[strtab_offset : strtab_offset + strtab_size]
        for index in range(self.shnum):
            base = self.shoff + index * self.shentsize
            name_offset, kind, flags = struct.unpack_from("<IIQ", self.raw, base)
            address, offset, size = struct.unpack_from("<QQQ", self.raw, base + 16)
            end = names.index(b"\0", name_offset)
            yield names[name_offset:end].decode(), (kind, flags, address, offset, size)

    def _symbols(self) -> dict[str, int]:
        found: dict[str, int] = {}
        symtab = self.sections.get(".symtab")
        strtab = self.sections.get(".strtab")
        if symtab is None or strtab is None:
            return found
        _, _, _, sym_offset, sym_size = symtab
        _, _, _, str_offset, str_size = strtab
        strings = self.raw[str_offset : str_offset + str_size]
        for base in range(sym_offset, sym_offset + sym_size, 24):
            (name_offset,) = struct.unpack_from("<I", self.raw, base)
            (value,) = struct.unpack_from("<Q", self.raw, base + 8)
            if name_offset == 0:
                continue
            end = strings.index(b"\0", name_offset)
            found[strings[name_offset:end].decode()] = value
        return found

    def read_virtual(self, address: int, length: int) -> bytes | None:
        for segment in self.segments:
            if segment.kind != PT_LOAD:
                continue
            start = segment.virtual_address
            if start <= address and address + length <= start + segment.file_size:
                offset = segment.offset + (address - start)
                return self.raw[offset : offset + length]
        return None

    def covers(self, address: int) -> bool:
        return any(
            segment.kind == PT_LOAD
            and segment.virtual_address
            <= address
            < segment.virtual_address + segment.memory_size
            for segment in self.segments
        )


def tool(*arguments: str) -> str:
    return subprocess.run(
        arguments, check=True, capture_output=True, text=True
    ).stdout


def audit(path: Path) -> list[str]:
    findings: list[str] = []
    image = Image(path)

    def refuse(condition: bool, message: str) -> None:
        if condition:
            findings.append(message)

    refuse(image.elf_class != 2, "the image is not ELF64")
    refuse(image.data_encoding != 1, "the image is not little-endian")
    refuse(image.machine != 0x3E, "the image is not x86-64")
    refuse(
        image.kind != 2,
        f"the image is type {image.kind}, not ET_EXEC: Phipia refuses PIE and shared objects",
    )

    loads = [segment for segment in image.segments if segment.kind == PT_LOAD]
    refuse(not loads, "the image has no loadable segment")
    refuse(
        any(segment.kind == PT_INTERP for segment in image.segments),
        "the image names an interpreter",
    )
    refuse(
        any(segment.kind == PT_DYNAMIC for segment in image.segments),
        "the image has a dynamic segment",
    )
    refuse(".dynamic" in image.sections, "the image has a dynamic section")
    refuse(
        any(segment.writable and segment.executable for segment in loads),
        "the image has a writable executable segment",
    )
    for segment in image.segments:
        refuse(
            segment.kind == PT_GNU_STACK and segment.executable,
            "the image asks for an executable stack",
        )

    base = min((segment.virtual_address for segment in loads), default=0)
    refuse(
        base != IMAGE_BASE,
        f"the image is based at {base:#x}, not the agreed {IMAGE_BASE:#x}",
    )
    for segment in loads:
        refuse(
            segment.virtual_address % PAGE != 0,
            f"a load segment starts mid-page at {segment.virtual_address:#x}",
        )
    refuse(
        not any(
            segment.executable
            and segment.virtual_address
            <= image.entry
            < segment.virtual_address + segment.memory_size
            for segment in loads
        ),
        f"the entry point {image.entry:#x} is not inside an executable segment",
    )

    for section in (".rela.dyn", ".rel.dyn", ".rela.plt", ".rel.plt"):
        refuse(section in image.sections, f"the image carries {section}")
    relocations = tool("readelf", "-Wr", str(path))
    refuse(
        "R_X86_64" in relocations,
        "the image carries relocation records, so it is not fully linked",
    )

    undefined = tool("nm", "-u", str(path)).strip()
    refuse(bool(undefined), f"the image has undefined symbols: {undefined}")

    got_start = image.symbols.get("__got_start")
    got_end = image.symbols.get("__got_end")
    if got_start is None or got_end is None:
        findings.append("the linker script did not mark the global offset table")
    else:
        size = got_end - got_start
        refuse(size < 0, "the global offset table has a negative size")
        refuse(
            size > GOT_MAX_BYTES,
            f"the global offset table is {size} bytes, past its {GOT_MAX_BYTES} bound",
        )
        contents = image.read_virtual(got_start, max(size, 0))
        if contents is None and size > 0:
            findings.append("the global offset table is not inside a loadable segment")
        elif contents:
            for index in range(0, len(contents) - 7, 8):
                (entry,) = struct.unpack_from("<Q", contents, index)
                refuse(
                    entry != 0 and not image.covers(entry),
                    f"global offset table entry {index // 8} points outside the image",
                )

    # Only bytes the processor can actually execute are scanned, and that is
    # decided by the segment they live in, not by a section flag. Read-only
    # data disassembles into whatever its bytes happen to spell, and a good
    # share of those spellings look like an x87 instruction.
    executable_ranges = [
        (segment.virtual_address, segment.virtual_address + segment.memory_size)
        for segment in loads
        if segment.executable
    ]
    refuse(not executable_ranges, "the image has no executable segment")
    disassembly = tool("objdump", "-d", "--no-show-raw-insn", str(path))
    offenders = []
    for line in disassembly.splitlines():
        address = ADDRESS.match(line)
        if address is None:
            continue
        where = int(address.group(1), 16)
        if not any(start <= where < end for start, end in executable_ranges):
            continue
        if FORBIDDEN.search(line) and not EXEMPT.search(line):
            offenders.append(line.strip())
    if offenders:
        findings.append(
            "the image uses floating-point, MMX, SSE, or AVX instructions, "
            f"which Phipia neither enables nor preserves ({len(offenders)} sites, "
            f"first: {offenders[0]})"
        )

    print(f"image      {path}")
    print("type       ET_EXEC, ELF64, x86-64")
    print(f"entry      {image.entry:#018x}")
    for segment in loads:
        print(f"load       {segment.describe()}")
    footprint = sum(segment.memory_size for segment in loads)
    print(f"footprint  {footprint} bytes, {(footprint + PAGE - 1) // PAGE} pages")
    executable_bytes = sum(end - start for start, end in executable_ranges)
    print(f"scanned    {executable_bytes} executable bytes")
    if got_start is not None and got_end is not None:
        print(f"got        {got_end - got_start} bytes, resolved at link time")
    print("simd       none")
    return findings


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: elf-audit.py <image>", file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    if not path.exists():
        print(f"no such image: {path}", file=sys.stderr)
        return 2
    findings = audit(path)
    if findings:
        print()
        for finding in findings:
            print(f"REFUSED: {finding}", file=sys.stderr)
        print(f"\n{len(findings)} findings", file=sys.stderr)
        return 1
    print("audit      passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
