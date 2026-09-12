#!/usr/bin/env python3
"""Build Phipia's bounded SPW3 photographic wallpaper collection.

Each committed PNG is cropped to 4:3, resampled to 1024x768 with bilinear
filtering and stored as deterministic little-endian RGB565. The kernel can
decode any frame into a caller-selected output size without carrying PNG or
DEFLATE code, while retaining 65,536 colours instead of the former 256-colour
dithered palette.
"""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path


MAGIC = b"SPW3"
OUT_WIDTH = 1024
OUT_HEIGHT = 768
MAX_FRAMES = 32
PIXEL_BITS = 16


def unfilter(raw: bytes, width: int, height: int, channels: int) -> bytes:
    stride = width * channels
    output = bytearray()
    previous = bytearray(stride)
    offset = 0
    for _ in range(height):
        if offset + stride + 1 > len(raw):
            raise SystemExit("truncated PNG scanline")
        kind = raw[offset]
        line = bytearray(raw[offset + 1:offset + 1 + stride])
        offset += stride + 1
        for index in range(stride):
            left = line[index - channels] if index >= channels else 0
            up = previous[index]
            upper_left = previous[index - channels] if index >= channels else 0
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
                raise SystemExit("unsupported PNG filter")
        output += line
        previous = line
    return bytes(output)


def read_png(path: Path) -> tuple[int, int, int, bytes]:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    offset = 8
    header = None
    compressed = bytearray()
    while offset + 12 <= len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        end = offset + length + 12
        if end > len(data):
            raise SystemExit(f"{path}: truncated PNG chunk")
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body[:13])
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
        offset = end
    if header is None:
        raise SystemExit(f"{path}: missing IHDR")
    width, height, depth, colour, compression, filtering, interlace = header
    if (depth, compression, filtering, interlace) != (8, 0, 0, 0) or colour not in (2, 6):
        raise SystemExit(f"{path}: expected non-interlaced 8-bit RGB/RGBA")
    channels = 3 if colour == 2 else 4
    return width, height, channels, unfilter(
        zlib.decompress(bytes(compressed)), width, height, channels
    )


def interpolate(first: int, second: int, fraction: int, denominator: int) -> int:
    return (first * (denominator - fraction) + second * fraction +
            denominator // 2) // denominator


def build_pixels(source: bytes, width: int, height: int, channels: int) -> bytes:
    # Fill a 4:3 output while preserving source proportions.
    if width * OUT_HEIGHT > height * OUT_WIDTH:
        crop_height = height
        crop_width = height * OUT_WIDTH // OUT_HEIGHT
        crop_x = (width - crop_width) // 2
        crop_y = 0
    else:
        crop_width = width
        crop_height = width * OUT_HEIGHT // OUT_WIDTH
        crop_x = 0
        crop_y = (height - crop_height) // 2
    result = bytearray(OUT_WIDTH * OUT_HEIGHT * 2)
    for y in range(OUT_HEIGHT):
        scaled_y = y * (crop_height - 1)
        source_y = scaled_y // (OUT_HEIGHT - 1)
        fraction_y = scaled_y % (OUT_HEIGHT - 1)
        next_y = min(source_y + 1, crop_height - 1)
        for x in range(OUT_WIDTH):
            scaled_x = x * (crop_width - 1)
            source_x = scaled_x // (OUT_WIDTH - 1)
            fraction_x = scaled_x % (OUT_WIDTH - 1)
            next_x = min(source_x + 1, crop_width - 1)
            channels_out = []
            for channel in range(3):
                top_left = source[
                    ((crop_y + source_y) * width + crop_x + source_x) *
                    channels + channel]
                top_right = source[
                    ((crop_y + source_y) * width + crop_x + next_x) *
                    channels + channel]
                bottom_left = source[
                    ((crop_y + next_y) * width + crop_x + source_x) *
                    channels + channel]
                bottom_right = source[
                    ((crop_y + next_y) * width + crop_x + next_x) *
                    channels + channel]
                top = interpolate(top_left, top_right, fraction_x,
                                  OUT_WIDTH - 1)
                bottom = interpolate(bottom_left, bottom_right, fraction_x,
                                     OUT_WIDTH - 1)
                channels_out.append(interpolate(
                    top, bottom, fraction_y, OUT_HEIGHT - 1))
            red, green, blue = channels_out
            packed = ((red * 31 + 127) // 255) << 11
            packed |= ((green * 63 + 127) // 255) << 5
            packed |= (blue * 31 + 127) // 255
            destination = (y * OUT_WIDTH + x) * 2
            result[destination] = packed & 0xFF
            result[destination + 1] = packed >> 8
    return bytes(result)


def main() -> None:
    if len(sys.argv) < 3:
        raise SystemExit("usage: make-wallpaper-asset.py INPUT.png [...] OUTPUT.spw")
    sources = [Path(value) for value in sys.argv[1:-1]]
    destination = Path(sys.argv[-1])
    if not 1 <= len(sources) <= MAX_FRAMES:
        raise SystemExit(f"wallpaper frame count must be 1..{MAX_FRAMES}")
    blob = bytearray(MAGIC)
    blob += struct.pack("<HHHH", OUT_WIDTH, OUT_HEIGHT, PIXEL_BITS,
                        len(sources))
    for source in sources:
        width, height, channels, pixels = read_png(source)
        blob += build_pixels(pixels, width, height, channels)
        print(f"{source}: {width}x{height} -> {OUT_WIDTH}x{OUT_HEIGHT}",
              file=sys.stderr)
    destination.write_bytes(blob)
    print(f"SPW3 {len(sources)} frames, {len(blob)} bytes -> {destination}",
          file=sys.stderr)


if __name__ == "__main__":
    main()
