#!/usr/bin/env python3
"""Pack Phipia's pre-rasterized, antialiased Inter UI atlas as SUF2.

The committed atlas and metrics are the reproducible build inputs. Creating
them from the pinned InterVariable.ttf is a developer-only step performed by
``tools/rasterize-inter-ui.py``; ordinary builds require only Python's standard
library and never carry a TrueType parser into the kernel.
"""

from __future__ import annotations

import hashlib
import struct
import sys
import zlib
from pathlib import Path


MAGIC = b"SUF2"
VERSION = 2
HEADER_LENGTH = 24
WIDTH = 16
HEIGHT = 19
ASCENT = 15
DESCENT = 4
MAX_ADVANCE = 15
ROW_BYTES = WIDTH
FIRST = 0x20
COUNT = 0x7F - FIRST


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"UI font source refusal: {message}")


def unfilter(raw: bytes, width: int, height: int) -> bytes:
    stride = width
    output = bytearray()
    previous = bytearray(stride)
    offset = 0
    for _ in range(height):
        if offset + stride + 1 > len(raw):
            fail("truncated PNG scanline")
        kind = raw[offset]
        line = bytearray(raw[offset + 1:offset + 1 + stride])
        offset += stride + 1
        for index in range(stride):
            left = line[index - 1] if index else 0
            up = previous[index]
            upper_left = previous[index - 1] if index else 0
            if kind == 1:
                line[index] = (line[index] + left) & 0xFF
            elif kind == 2:
                line[index] = (line[index] + up) & 0xFF
            elif kind == 3:
                line[index] = (line[index] + (left + up) // 2) & 0xFF
            elif kind == 4:
                estimate = left + up - upper_left
                distances = (abs(estimate - left), abs(estimate - up),
                             abs(estimate - upper_left))
                nearest = (left, up, upper_left)[distances.index(min(distances))]
                line[index] = (line[index] + nearest) & 0xFF
            elif kind != 0:
                fail("unsupported PNG filter")
        output += line
        previous = line
    if offset != len(raw):
        fail("unexpected PNG scanline tail")
    return bytes(output)


def read_atlas(path: Path) -> bytes:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        fail("atlas is not a PNG")
    offset = 8
    header = None
    compressed = bytearray()
    while offset + 12 <= len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        end = offset + 12 + length
        if end > len(data):
            fail("truncated PNG chunk")
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
        offset = end
    if header is None:
        fail("atlas PNG has no IHDR")
    width, height, depth, colour, compression, filtering, interlace = header
    if (width, height) != (WIDTH * COUNT, HEIGHT):
        fail(f"atlas geometry must be {WIDTH * COUNT}x{HEIGHT}")
    if (depth, colour, compression, filtering, interlace) != (8, 0, 0, 0, 0):
        fail("atlas must be a non-interlaced 8-bit grayscale PNG")
    try:
        raw = zlib.decompress(bytes(compressed))
    except zlib.error as error:
        fail(f"atlas decompression failed: {error}")
    rows = unfilter(raw, width, height)
    glyphs = bytearray()
    for glyph in range(COUNT):
        glyph_x = glyph * WIDTH
        for y in range(HEIGHT):
            start = y * width + glyph_x
            glyphs += rows[start:start + WIDTH]
    return bytes(glyphs)


def read_advances(path: Path) -> bytes:
    values: dict[int, int] = {}
    for number, raw in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 2:
            fail(f"bad metrics line {number}")
        try:
            code = int(parts[0], 16)
            advance = int(parts[1], 10)
        except ValueError:
            fail(f"non-numeric metrics line {number}")
        if code in values or not FIRST <= code < FIRST + COUNT:
            fail(f"bad or duplicate code point on line {number}")
        if not 1 <= advance <= MAX_ADVANCE:
            fail(f"bad advance on line {number}")
        values[code] = advance
    missing = [code for code in range(FIRST, FIRST + COUNT) if code not in values]
    if missing:
        fail(f"missing advance U+{missing[0]:04X}")
    return bytes(values[code] for code in range(FIRST, FIRST + COUNT))


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: make-ui-font-asset.py ATLAS.png METRICS.txt OUTPUT.suf"
        )
    atlas = Path(sys.argv[1])
    metrics = Path(sys.argv[2])
    output = Path(sys.argv[3])
    advances = read_advances(metrics)
    bitmaps = read_atlas(atlas)
    glyph_bytes = WIDTH * HEIGHT
    data = bytearray()
    for index, advance in enumerate(advances):
        data.append(advance)
        start = index * glyph_bytes
        data += bitmaps[start:start + glyph_bytes]
    header = struct.pack(
        "<4s8BIII", MAGIC, VERSION, HEADER_LENGTH, WIDTH, HEIGHT,
        ASCENT, DESCENT, MAX_ADVANCE, ROW_BYTES, FIRST, COUNT, len(data)
    )
    blob = header + data
    output.write_bytes(blob)
    digest = hashlib.sha256(blob).hexdigest().upper()
    print(
        f"{atlas} + {metrics}: Inter UI {COUNT} glyphs, {WIDTH}x{HEIGHT} "
        f"alpha, {len(blob)} bytes, SHA-256 {digest} -> {output}"
    )


if __name__ == "__main__":
    main()
