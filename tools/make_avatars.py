#!/usr/bin/env python3
"""Draws the 16 avatar portraits players choose from (docs/social.md) as one
64x64 sheet (4x4 portraits of 16x16): services/platform/web/avatars.png. The
browser page and the player's launcher both show it; avatar N is at column
N % 4, row N // 4. Needs Pillow."""
from pathlib import Path

from PIL import Image

OUT = Path(__file__).resolve().parent.parent / "services" / "platform" / "web" / "avatars.png"
BODIES = [(255, 122, 74), (92, 160, 255), (110, 200, 90), (255, 120, 180), (170, 110, 240), (255, 205, 70),
          (60, 200, 190), (230, 70, 70), (255, 150, 40), (140, 140, 160), (120, 90, 60), (250, 250, 250),
          (80, 110, 200), (200, 230, 90), (240, 160, 200), (100, 60, 140)]
STYLES = ["blob", "ears", "antenna", "hat", "blob", "horns", "ears", "bow",
          "hat", "antenna", "blob", "ears", "horns", "bow", "blob", "hat"]


def portrait(body, style):
    im = Image.new("RGBA", (16, 16), (28, 24, 44, 255))
    px = im.load()
    dark = tuple(max(0, c - 70) for c in body)
    for y in range(4, 15):
        for x in range(2, 14):
            if y in (4, 14) and x in (2, 13):
                continue
            px[x, y] = body + (255,)
    for y in range(5, 14):
        px[2, y] = dark + (255,)
    for ex in (5, 10):
        px[ex, 8] = px[ex + 1, 8] = px[ex, 9] = (255, 255, 255, 255)
        px[ex + 1, 9] = (30, 20, 40, 255)
    px[7, 12] = px[8, 12] = dark + (255,)
    if style == "ears":
        for x, y in [(3, 3), (4, 3), (3, 2), (11, 3), (12, 3), (12, 2)]:
            px[x, y] = body + (255,)
    elif style == "antenna":
        for y in range(1, 4):
            px[8, y] = dark + (255,)
        px[8, 0] = px[7, 0] = (255, 230, 90, 255)
    elif style == "hat":
        for x in range(3, 13):
            px[x, 3] = (40, 40, 60, 255)
        for x in range(5, 11):
            for y in range(0, 3):
                px[x, y] = (40, 40, 60, 255)
            px[x, 2] = (200, 60, 60, 255)
    elif style == "horns":
        for x, y in [(3, 3), (3, 2), (4, 1), (12, 3), (12, 2), (11, 1)]:
            px[x, y] = (245, 240, 220, 255)
    elif style == "bow":
        for x, y in [(9, 2), (10, 1), (10, 3), (11, 2), (12, 1), (12, 3), (13, 2), (11, 3), (11, 1)]:
            px[x, y] = (255, 90, 150, 255)
    return im


def main():
    sheet = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    for i, (body, style) in enumerate(zip(BODIES, STYLES)):
        sheet.paste(portrait(body, style), ((i % 4) * 16, (i // 4) * 16))
    sheet.save(OUT, optimize=True)
    print(f"Wrote {OUT}")


if __name__ == "__main__":
    main()
