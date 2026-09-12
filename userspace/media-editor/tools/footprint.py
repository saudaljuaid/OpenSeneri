#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Say where the image's pages actually go.

`elf-audit.py` reports one number: the footprint in pages. That number has been
recorded in the platform contract after every change, and reading it as "the
program has grown" turned out to be wrong in a way that mattered — sixteen of
the forty-two pages are a single constant, `media_editor_rt::HEAP`, which is a
reservation rather than anything the program contains.

So this splits the number. It reads the loaded sections and the symbol table,
attributes every sized symbol to the crate that emitted it, and prints the
result largest first. What it produces is evidence for a decision that has to
be made eventually — whether to split the program so the freestanding image
links less of it — and evidence is what that decision has been missing.

It refuses nothing. A budget that failed a build would be a guess at what the
right size is, and nobody knows that until `PHIP-03` says what a program is
given. This measures; the platform contract judges.
"""

from __future__ import annotations

import re
import signal
import struct
import sys
from pathlib import Path

# This prints a report a person reads, so somebody will pipe it into `head` or
# `less` and close the pipe early. Without this that is a traceback rather than
# a clean stop, and a tool that appears to crash when it is read is a tool
# people stop reading.
if hasattr(signal, "SIGPIPE"):
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)

PAGE = 4096

#: Sections that occupy memory when the program runs.
LOADED = (".text", ".rodata", ".data", ".bss")


def sections(raw: bytes) -> list[tuple[str, int, int, int, int]]:
    """Every section: name, kind, address, file offset, size."""
    (shoff,) = struct.unpack_from("<Q", raw, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", raw, 0x3A)
    base = shoff + shstrndx * shentsize
    strtab_offset, strtab_size = struct.unpack_from("<QQ", raw, base + 24)
    names = raw[strtab_offset : strtab_offset + strtab_size]

    found = []
    for index in range(shnum):
        header = shoff + index * shentsize
        (name_offset, kind) = struct.unpack_from("<II", raw, header)
        address, offset, size = struct.unpack_from("<QQQ", raw, header + 16)
        end = names.index(b"\0", name_offset)
        found.append((names[name_offset:end].decode(), kind, address, offset, size))
    return found


def symbols(raw: bytes, table: list) -> list[tuple[int, str, str]]:
    """Every sized symbol: size, the section it is in, and its name."""
    by_name = {entry[0]: entry for entry in table}
    if ".symtab" not in by_name or ".strtab" not in by_name:
        return []
    _, _, _, sym_offset, sym_size = by_name[".symtab"]
    _, _, _, str_offset, str_size = by_name[".strtab"]
    strings = raw[str_offset : str_offset + str_size]

    found = []
    for base in range(sym_offset, sym_offset + sym_size, 24):
        (name_offset,) = struct.unpack_from("<I", raw, base)
        (section_index,) = struct.unpack_from("<H", raw, base + 6)
        (size,) = struct.unpack_from("<Q", raw, base + 16)
        if name_offset == 0 or size == 0:
            continue
        end = strings.index(b"\0", name_offset)
        name = strings[name_offset:end].decode()
        where = table[section_index][0] if section_index < len(table) else "?"
        found.append((size, where, name))
    return found


#: What a symbol whose crate cannot be read is filed under.
#:
#: A bucket rather than a refusal per symbol, because a few always exist --
#: assembly stubs, linker-provided symbols, anything without a Rust name at
#: all. What is *not* acceptable is this bucket holding most of the image,
#: which is exactly what happened for several commits: the reader below knew
#: only Rust's legacy mangling, the toolchain emits v0, and every symbol landed
#: here while the line underneath went on saying "attributed in total".
#:
#: `UNREADABLE_SHARE` is what turns that from a silent bucket into a finding.
UNKNOWN = "(crate unreadable)"

#: How much of the sized symbols may land in `UNKNOWN` before this refuses.
#:
#: Five per cent, against a measured 0.27% at the commit that taught this
#: reader v0 -- 965 bytes, and every one of them a compiler intrinsic with a C
#: name: `memcmp`, `memset`, `__udivti3`. Those carry no crate and never will.
#:
#: The bound is roughly twenty times the real figure on purpose. It is not
#: This limit detects a symbol reader that no longer recognises the toolchain's
#: mangling. That failure produces a sudden jump rather than gradual growth.
UNREADABLE_SHARE = 0.05

#: A crate root in Rust's v0 mangling: `C`, an optional disambiguator of
#: base-62 digits between `s` and `_`, then a length-prefixed identifier.
#:
#: The trailing `(_?)` is the separator v0 writes between the length and the
#: name when the name itself begins with `_` or a digit, so that the two cannot
#: run together. Reading it as part of the name shifts every such crate by one
#: character -- which is how `__rustc`, the compiler's own shim crate,
#: first appeared in this table as `___rust`.
V0_CRATE = re.compile(r"C(?:s[0-9a-zA-Z]*_)?([1-9][0-9]*)(_?)")


def named(crate: str) -> str:
    """File a crate name under its own name, or under the language's."""
    return crate if crate.startswith("media_editor") else f"(rust: {crate})"


def crate_of(symbol: str) -> str:
    """Which crate emitted a symbol, from its mangled name.

    Both manglings are read, because which one a build uses is the
    toolchain's decision and not this tool's. Legacy writes `_ZN` then
    length-prefixed path components, so the first component is the crate. v0
    writes `_R` then a path whose root is a `C` node -- see [`V0_CRATE`].

    A generic instantiated in one crate from another's code is attributed to
    whichever crate's name comes first, which is a rough edge and an honest
    one: the alternative is claiming a precision the mangling does not carry.
    """
    if symbol.startswith("_ZN"):
        rest = symbol[3:]
        digits = ""
        while rest and rest[0].isdigit():
            digits += rest[0]
            rest = rest[1:]
        if not digits:
            return UNKNOWN
        return named(rest[: int(digits)])
    if symbol.startswith("_R"):
        # The first `C` that parses as a crate root wins. A later one would be
        # a different crate's path appearing as a generic argument, and the
        # root comes first by the grammar's construction.
        for found in V0_CRATE.finditer(symbol):
            length = int(found.group(1))
            name = symbol[found.end() : found.end() + length]
            if len(name) == length and IDENTIFIER.fullmatch(name):
                return named(name)
        return UNKNOWN
    return UNKNOWN


#: What a length-prefixed component has to look like to be believed.
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

#: Symbols this reader is checked against, and the crate each belongs to.
#:
#: A tool that reads a format needs a case it is known to read, for the same
#: reason a parser needs a fixture: the alternative is finding out from a
#: table that has quietly become one row. Each of these is a real symbol
#: lifted from a real build of this image.
WITNESSES = (
    # v0, a value at a crate root.
    ("_RNvCs6BcOe5Imze2_15media_editor_rt4HEAP", "media_editor_rt"),
    # v0, a method on a type in a module.
    (
        "_RNvMNtCs6oV4Mx6d6CN_18media_editor_model4editNtB2_4Edit5apply",
        "media_editor_model",
    ),
    # v0, whose crate name begins with an underscore and therefore carries the
    # separator that this reader once ate.
    ("_RNvCs6rREvFdRhLb_7___rustc12___rust_alloc", "(rust: __rustc)"),
    # Legacy, still read because which mangling a build uses is the
    # toolchain's decision and not this tool's.
    ("_ZN15media_editor_io6format6encode17h0123456789abcdefE", "media_editor_io"),
    # A C name, which carries no crate at all and must say so rather than
    # being filed under a plausible-looking guess.
    ("memcmp", UNKNOWN),
)


def self_check() -> list[str]:
    """What this reader gets wrong on the symbols it is known to face."""
    return [
        f"{symbol} reads as {crate_of(symbol)}, not {wanted}"
        for symbol, wanted in WITNESSES
        if crate_of(symbol) != wanted
    ]


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: footprint.py <image>", file=sys.stderr)
        return 2
    wrong = self_check()
    if wrong:
        for finding in wrong:
            print(f"footprint  {finding}", file=sys.stderr)
        return 1
    raw = Path(sys.argv[1]).read_bytes()
    table = sections(raw)
    sizes = {name: size for name, _, _, _, size in table}

    total = sum(sizes.get(name, 0) for name in LOADED)
    print(f"footprint  {total} bytes, {(total + PAGE - 1) // PAGE} pages\n")

    print("by section")
    for name in LOADED:
        size = sizes.get(name, 0)
        if size:
            share = 100 * size / total
            print(f"  {name:9s} {size:8d}  {size / PAGE:5.1f} pages  {share:4.1f}%")

    held = symbols(raw, table)
    if not held:
        print("\nno symbol table: build with debug information to see the rest")
        return 0

    print("\nlargest single symbols")
    for size, where, name in sorted(held, reverse=True)[:10]:
        print(f"  {size:8d}  {where:8s} {name[:64]}")

    per_crate: dict[str, int] = {}
    for size, _, name in held:
        crate = crate_of(name)
        per_crate[crate] = per_crate.get(crate, 0) + size
    attributed = sum(per_crate.values())

    print("\nby crate, from the symbols that carry a size")
    for crate, size in sorted(per_crate.items(), key=lambda pair: -pair[1]):
        print(f"  {size:8d}  {size / PAGE:5.1f} pages  {crate}")
    print(f"  {attributed:8d}  {attributed / PAGE:5.1f} pages  read from the mangling")
    print(
        f"  {total - attributed:8d}  {(total - attributed) / PAGE:5.1f} pages  "
        "not attributed: padding, literals, and anything the table does not size"
    )

    # The check on the check. A reader that has stopped understanding the
    # mangling still produces a table, and the table still adds up -- it just
    # has one row in it. Saying so is the difference between a measurement and
    # a shape that looks like one.
    unreadable = per_crate.get(UNKNOWN, 0)
    if attributed and unreadable > attributed * UNREADABLE_SHARE:
        share = 100 * unreadable / attributed
        print(
            f"\nfootprint  {share:.1f}% of the sized symbols have a name this "
            f"reader cannot parse, past the {100 * UNREADABLE_SHARE:.0f}% bound",
            file=sys.stderr,
        )
        print(
            "footprint  the toolchain's mangling has changed, or the image "
            "holds symbols from something that is not rustc",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
