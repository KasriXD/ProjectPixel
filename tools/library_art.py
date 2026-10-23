"""Pixel art for the starter library (library/art), drawn procedurally.

All art is original. Each function returns a Pillow image; tools/generate_library.py
saves them. Sizes are multiples of 16 so they line up with 16-pixel tiles.
"""

import random

from PIL import Image


def hexrgb(h, alpha=255):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)) + (alpha,)


def draw_grid(img, ox, oy, rows, palette):
    width = len(rows[0])
    for y, row in enumerate(rows):
        assert len(row) == width, f"row {y} is {len(row)} wide, expected {width}: {row!r}"
        for x, ch in enumerate(row):
            if ch in palette:
                img.putpixel((ox + x, oy + y), palette[ch])


def fill(img, ox, oy, w, h, color):
    for y in range(h):
        for x in range(w):
            img.putpixel((ox + x, oy + y), color)


def speckle(img, ox, oy, colors, count, seed, w=16, h=16):
    rng = random.Random(seed)
    for _ in range(count):
        img.putpixel((ox + rng.randrange(w), oy + rng.randrange(h)), rng.choice(colors))


def outline(img, color, ox=0, oy=0, w=None, h=None):
    """Draws `color` on opaque pixels that touch a transparent one (inside the box)."""
    w = w or img.width
    h = h or img.height
    src = img.copy()
    for y in range(oy, oy + h):
        for x in range(ox, ox + w):
            if src.getpixel((x, y))[3] == 0:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if not (ox <= nx < ox + w and oy <= ny < oy + h) or src.getpixel((nx, ny))[3] == 0:
                    img.putpixel((x, y), color)
                    break


def blob(img, ox, oy, w, h, circles, color):
    for y in range(h):
        for x in range(w):
            if any((x - cx) ** 2 + (y - cy) ** 2 <= r * r for cx, cy, r in circles):
                img.putpixel((ox + x, oy + y), color)


# --------------------------------------------------------------------------- top-down tiles

GRASS, GRASS_DK, GRASS_LT = hexrgb("#5bb450"), hexrgb("#47993f"), hexrgb("#7cc96a")
DIRT, DIRT_DK, DIRT_LT = hexrgb("#c9a26b"), hexrgb("#aa8452"), hexrgb("#e0c08c")
SAND, SAND_DK, SAND_LT = hexrgb("#ecd59a"), hexrgb("#d6bb78"), hexrgb("#f6e6b8")
WATER, WATER_DK, WATER_LT = hexrgb("#3d8fd6"), hexrgb("#2f74b5"), hexrgb("#7fbdf0")
STONE, STONE_DK, STONE_LT = hexrgb("#a3a8b0"), hexrgb("#7d828b"), hexrgb("#c4c8ce")
WALL_TOP, WALL_FACE, WALL_DK, WALL_LT = hexrgb("#9aa0aa"), hexrgb("#6e737c"), hexrgb("#4e525a"), hexrgb("#b9bec6")
WOOD, WOOD_DK, WOOD_LT = hexrgb("#b07a3c"), hexrgb("#7a5226"), hexrgb("#d39a55")
HEDGE, HEDGE_DK, HEDGE_LT = hexrgb("#2f7d33"), hexrgb("#1d5a22"), hexrgb("#4fa752")
BRICK, BRICK_DK, MORTAR = hexrgb("#b5523b"), hexrgb("#8c3b28"), hexrgb("#e0c9a6")


def tile_grass(img, ox, oy, seed):
    fill(img, ox, oy, 16, 16, GRASS)
    speckle(img, ox, oy, [GRASS_DK, GRASS_DK, GRASS_LT], 26, seed)
    rng = random.Random(seed + 1)
    for _ in range(3):  # small blades
        x, y = rng.randrange(1, 15), rng.randrange(2, 15)
        img.putpixel((ox + x, oy + y), GRASS_DK)
        img.putpixel((ox + x, oy + y - 1), GRASS_LT)


def tile_flowers(img, ox, oy, seed):
    tile_grass(img, ox, oy, seed)
    for (x, y, petal) in ((3, 4, "#ff6f91"), (11, 3, "#fff176"), (7, 10, "#ffffff"), (12, 12, "#ff6f91")):
        c = hexrgb(petal)
        for dx, dy in ((0, -1), (-1, 0), (1, 0), (0, 1)):
            img.putpixel((ox + x + dx, oy + y + dy), c)
        img.putpixel((ox + x, oy + y), hexrgb("#ffb300"))


def tile_textured(img, ox, oy, base, dark, light, seed, count=30):
    fill(img, ox, oy, 16, 16, base)
    speckle(img, ox, oy, [dark, dark, light], count, seed)


def tile_water(img, ox, oy, seed, edge=False):
    fill(img, ox, oy, 16, 16, WATER)
    rng = random.Random(seed)
    for _ in range(5):
        x, y = rng.randrange(0, 12), rng.randrange(0, 16)
        for i in range(4):
            img.putpixel((ox + x + i, oy + y), WATER_LT if i in (1, 2) else WATER_DK)
    if edge:  # grass bank along the top
        for x in range(16):
            bank = 5 + (1 if x % 6 == 2 else 0)
            for y in range(bank):
                img.putpixel((ox + x, oy + y), GRASS if y < bank - 1 else GRASS_DK)
            img.putpixel((ox + x, oy + bank), WATER_LT)


def tile_stone_floor(img, ox, oy, dark=False):
    base, line, hi = (STONE, STONE_DK, STONE_LT) if not dark else (STONE_DK, WALL_DK, STONE)
    fill(img, ox, oy, 16, 16, base)
    for i in range(16):
        img.putpixel((ox + i, oy + 7), line)
        img.putpixel((ox + i, oy + 15), line)
        img.putpixel((ox + 7, oy + i), line)
        img.putpixel((ox + 15, oy + i), line)
    for (x, y) in ((0, 0), (8, 0), (0, 8), (8, 8)):
        for i in range(6):
            img.putpixel((ox + x + i, oy + y), hi)
            img.putpixel((ox + x, oy + y + i), hi)


def tile_wood(img, ox, oy):
    fill(img, ox, oy, 16, 16, WOOD)
    for y in (3, 7, 11, 15):
        for x in range(16):
            img.putpixel((ox + x, oy + y), WOOD_DK)
    for y, x in ((1, 5), (5, 12), (9, 3), (13, 9)):
        img.putpixel((ox + x, oy + y), WOOD_DK)  # plank ends
        img.putpixel((ox + x + 1, oy + y), WOOD_LT)
    speckle(img, ox, oy, [WOOD_LT], 8, 77)


def tile_wall(img, ox, oy):
    # Seen from above at an angle: a light top, then the brick face.
    fill(img, ox, oy, 16, 6, WALL_TOP)
    for x in range(16):
        img.putpixel((ox + x, oy), WALL_LT)
        img.putpixel((ox + x, oy + 5), WALL_DK)
    fill(img, ox, oy + 6, 16, 10, WALL_FACE)
    for y in (10, 15):
        for x in range(16):
            img.putpixel((ox + x, oy + y), WALL_DK)
    for x in (4, 12):
        for y in range(6, 10):
            img.putpixel((ox + x, oy + y), WALL_DK)
    for x in (0, 8):
        for y in range(11, 15):
            img.putpixel((ox + x, oy + y), WALL_DK)
    speckle(img, ox, oy + 6, [WALL_LT], 5, 31, h=10)


def tile_bricks(img, ox, oy):
    fill(img, ox, oy, 16, 16, BRICK)
    for y in (3, 7, 11, 15):
        for x in range(16):
            img.putpixel((ox + x, oy + y), MORTAR)
    for row, xs in enumerate(((5, 13), (1, 9), (5, 13), (1, 9))):
        for x in xs:
            for y in range(row * 4, row * 4 + 3):
                img.putpixel((ox + x, oy + y), MORTAR)
    speckle(img, ox, oy, [BRICK_DK], 12, 41)


def tile_hedge(img, ox, oy):
    fill(img, ox, oy, 16, 16, HEDGE)
    rng = random.Random(55)
    for _ in range(14):
        x, y = rng.randrange(0, 15), rng.randrange(0, 13)
        img.putpixel((ox + x, oy + y), HEDGE_LT)
        img.putpixel((ox + x + 1, oy + y), HEDGE_LT)
    for _ in range(10):
        img.putpixel((ox + rng.randrange(16), oy + rng.randrange(16)), HEDGE_DK)
    for x in range(16):
        img.putpixel((ox + x, oy + 15), HEDGE_DK)
        img.putpixel((ox + x, oy + 14), HEDGE_DK if x % 3 else HEDGE)


def tile_bridge(img, ox, oy):
    tile_water(img, ox, oy, 91)
    for x in range(16):
        for y in range(2, 14):
            img.putpixel((ox + x, oy + y), WOOD if x % 4 else WOOD_DK)
        img.putpixel((ox + x, oy + 2), WOOD_DK)
        img.putpixel((ox + x, oy + 13), WOOD_DK)
        img.putpixel((ox + x, oy + 1), WOOD_LT if x % 4 else WOOD_DK)


def tile_cliff(img, ox, oy):
    rock, rock_dk, rock_lt = hexrgb("#8d7b6a"), hexrgb("#5f5145"), hexrgb("#b39e88")
    fill(img, ox, oy, 16, 16, rock)
    speckle(img, ox, oy, [rock_dk, rock_lt], 30, 63)
    for (x0, y0, x1, y1) in ((2, 3, 6, 8), (9, 1, 12, 6), (11, 9, 14, 14), (4, 11, 7, 15)):
        steps = max(abs(x1 - x0), abs(y1 - y0))
        for i in range(steps + 1):
            img.putpixel((ox + x0 + (x1 - x0) * i // steps, oy + y0 + (y1 - y0) * i // steps), rock_dk)


def tile_carpet(img, ox, oy):
    red, red_dk, gold = hexrgb("#a83246"), hexrgb("#822236"), hexrgb("#e8b54a")
    fill(img, ox, oy, 16, 16, red)
    for y in range(0, 16, 4):
        for x in range(0, 16, 4):
            img.putpixel((ox + x + 1, oy + y + 1), red_dk)
            img.putpixel((ox + x + 2, oy + y + 2), gold)


def tile_checker(img, ox, oy):
    a, b = hexrgb("#e8e2d0"), hexrgb("#c9c0a8")
    for y in range(16):
        for x in range(16):
            img.putpixel((ox + x, oy + y), a if (x // 8 + y // 8) % 2 == 0 else b)


TOPDOWN_TILE_NAMES = [
    "grass", "flowers", "path", "sand", "water", "stone floor", "wood floor", "dark grass",
    "stone wall", "bricks", "hedge", "water bank", "bridge", "cliff", "carpet", "checkered floor",
]


def make_topdown_tiles():
    """8x2 tiles of 16x16 (see TOPDOWN_TILE_NAMES)."""
    img = Image.new("RGBA", (128, 32), (0, 0, 0, 0))
    tile_grass(img, 0, 0, 1)
    tile_flowers(img, 16, 0, 2)
    tile_textured(img, 32, 0, DIRT, DIRT_DK, DIRT_LT, 3)
    tile_textured(img, 48, 0, SAND, SAND_DK, SAND_LT, 4, 22)
    tile_water(img, 64, 0, 5)
    tile_stone_floor(img, 80, 0)
    tile_wood(img, 96, 0)
    tile_textured(img, 112, 0, GRASS_DK, hexrgb("#357a30"), GRASS, 8, 40)
    tile_wall(img, 0, 16)
    tile_bricks(img, 16, 16)
    tile_hedge(img, 32, 16)
    tile_water(img, 48, 16, 12, edge=True)
    tile_bridge(img, 64, 16)
    tile_cliff(img, 80, 16)
    tile_carpet(img, 96, 16)
    tile_checker(img, 112, 16)
    return img


# --------------------------------------------------------------------------- characters

HERO = [
    "................",
    ".....oooooo.....",
    "....oGGGGGGo....",
    "...oGgGGGGGGo...",
    "...oGGGGGGGGo...",
    "...ooooooooooo..",
    "...oSSSSSSSSo...",
    "...oSKSSSSKSo...",
    "...oSSSSSSSSo...",
    "....oSSmmSSo....",
    "...ooBBBBBBoo...",
    "..oSoBBbBBBoSo..",
    "..ooBBBBBBBBoo..",
    "....oNNooNNo....",
    "....oFFo.oFFo...",
    "....ooo..ooo....",
]
HERO_PALETTE = {
    "o": hexrgb("#24192b"), "G": hexrgb("#3fa34d"), "g": hexrgb("#7fd88a"), "S": hexrgb("#ffcfa8"),
    "K": hexrgb("#1a1a2e"), "m": hexrgb("#c0505a"), "B": hexrgb("#3f6fd8"), "b": hexrgb("#7ea2f0"),
    "N": hexrgb("#4a3a6a"), "F": hexrgb("#6a4a2a"),
}


def make_hero():
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    draw_grid(img, 0, 0, HERO, HERO_PALETTE)
    return img


SLIME = [
    [
        "................",
        "................",
        "................",
        "................",
        "................",
        "......oooo......",
        "....ooGGGGoo....",
        "...oGgGGGGGGo...",
        "..oGgGGGGGGGGo..",
        "..oGGWKGGWKGGo..",
        ".oGGGWKGGWKGGGo.",
        ".oGGGGGGGGGGGGo.",
        ".oGGGGGmmGGGGGo.",
        ".oDGGGGGGGGGGDo.",
        "..oDDDDDDDDDDo..",
        "...oooooooooo...",
    ],
    [
        "................",
        "................",
        "................",
        "................",
        "................",
        "................",
        "................",
        ".....oooooo.....",
        "...ooGgGGGGoo...",
        "..oGgGGGGGGGGo..",
        ".oGGGWKGGWKGGGo.",
        "oGGGGWKGGWKGGGGo",
        "oGGGGGGmmGGGGGGo",
        "oDGGGGGGGGGGGGDo",
        ".oDDDDDDDDDDDDo.",
        "..oooooooooooo..",
    ],
]
SLIME_PALETTE = {
    "o": hexrgb("#1f3d24"), "G": hexrgb("#6cc24a"), "g": hexrgb("#b6f08a"), "D": hexrgb("#3f8f35"),
    "W": hexrgb("#ffffff"), "K": hexrgb("#1a1a2e"), "m": hexrgb("#2b5a2a"),
}

BEETLE = [
    [
        "................",
        "......o..o......",
        ".......oo.......",
        "......oKKo......",
        ".o...oKKKKo...o.",
        "..o.oRRRRRRo.o..",
        "...oRrRRKRRRo...",
        "..oRRRRKRRRRRo..",
        "o.oRKRRKRRKRRo.o",
        ".ooRRRRKRRRRRoo.",
        "..oRRKRKRRRKRo..",
        ".o.oRRRKRRRRo.o.",
        "o...oRRKRRRo...o",
        ".....oooooo.....",
        "................",
        "................",
    ],
    [
        "................",
        "......o..o......",
        ".......oo.......",
        "......oKKo......",
        "...o.oKKKKo.o...",
        ".o..oRRRRRRo..o.",
        "..ooRrRRKRRRoo..",
        "...oRRRRKRRRRo..",
        "..oRKRRKRRKRRo..",
        "o.oRRRRKRRRRRo.o",
        ".ooRRKRKRRRKRoo.",
        "...oRRRKRRRRo...",
        "..o.oRRKRRRo.o..",
        ".o...oooooo...o.",
        "................",
        "................",
    ],
]
BEETLE_PALETTE = {
    "o": hexrgb("#1c1416"), "K": hexrgb("#2c2226"), "R": hexrgb("#d33a3a"), "r": hexrgb("#ff8a80"),
}


def make_frames(frames, palette, width=16, height=16):
    img = Image.new("RGBA", (width * len(frames), height), (0, 0, 0, 0))
    for i, frame in enumerate(frames):
        draw_grid(img, i * width, 0, frame, palette)
    return img


# --------------------------------------------------------------------------- items

GEM = [
    "......oo......",
    ".....oCCo.....",
    "....oCHCCo....",
    "...oCHCCCDo...",
    "..oCHCCCCCDo..",
    ".oCCCCCCCCCDo.",
    ".oDCCCCCCCDDo.",
    "..oDCCCCCDDo..",
    "...oDCCCDDo...",
    "....oDCDDo....",
    ".....oDDo.....",
    "......oo......",
]
GEM_PALETTE = {"o": hexrgb("#0b4f6c"), "C": hexrgb("#4dd0e1"), "H": hexrgb("#e0fbff"), "D": hexrgb("#0097a7")}


def make_gem():
    """4 frames of 16x16: the gem with a sparkle moving across it."""
    img = Image.new("RGBA", (64, 16), (0, 0, 0, 0))
    sparkles = [None, (5, 4), (8, 6), (10, 8)]
    for i, spark in enumerate(sparkles):
        draw_grid(img, i * 16 + 1, 2, GEM, GEM_PALETTE)
        if spark:
            x, y = i * 16 + 1 + spark[0], 2 + spark[1]
            for dx, dy in ((0, 0), (1, 0), (-1, 0), (0, 1), (0, -1)):
                img.putpixel((x + dx, y + dy), hexrgb("#ffffff"))
    return img


KEY = [
    "................",
    "................",
    "................",
    "..oooo..........",
    ".oYYYYo.........",
    "oYHooYYo........",
    "oYo..oYoooooooo.",
    "oYo..oYYYYYYYYYo",
    "oYYooYYoooYoYoYo",
    ".oYYYYo...oYoYo.",
    "..oooo.....o.o..",
    "................",
    "................",
    "................",
    "................",
    "................",
]
KEY_PALETTE = {"o": hexrgb("#7a4f00"), "Y": hexrgb("#ffc93c"), "H": hexrgb("#fff3b0")}

LOCK_BLOCK = [
    "oooooooooooooooo",
    "oMBBBBBBBBBBBBMo",
    "oBbbbbbbbbbbbbBo",
    "oBbbbbooooobbbBo",
    "oBbbbobbbbbobbBo",
    "oBbbbobbbbbobbBo",
    "oBbbooooooooobBo",
    "oBbboYYYYYYYobBo",
    "oBbboYYYooYYobBo",
    "oBbboYYYooYYobBo",
    "oBbboYYYYoYYobBo",
    "oBbboYYYYYYYobBo",
    "oBbbbooooooobbBo",
    "oBbbbbbbbbbbbbBo",
    "oMBBBBBBBBBBBBMo",
    "oooooooooooooooo",
]
LOCK_BLOCK_PALETTE = {
    "o": hexrgb("#2b1d1a"), "B": hexrgb("#6d4c41"), "b": hexrgb("#8d6e63"), "M": hexrgb("#b0bec5"),
    "Y": hexrgb("#ffc93c"),
}

def make_spikes():
    """16x16: three metal spikes on a base plate (side view)."""
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    light, shade, dark = hexrgb("#eef1f5"), hexrgb("#9aa1ab"), hexrgb("#3a3e45")
    for k in range(3):
        centre = 3 + k * 5  # 3, 8, 13
        for row in range(9):  # y = 4 .. 12, tip at the top
            half = row // 2
            for x in range(centre - half, centre + half + 1):
                if 0 <= x < 16:
                    colour = light if x < centre else shade
                    if x == centre + half and row > 1:
                        colour = dark
                    img.putpixel((x, 4 + row), colour)
    base, base_dk = hexrgb("#5d636d"), hexrgb("#2a2d33")
    for x in range(16):
        img.putpixel((x, 13), base_dk)
        img.putpixel((x, 14), base)
        img.putpixel((x, 15), base_dk)
    return img


SPIKE_TRAP = [
    "oooooooooooooooo",
    "oPPPPPPPPPPPPPPo",
    "oPdddPPPPPPdddPo",
    "oPdHdPPPPPPdHdPo",
    "oPdddPPPPPPdddPo",
    "oPPPPPPPPPPPPPPo",
    "oPPPPPdddPPPPPPo",
    "oPPPPPdHdPPPPPPo",
    "oPPPPPdddPPPPPPo",
    "oPPPPPPPPPPPPPPo",
    "oPdddPPPPPPdddPo",
    "oPdHdPPPPPPdHdPo",
    "oPdddPPPPPPdddPo",
    "oPPPPPPPPPPPPPPo",
    "oLLLLLLLLLLLLLLo",
    "oooooooooooooooo",
]
SPIKE_TRAP_PALETTE = {"o": hexrgb("#2a2d33"), "P": hexrgb("#7d8590"), "d": hexrgb("#3c4048"),
                      "H": hexrgb("#f0f3f7"), "L": hexrgb("#5d636d")}

SPRING = [
    "................",
    "................",
    "................",
    "..oooooooooooo..",
    "..oRRRRRRRRRRo..",
    "..oDDDDDDDDDDo..",
    "..oooooooooooo..",
    "......oMo.......",
    ".......oMo......",
    "......oMo.......",
    ".......oMo......",
    "......oMo.......",
    ".......oMo......",
    ".oooooooooooooo.",
    ".oGGGGGGGGGGGGo.",
    ".oooooooooooooo.",
]
SPRING_PALETTE = {"o": hexrgb("#2a2d33"), "R": hexrgb("#ef5350"), "D": hexrgb("#b71c1c"),
                  "M": hexrgb("#cfd8dc"), "G": hexrgb("#78909c")}

CHECKPOINT = [
    [
        "................",
        "...oooooo.......",
        "...oPGGGGoo.....",
        "...oPGGgGGGo....",
        "...oPGGGGGGGo...",
        "...oPGGGGGGo....",
        "...oPGGGGoo.....",
        "...oPoooo.......",
        "...oPo..........",
        "...oPo..........",
        "...oPo..........",
        "...oPo..........",
        "...oPo..........",
        "..ooooo.........",
        ".oSSSSSo........",
        ".ooooooo........",
    ],
    [
        "................",
        "...ooooooo......",
        "...oPGGGGGoo....",
        "...oPGGgGGGGo...",
        "...oPGGGGGGo....",
        "...oPGGGGGGGo...",
        "...oPGGGGGoo....",
        "...oPooooo......",
        "...oPo..........",
        "...oPo..........",
        "...oPo..........",
        "...oPo..........",
        "...oPo..........",
        "..ooooo.........",
        ".oSSSSSo........",
        ".ooooooo........",
    ],
]
CHECKPOINT_PALETTE = {"o": hexrgb("#23272e"), "P": hexrgb("#cfd8dc"), "G": hexrgb("#43a047"),
                      "g": hexrgb("#a5d6a7"), "S": hexrgb("#8d9399")}


def make_goal_flag():
    """16x32: a tall pole with a chequered flag."""
    img = Image.new("RGBA", (16, 32), (0, 0, 0, 0))
    pole, pole_dk, gold, gold_dk = hexrgb("#e0e3e7"), hexrgb("#6e747c"), hexrgb("#ffd23f"), hexrgb("#b8860b")
    for y in range(4, 29):
        img.putpixel((3, y), pole_dk)
        img.putpixel((4, y), pole)
        img.putpixel((5, y), pole_dk)
    blob(img, 0, 0, 16, 32, [(4, 2, 2)], gold)
    img.putpixel((3, 1), hexrgb("#fff6b8"))
    img.putpixel((4, 4), gold_dk)
    black, white = hexrgb("#1f2329"), hexrgb("#f5f5f5")
    for y in range(5, 14):
        for x in range(6, 15):
            img.putpixel((x, y), black if ((x - 6) // 3 + (y - 5) // 3) % 2 == 0 else white)
    for y in range(5, 14):
        img.putpixel((15, y), black)
    for x in range(6, 16):
        img.putpixel((x, 4), black)
        img.putpixel((x, 14), black)
    base, base_dk = hexrgb("#8d9399"), hexrgb("#4b5057")
    for y in range(28, 32):
        for x in range(1, 9):
            img.putpixel((x, y), base_dk if y in (28, 31) or x in (1, 8) else base)
    return img


def make_stairs():
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    frame, frame_lt, dark = hexrgb("#6e737c"), hexrgb("#b9bec6"), hexrgb("#1b1d22")
    fill(img, 0, 0, 16, 16, frame)
    fill(img, 2, 2, 12, 12, dark)
    steps = [hexrgb("#c4c8ce"), hexrgb("#9aa0aa"), hexrgb("#7d828b"), hexrgb("#5a5f68"), hexrgb("#3a3e45")]
    for i, c in enumerate(steps):
        fill(img, 2, 2 + i * 2, 12, 2, c)
        for x in range(2, 14):
            img.putpixel((x, 2 + i * 2), frame_lt if i == 0 else c)
    for i in range(16):
        img.putpixel((i, 0), frame_lt)
        img.putpixel((0, i), frame_lt)
        img.putpixel((i, 15), hexrgb("#4e525a"))
        img.putpixel((15, i), hexrgb("#4e525a"))
    return img


# --------------------------------------------------------------------------- nature

def make_tree():
    """32x32 top-down tree: round canopy over a short trunk, with a shadow."""
    img = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
    shadow = hexrgb("#000000", 60)
    for y in range(26, 31):
        for x in range(6, 26):
            if ((x - 16) / 10) ** 2 + ((y - 28) / 2.6) ** 2 <= 1:
                img.putpixel((x, y), shadow)
    trunk, trunk_dk = hexrgb("#8d5a2b"), hexrgb("#5d3a1a")
    for y in range(20, 29):
        for x in range(13, 19):
            img.putpixel((x, y), trunk_dk if x in (13, 18) else trunk)
    leaves = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
    blob(leaves, 0, 0, 32, 32, [(16, 12, 10), (9, 15, 7), (23, 15, 7), (16, 18, 8)], HEDGE)
    blob(leaves, 0, 0, 32, 32, [(13, 9, 5), (20, 10, 4)], HEDGE_LT)
    rng = random.Random(9)
    for _ in range(40):
        x, y = rng.randrange(32), rng.randrange(32)
        if leaves.getpixel((x, y))[3]:
            leaves.putpixel((x, y), rng.choice([HEDGE_DK, HEDGE_LT, hexrgb("#6cc24a")]))
    outline(leaves, HEDGE_DK)
    img.alpha_composite(leaves)
    return img


def make_rock():
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    base, dark, light = hexrgb("#9aa0aa"), hexrgb("#5f646d"), hexrgb("#d0d4da")
    blob(img, 0, 0, 16, 16, [(8, 9, 6), (5, 10, 4), (11, 10, 4)], base)
    blob(img, 0, 0, 16, 16, [(6, 7, 2)], light)
    for x in range(16):
        for y in range(11, 16):
            if img.getpixel((x, y))[3] and y >= 12:
                img.putpixel((x, y), dark)
    outline(img, hexrgb("#3a3e45"))
    return img


def make_shrub():
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    blob(img, 0, 0, 16, 16, [(8, 8, 6), (4, 10, 4), (12, 10, 4)], HEDGE)
    blob(img, 0, 0, 16, 16, [(6, 6, 2)], HEDGE_LT)
    for (x, y) in ((5, 9), (10, 7), (11, 11), (7, 12)):
        img.putpixel((x, y), hexrgb("#e53950"))
    outline(img, HEDGE_DK)
    return img


def make_key():
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    draw_grid(img, 0, 0, KEY, KEY_PALETTE)
    return img


def make_single(rows, palette):
    img = Image.new("RGBA", (len(rows[0]), len(rows)), (0, 0, 0, 0))
    draw_grid(img, 0, 0, rows, palette)
    return img


def make_all():
    """Every library-only image, by file name."""
    return {
        "topdown_tiles.png": make_topdown_tiles(),
        "hero.png": make_hero(),
        "slime.png": make_frames(SLIME, SLIME_PALETTE),
        "beetle.png": make_frames(BEETLE, BEETLE_PALETTE),
        "gem.png": make_gem(),
        "key.png": make_key(),
        "lock_block.png": make_single(LOCK_BLOCK, LOCK_BLOCK_PALETTE),
        "spikes.png": make_spikes(),
        "spike_trap.png": make_single(SPIKE_TRAP, SPIKE_TRAP_PALETTE),
        "spring.png": make_single(SPRING, SPRING_PALETTE),
        "checkpoint.png": make_frames(CHECKPOINT, CHECKPOINT_PALETTE),
        "goal_flag.png": make_goal_flag(),
        "stairs.png": make_stairs(),
        "tree.png": make_tree(),
        "rock.png": make_rock(),
        "shrub.png": make_shrub(),
    }
