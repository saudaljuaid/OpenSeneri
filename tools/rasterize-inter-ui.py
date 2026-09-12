#!/usr/bin/env python3
"""Developer tool: rasterize pinned Inter into Phipia's committed UI atlas.

This regeneration step requires Pillow. The regular kernel build consumes the
resulting grayscale PNG and metrics text using only the Python standard
library, so CI does not depend on Pillow or a runtime TrueType implementation.
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


WIDTH = 16
HEIGHT = 19
ASCENT = 15
FIRST = 0x20
COUNT = 0x7F - FIRST
FONT_SIZE = 15


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: rasterize-inter-ui.py InterVariable.ttf ATLAS.png METRICS.txt"
        )
    source, atlas_path, metrics_path = map(Path, sys.argv[1:])
    font = ImageFont.truetype(str(source), FONT_SIZE)
    if font.getmetrics() != (ASCENT, HEIGHT - ASCENT):
        raise SystemExit(f"unexpected Inter metrics: {font.getmetrics()}")
    atlas = Image.new("L", (WIDTH * COUNT, HEIGHT), 0)
    draw = ImageDraw.Draw(atlas)
    metrics = [
        "# Inter UI advances; source pinned in docs/THIRD_PARTY_ASSETS.md",
        f"# cell={WIDTH}x{HEIGHT} baseline={ASCENT} font_size={FONT_SIZE}",
    ]
    for code in range(FIRST, FIRST + COUNT):
        character = chr(code)
        advance = max(1, int(round(font.getlength(character))))
        if advance > 15:
            raise SystemExit(f"advance too large for U+{code:04X}: {advance}")
        # One-pixel inset retains Inter's occasional -1 left bearing.
        draw.text(((code - FIRST) * WIDTH + 1, ASCENT), character,
                  fill=255, font=font, anchor="ls")
        metrics.append(f"{code:02X} {advance}")
    atlas.save(atlas_path, format="PNG", optimize=True)
    metrics_path.write_text("\n".join(metrics) + "\n", encoding="ascii")
    print(f"{source} -> {atlas_path} and {metrics_path}")


if __name__ == "__main__":
    main()
