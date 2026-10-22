#!/usr/bin/env python3
"""Prints a coarse character rendering of a screenshot of the example level,
so CI logs show what was drawn: '.' sky, 'g' grass, 'd' dirt, 's' stone,
'#' bright HUD text, 'o' coin/player colours, ' ' dark, '?' other."""
import sys
from PIL import Image

def classify(r, g, b):
    if r < 40 and g < 40 and b < 60:
        return " "
    if abs(r - 0x9b) < 30 and abs(g - 0xd4) < 30 and abs(b - 0xf0) < 30:
        return "."
    if g > 150 and r < 150 and b < 120:
        return "g"
    if r > 110 and 60 < g < 140 and b < 90:
        return "d"
    if abs(r - g) < 20 and abs(g - b) < 25 and 90 < r < 200:
        return "s"
    if r > 220 and g > 200 and b > 120:
        return "#"
    if r > 200 and (g > 150 or g < 140) and b < 120:
        return "o"
    return "?"

img = Image.open(sys.argv[1]).convert("RGB")
cols, rows = 64, 18
img = img.resize((cols, rows), Image.NEAREST)
counts = {}
for y in range(rows):
    line = "".join(classify(*img.getpixel((x, y))) for x in range(cols))
    for ch in line:
        counts[ch] = counts.get(ch, 0) + 1
    print(line)
print("cells: " + ", ".join(f"'{k}'={v}" for k, v in sorted(counts.items())))
