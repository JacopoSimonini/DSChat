#!/usr/bin/env python3
"""Draw the DSChat ROM icon (32x32, 16 colours, colour 0 = transparent).

An original, simple design: a rounded speech bubble with three "typing"
dots and a tail at the bottom left, plus a small second bubble behind it
(two sides of a chat). Written as an indexed PNG that ndstool puts in the
ROM banner (ds/dschat/Makefile GAME_ICON). Run: python3 tools/make_icon.py
"""
import sys
from pathlib import Path

from PIL import Image

OUT = Path(__file__).resolve().parent.parent / "ds" / "dschat" / "icon.png"

PAL = [
    (255, 0, 255),    # 0 transparent
    (16, 24, 48),     # 1 outline (dark navy)
    (248, 248, 240),  # 2 bubble fill
    (200, 208, 216),  # 3 bubble shade
    (40, 120, 220),   # 4 dots (blue)
    (60, 170, 90),    # 5 back bubble (green)
    (30, 100, 55),    # 6 back bubble outline
]


def rounded_rect(px, x0, y0, x1, y1, r, fill, edge):
    """Filled rectangle with corners cut to radius r, 1 px outline."""
    def inside(x, y):
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r + r * 0.8
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if inside(x, y):
                border = not all(inside(x + dx, y + dy) for dx, dy in
                                 ((1, 0), (-1, 0), (0, 1), (0, -1)))
                px[x, y] = edge if border else fill


def main():
    img = Image.new("P", (32, 32), 0)
    img.putpalette([c for rgb in PAL for c in rgb] + [0] * (3 * (256 - len(PAL))))
    px = img.load()

    # back bubble (the other speaker), top right
    rounded_rect(px, 14, 1, 30, 13, 4, 5, 6)

    # front bubble
    rounded_rect(px, 1, 6, 26, 25, 6, 2, 1)
    for y in range(20, 25):                                           # bottom shade
        for x in range(2, 26):
            if px[x, y] == 2 and px[x, y + 1] in (1, 3):
                px[x, y] = 3
    # tail, bottom left: a filled triangle with an outline, joined to the bubble
    def in_tail(x, y):
        # triangle (6,24) (12,24) (3,30)
        def side(ax, ay, bx, by):
            return (bx - ax) * (y - ay) - (by - ay) * (x - ax)
        a, b, c = side(6, 24, 12, 24), side(12, 24, 3, 30), side(3, 30, 6, 24)
        return (a >= 0 and b >= 0 and c >= 0) or (a <= 0 and b <= 0 and c <= 0)
    for y in range(24, 31):
        for x in range(1, 14):
            if in_tail(x, y) and px[x, y] in (0, 1):
                edge = not all(in_tail(x + dx, y + dy) or px[x + dx, y + dy] in (2, 3)
                               for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
                px[x, y] = 1 if edge else 3
    # three dots (3x3, slightly rounded)
    for cx in (8, 14, 20):
        for dy in range(3):
            for dx in range(3):
                px[cx - 1 + dx, 14 + dy] = 4
    img.info["transparency"] = 0
    img.save(OUT, transparency=0)
    print(f"wrote {OUT}")
    if "--show" in sys.argv:
        big = img.convert("RGB").resize((256, 256), Image.NEAREST)
        big.save(OUT.with_name("icon_preview.png"))


if __name__ == "__main__":
    main()
