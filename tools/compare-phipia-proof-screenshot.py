#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare actual Phipia QEMU frames using bounded stable regions."""

import argparse
import struct
import zlib
from pathlib import Path


WIDTH = 1024
HEIGHT = 768

# Phipia exposes no timing, addresses, counters, or other variable fields
# in these three frames. The documented variable-region set is intentionally
# empty, so a changed pixel anywhere is a refusal.
VARIABLE_REGIONS = {
    "clean": (),
    "focus": (),
    "terminal": (),
}
STABLE_REGIONS = {
    "clean": ((0, 0, WIDTH, HEIGHT, "clean desktop"),),
    "focus": ((0, 0, WIDTH, HEIGHT, "focused and hovered dock"),),
    "terminal": ((0, 0, WIDTH, HEIGHT, "terminal ledger result"),),
}


def paeth(left, up, upper_left):
    estimate = left + up - upper_left
    distances = (abs(estimate - left), abs(estimate - up),
                 abs(estimate - upper_left))
    return (left, up, upper_left)[distances.index(min(distances))]


def read_png(path):
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    position = 8
    width = height = None
    compressed = bytearray()
    while position < len(data):
        length = struct.unpack(">I", data[position:position + 4])[0]
        kind = data[position + 4:position + 8]
        body = data[position + 8:position + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour, compression, filtering, interlace = \
                struct.unpack(">IIBBBBB", body)
            if (depth, colour, compression, filtering, interlace) != \
                    (8, 2, 0, 0, 0):
                raise ValueError("expected non-interlaced 8-bit RGB PNG")
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
        position += length + 12
    if width is None or height is None:
        raise ValueError("PNG has no IHDR")
    raw = zlib.decompress(bytes(compressed))
    stride = width * 3
    if len(raw) != height * (stride + 1):
        raise ValueError("PNG pixel body has the wrong length")
    pixels = bytearray(width * height * 3)
    previous = bytearray(stride)
    source = 0
    for y in range(height):
        method = raw[source]
        source += 1
        row = bytearray(raw[source:source + stride])
        source += stride
        for x in range(stride):
            left = row[x - 3] if x >= 3 else 0
            up = previous[x]
            upper_left = previous[x - 3] if x >= 3 else 0
            if method == 1:
                row[x] = (row[x] + left) & 0xFF
            elif method == 2:
                row[x] = (row[x] + up) & 0xFF
            elif method == 3:
                row[x] = (row[x] + (left + up) // 2) & 0xFF
            elif method == 4:
                row[x] = (row[x] + paeth(left, up, upper_left)) & 0xFF
            elif method != 0:
                raise ValueError("PNG uses an unknown scanline filter")
        pixels[y * stride:(y + 1) * stride] = row
        previous = row
    return width, height, pixels


def compare_pixels(reference, candidate, mode):
    ref_width, ref_height, ref = reference
    got_width, got_height, got = candidate
    if (ref_width, ref_height) != (WIDTH, HEIGHT):
        return False, "reference dimensions are not 1024x768"
    if (got_width, got_height) != (WIDTH, HEIGHT):
        return False, f"candidate dimensions are {got_width}x{got_height}"
    if VARIABLE_REGIONS[mode]:
        return False, "undocumented variable regions are forbidden"
    stride = WIDTH * 3
    for x, y, width, height, label in STABLE_REGIONS[mode]:
        if x + width > WIDTH or y + height > HEIGHT:
            return False, f"stable region {label} exceeds the framebuffer"
        for row in range(y, y + height):
            begin = row * stride + x * 3
            end = begin + width * 3
            if ref[begin:end] != got[begin:end]:
                for offset in range(begin, end):
                    if ref[offset] != got[offset]:
                        pixel = offset // 3
                        return False, (
                            f"stable region {label} changed at "
                            f"{pixel % WIDTH},{pixel // WIDTH}"
                        )
    return True, "all stable Phipia pixels match"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=sorted(STABLE_REGIONS), required=True)
    parser.add_argument("reference")
    parser.add_argument("candidate", nargs="?")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    reference = read_png(args.reference)
    candidate = read_png(args.candidate or args.reference)
    passed, detail = compare_pixels(reference, candidate, args.mode)
    if not passed:
        raise SystemExit(detail)
    if args.self_test:
        damaged = (candidate[0], candidate[1], bytearray(candidate[2]))
        damaged[2][0] ^= 1
        negative_passed, _ = compare_pixels(reference, damaged, args.mode)
        if negative_passed:
            raise SystemExit("single-pixel negative control was accepted")
        print("single stable-pixel mutation refused")
    print(detail)


if __name__ == "__main__":
    main()
