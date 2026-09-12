#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Turn assets/phipia-logo.png into the run-length asset the kernel draws.

The kernel deliberately does not carry a PNG or DEFLATE parser.  The expensive
and general-purpose half happens here, at development time, and the kernel is
left with a small format it can validate in a single bounded pass.

Run it only when the logo itself changes:

    python3 tools/make-logo-asset.py assets/phipia-logo.png 280 build/logo.srl

The result is a build artifact and is not committed; the kernel includes it at
compile time.  The wire format is four magic bytes, a little-endian width and
height, then runs of one length byte and four RGBA bytes.

Requires only the Python standard library.
"""
import sys
import zlib
import struct


def read_png(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise SystemExit('not a PNG')
    pos, idat, header = 8, bytearray(), None
    while pos < len(data):
        length = struct.unpack('>I', data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', body[:13])
        elif kind == b'IDAT':
            idat += body
        elif kind == b'IEND':
            break
        pos += 12 + length
    width, height, depth, colour, _, _, interlace = header
    if depth != 8 or colour != 6 or interlace != 0:
        raise SystemExit('expected a non-interlaced 8-bit RGBA PNG')
    return width, height, unfilter(zlib.decompress(bytes(idat)), width, height)


def unfilter(raw, width, height):
    """Undo the per-scanline filters PNG applies (RFC 2083 section 6)."""
    stride, out, previous = width * 4, bytearray(), bytearray(width * 4)
    pos = 0
    for _ in range(height):
        kind = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for index in range(stride):
            left = line[index - 4] if index >= 4 else 0
            up = previous[index]
            upleft = previous[index - 4] if index >= 4 else 0
            if kind == 1:
                line[index] = (line[index] + left) & 0xFF
            elif kind == 2:
                line[index] = (line[index] + up) & 0xFF
            elif kind == 3:
                line[index] = (line[index] + (left + up) // 2) & 0xFF
            elif kind == 4:
                p = left + up - upleft
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upleft)
                nearest = left if (pa <= pb and pa <= pc) else (
                    up if pb <= pc else upleft)
                line[index] = (line[index] + nearest) & 0xFF
        out += line
        previous = line
    return out


def fit_within(width, height, maximum):
    """Keep the source aspect ratio and never enlarge the supplied artwork."""
    if maximum <= 0:
        raise SystemExit('maximum dimension must be positive')
    if width <= maximum and height <= maximum:
        return width, height
    if width >= height:
        return maximum, max(1, (height * maximum + width // 2) // width)
    return max(1, (width * maximum + height // 2) // height), maximum


def crop_to_primary_mark(pixels, width, height, threshold=8, padding=8):
    """Crop to the largest connected non-transparent component.

    Background removal can leave isolated opaque crumbs around an otherwise
    transparent canvas. Selecting the primary component keeps the supplied
    mark exact while preventing those crumbs from expanding its runtime box.
    """
    occupied = bytearray(
        pixels[index * 4 + 3] >= threshold for index in range(width * height)
    )
    largest = None
    for seed in range(width * height):
        if not occupied[seed]:
            continue
        occupied[seed] = 0
        stack = [seed]
        count = 0
        minimum_x = maximum_x = seed % width
        minimum_y = maximum_y = seed // width
        while stack:
            index = stack.pop()
            x, y = index % width, index // width
            count += 1
            minimum_x, maximum_x = min(minimum_x, x), max(maximum_x, x)
            minimum_y, maximum_y = min(minimum_y, y), max(maximum_y, y)
            neighbours = []
            if x:
                neighbours.append(index - 1)
            if x + 1 < width:
                neighbours.append(index + 1)
            if y:
                neighbours.append(index - width)
            if y + 1 < height:
                neighbours.append(index + width)
            for neighbour in neighbours:
                if occupied[neighbour]:
                    occupied[neighbour] = 0
                    stack.append(neighbour)
        candidate = (count, minimum_x, minimum_y, maximum_x, maximum_y)
        if largest is None or candidate[0] > largest[0]:
            largest = candidate

    if largest is None:
        raise SystemExit('logo has no visible primary mark')
    _, minimum_x, minimum_y, maximum_x, maximum_y = largest
    minimum_x = max(0, minimum_x - padding)
    minimum_y = max(0, minimum_y - padding)
    maximum_x = min(width - 1, maximum_x + padding)
    maximum_y = min(height - 1, maximum_y + padding)
    cropped_width = maximum_x - minimum_x + 1
    cropped_height = maximum_y - minimum_y + 1
    cropped = bytearray(cropped_width * cropped_height * 4)
    for y in range(cropped_height):
        source = ((minimum_y + y) * width + minimum_x) * 4
        destination = y * cropped_width * 4
        cropped[destination:destination + cropped_width * 4] = \
            pixels[source:source + cropped_width * 4]
    return cropped_width, cropped_height, cropped


def downscale(pixels, width, height, out_width, out_height):
    """Box filter, averaging in premultiplied space so edges do not halo."""
    if out_width == width and out_height == height:
        return pixels

    out = bytearray(out_width * out_height * 4)
    for y in range(out_height):
        y0 = y * height // out_height
        y1 = max((y + 1) * height // out_height, y0 + 1)
        for x in range(out_width):
            x0 = x * width // out_width
            x1 = max((x + 1) * width // out_width, x0 + 1)
            r = g = b = a = n = 0
            for sy in range(y0, y1):
                row = sy * width * 4
                for sx in range(x0, x1):
                    o = row + sx * 4
                    alpha = pixels[o + 3]
                    r += pixels[o] * alpha
                    g += pixels[o + 1] * alpha
                    b += pixels[o + 2] * alpha
                    a += alpha
                    n += 1
            o = (y * out_width + x) * 4
            if a:
                out[o], out[o + 1], out[o + 2] = r // a, g // a, b // a
            out[o + 3] = a // n
    return out


def encode(pixels, width, height):
    """Runs of identical RGBA, at most 255 long, five bytes each."""
    body, run, index = bytearray(), None, 0
    total = width * height
    while index < total:
        o = index * 4
        pixel = bytes(pixels[o:o + 4])
        if run and run[0] == pixel and run[1] < 255:
            run = (pixel, run[1] + 1)
        else:
            if run:
                body.append(run[1])
                body += run[0]
            run = (pixel, 1)
        index += 1
    if run:
        body.append(run[1])
        body += run[0]
    return body


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else 'assets/phipia-logo.png'
    maximum = int(sys.argv[2]) if len(sys.argv) > 2 else 280
    keep_canvas = len(sys.argv) > 4 and sys.argv[4] == '--keep-canvas'
    if len(sys.argv) > 4 and not keep_canvas:
        raise SystemExit('fourth argument must be --keep-canvas')
    source_width, source_height, pixels = read_png(source)
    if keep_canvas:
        width, height = source_width, source_height
    else:
        width, height, pixels = crop_to_primary_mark(
            pixels, source_width, source_height
        )
    out_width, out_height = fit_within(width, height, maximum)
    scaled = downscale(pixels, width, height, out_width, out_height)
    body = encode(scaled, out_width, out_height)
    blob = bytearray(b'SRL1')
    blob += struct.pack('<HH', out_width, out_height)
    blob += body

    destination = sys.argv[3] if len(sys.argv) > 3 else 'build/logo.srl'
    with open(destination, 'wb') as handle:
        handle.write(blob)

    print(f'{source}: {source_width}x{source_height} -> crop {width}x{height} '
          f'-> {out_width}x{out_height}, '
          f'{len(body) // 5} runs, {len(blob)} bytes -> {destination}',
          file=sys.stderr)


if __name__ == '__main__':
    main()
