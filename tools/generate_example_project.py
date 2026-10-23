#!/usr/bin/env python3
"""Generates the example platformer project in examples/platformer/.

Produces:
  assets/tiles.png, assets/player.png, assets/coin.png, assets/scenery.png,
  assets/platform.png
  scenes/level1.scene.json
  project.json
The Luau scripts in scripts/ are hand-written and not generated.

All art is original and drawn procedurally here. Ids are deterministic
(UUIDv5 from fixed names) so regenerating produces identical files.

Requires Pillow:  python3 -m pip install pillow
Usage:            python3 tools/generate_example_project.py

The scene is then rewritten with `px-headless examples/platformer --resave`
(found in build/*/apps/headless), so it is stored exactly as Studio saves it
and opening and saving it in Studio changes nothing. A test checks this.
"""

import json
import pathlib
import subprocess
import uuid

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent / "examples" / "platformer"
NS = uuid.UUID("6f1d8a52-3c0e-4b8e-9a51-0e6c2f7b9d10")


def gid(name: str) -> str:
    return str(uuid.uuid5(NS, name))


def hexrgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)) + (255,)


def draw_grid(img, ox, oy, rows, palette):
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            if ch in palette:
                img.putpixel((ox + x, oy + y), palette[ch])


# --------------------------------------------------------------------------- art

def make_tiles():
    """4x2 tileset of 16x16 tiles."""
    img = Image.new("RGBA", (64, 32), (0, 0, 0, 0))
    grass_hi, grass, grass_dk = hexrgb("#8fd16a"), hexrgb("#5bb450"), hexrgb("#3e8e41")
    dirt, dirt_dk, dirt_lt = hexrgb("#a0663c"), hexrgb("#7d4a2a"), hexrgb("#c48450")
    stone, stone_dk, stone_lt = hexrgb("#8a8f98"), hexrgb("#5f646d"), hexrgb("#b7bcc4")
    wood, wood_dk, wood_lt = hexrgb("#b07a3c"), hexrgb("#7a5226"), hexrgb("#d39a55")
    metal, metal_dk, metal_lt = hexrgb("#6f86a3"), hexrgb("#4a5d75"), hexrgb("#a9bed6")

    def dirt_fill(ox, oy, start_row=0, dark=False):
        base = dirt_dk if dark else dirt
        for y in range(start_row, 16):
            for x in range(16):
                img.putpixel((ox + x, oy + y), base)
        # deterministic specks
        for i, (sx, sy) in enumerate([(3, 6), (11, 4), (7, 11), (13, 13), (2, 14), (9, 8), (5, 2), (14, 9)]):
            if sy >= start_row:
                img.putpixel((ox + sx, oy + sy), dirt_dk if not dark and i % 2 == 0 else dirt_lt if not dark else dirt)

    # 0: grass top
    dirt_fill(0, 0)
    for x in range(16):
        top = 4 + (1 if x % 5 == 2 else 0)
        for y in range(top):
            img.putpixel((x, y), grass)
        img.putpixel((x, 0), grass_hi)
        img.putpixel((x, top), grass_dk)
        if x % 4 == 1:
            img.putpixel((x, 1), grass_hi)
    # 1: dirt
    dirt_fill(16, 0)
    # 2: stone brick
    for y in range(16):
        for x in range(16):
            img.putpixel((32 + x, y), stone)
    for y in (0, 8):
        for x in range(16):
            img.putpixel((32 + x, y), stone_lt)
    for y in (7, 15):
        for x in range(16):
            img.putpixel((32 + x, y), stone_dk)
    for y in range(0, 8):
        img.putpixel((32 + 11, y), stone_dk)
    for y in range(8, 16):
        img.putpixel((32 + 4, y), stone_dk)
    # 3: crate
    for y in range(16):
        for x in range(16):
            edge = x in (0, 15) or y in (0, 15)
            img.putpixel((48 + x, y), wood_dk if edge else wood)
    for i in range(1, 15):
        img.putpixel((48 + i, i), wood_dk)
        img.putpixel((48 + i, 15 - i), wood_lt if i % 3 else wood_dk)
    for x in range(1, 15):
        img.putpixel((48 + x, 1), wood_lt)
    # 4: grass left edge, 5: grass right edge (row 1)
    for idx, flip in ((0, False), (1, True)):
        ox = idx * 16
        dirt_fill(ox, 16)
        for x in range(16):
            xx = 15 - x if flip else x
            top = 4 if x > 2 else 16
            for y in range(min(top, 16)):
                img.putpixel((ox + xx, 16 + y), grass if x > 2 or y < 4 else grass_dk)
            img.putpixel((ox + xx, 16), grass_hi)
    # 6: deep dirt
    dirt_fill(32, 16, dark=True)
    # 7: metal block
    for y in range(16):
        for x in range(16):
            edge = x in (0, 15) or y in (0, 15)
            img.putpixel((48 + x, 16 + y), metal_dk if edge else metal)
    for x in range(1, 15):
        img.putpixel((48 + x, 17), metal_lt)
        img.putpixel((49, 16 + x), metal_lt)
    for (rx, ry) in ((3, 3), (12, 3), (3, 12), (12, 12)):
        img.putpixel((48 + rx, 16 + ry), metal_lt)
        img.putpixel((48 + rx + 1, 16 + ry + 1), metal_dk)
    return img


PLAYER = [
    "................",
    "................",
    "....oooooooo....",
    "...oBBBBBBBBo...",
    "..oBBBBBBBBBBo..",
    "..oBBhBBBBBBBo..",
    "..oBBWWBBWWBBo..",
    "..oBBWKBBWKBBo..",
    "..oBBBBBBBBBBo..",
    "..oBBBBmmBBBBo..",
    "..oDBBBBBBBBDo..",
    "...oDDDDDDDDo...",
    "....oooooooo....",
    "....oFFo.oFFo...",
    "....oFFo.oFFo...",
    "....oooo.oooo...",
]
PLAYER_PALETTE = {
    "o": hexrgb("#2b1d2e"), "B": hexrgb("#ff7f50"), "D": hexrgb("#d9583a"), "h": hexrgb("#ffb08f"),
    "W": hexrgb("#ffffff"), "K": hexrgb("#1a1a2e"), "m": hexrgb("#8c2f39"), "F": hexrgb("#5a3a2a"),
}


def make_player():
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    draw_grid(img, 0, 0, PLAYER, PLAYER_PALETTE)
    return img


COIN_FRAMES = [
    [
        "....oooooo....",
        "...oYYYYYYo...",
        "..oYHHYYYYYo..",
        ".oYHYYYYYYYGo.",
        ".oYHYYOOYYYGo.",
        ".oYYYYOOYYYGo.",
        ".oYYYYOOYYYGo.",
        ".oYYYYOOYYYGo.",
        ".oYYYYYYYYYGo.",
        "..oYYYYYYYGo..",
        "...oGGGGGGo...",
        "....oooooo....",
    ],
    [
        ".....oooo.....",
        "....oYYYYo....",
        "...oYHYYYGo...",
        "...oHYYYYGo...",
        "...oHYOOYGo...",
        "...oYYOOYGo...",
        "...oYYOOYGo...",
        "...oYYOOYGo...",
        "...oYYYYYGo...",
        "...oYYYYYGo...",
        "....oGGGGo....",
        ".....oooo.....",
    ],
    [
        "......oo......",
        ".....oHGo.....",
        ".....oHGo.....",
        ".....oHGo.....",
        ".....oYGo.....",
        ".....oYGo.....",
        ".....oYGo.....",
        ".....oYGo.....",
        ".....oYGo.....",
        ".....oYGo.....",
        ".....oYGo.....",
        "......oo......",
    ],
]
COIN_PALETTE = {
    "o": hexrgb("#8a5a00"), "Y": hexrgb("#ffd23f"), "G": hexrgb("#e09b00"),
    "H": hexrgb("#fff6b8"), "O": hexrgb("#f0b000"),
}


def make_coin():
    """4 frames of 16x16: full, narrow, edge, narrow (spin)."""
    img = Image.new("RGBA", (64, 16), (0, 0, 0, 0))
    for i, frame in enumerate([COIN_FRAMES[0], COIN_FRAMES[1], COIN_FRAMES[2], COIN_FRAMES[1]]):
        draw_grid(img, i * 16 + 1, 2, frame, COIN_PALETTE)
    return img


def make_scenery():
    """64x32 sheet: cloud (0,0,32,16), bush (32,0,32,16), flowers (0,16,16,16), sign (16,16,16,16)."""
    img = Image.new("RGBA", (64, 32), (0, 0, 0, 0))
    white, shade, outline = hexrgb("#ffffff"), hexrgb("#dceefa"), hexrgb("#b5d7ee")
    # cloud: union of circles
    circles = [(9, 10, 6), (16, 7, 7), (23, 10, 6), (13, 11, 5), (20, 11, 5)]
    for y in range(16):
        for x in range(32):
            inside = any((x - cx) ** 2 + (y - cy) ** 2 <= r * r for cx, cy, r in circles)
            if inside:
                img.putpixel((x, y), shade if y >= 12 else white)
    for y in range(16):
        for x in range(32):
            if img.getpixel((x, y))[3] and any(
                not (0 <= x + dx < 32 and 0 <= y + dy < 16) or img.getpixel((x + dx, y + dy))[3] == 0
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))
            ):
                img.putpixel((x, y), outline)
    # bush
    g1, g2, g3 = hexrgb("#4caf50"), hexrgb("#2e7d32"), hexrgb("#81c784")
    bush = [(40, 11, 6), (48, 8, 7), (56, 11, 6)]
    for y in range(16):
        for x in range(32, 64):
            if y <= 15 and any((x - cx) ** 2 + (y - cy) ** 2 <= r * r for cx, cy, r in bush):
                img.putpixel((x, y), g1 if y > 6 else g3)
    for x in range(33, 63):
        if img.getpixel((x, 15))[3]:
            img.putpixel((x, 15), g2)
    for (bx, by) in ((44, 9), (50, 6), (54, 11), (39, 12), (58, 9)):
        img.putpixel((bx, by), g3)
    # flowers
    stem, petal, centre = hexrgb("#3e8e41"), hexrgb("#ff6f91"), hexrgb("#ffd23f")
    for fx, fy in ((4, 9), (10, 7)):
        for y in range(fy + 2, 16):
            img.putpixel((fx, 16 + y), stem)
        for dx, dy in ((0, 0), (-1, 1), (1, 1), (0, 2), (-1, -1 + 2), (1, 1)):
            img.putpixel((fx + dx, 16 + fy + dy), petal)
        img.putpixel((fx, 16 + fy + 1), centre)
    # sign
    post, board, board_dk = hexrgb("#7a5226"), hexrgb("#d39a55"), hexrgb("#7a5226")
    for y in range(8, 16):
        img.putpixel((16 + 7, 16 + y), post)
        img.putpixel((16 + 8, 16 + y), post)
    for y in range(2, 9):
        for x in range(2, 14):
            img.putpixel((16 + x, 16 + y), board_dk if y in (2, 8) or x in (2, 13) else board)
    for x in range(4, 12, 2):
        img.putpixel((16 + x, 16 + 5), board_dk)
    return img


def make_platform():
    """48x16 moving platform: wooden planks on a metal frame."""
    img = Image.new("RGBA", (48, 16), (0, 0, 0, 0))
    frame, frame_lt, frame_dk = hexrgb("#6f86a3"), hexrgb("#a9bed6"), hexrgb("#4a5d75")
    wood, wood_lt, wood_dk = hexrgb("#b07a3c"), hexrgb("#d39a55"), hexrgb("#7a5226")
    for y in range(0, 10):
        for x in range(48):
            edge = y in (0, 9) or x in (0, 47)
            img.putpixel((x, y), wood_dk if edge else (wood_lt if y == 1 else wood))
    for x in range(8, 48, 8):
        for y in range(1, 9):
            img.putpixel((x, y), wood_dk)
    for y in range(10, 14):
        for x in list(range(2, 8)) + list(range(40, 46)):
            img.putpixel((x, y), frame_dk if y == 13 or x in (2, 7, 40, 45) else frame)
    for x in (4, 42):
        img.putpixel((x, 11), frame_lt)
    return img


# ------------------------------------------------------------------------- level

# Legend: G grass, L grass-left-edge, R grass-right-edge, D dirt, E deep dirt,
# S stone, C crate, M metal, o coin, P player spawn, c cloud, b bush, f flowers,
# s sign. '.' is empty. The level is 84 x 14 tiles of 16x16 pixels.
LEVEL_WIDTH, LEVEL_HEIGHT = 84, 14

# (first column, last column, surface row) of each ground segment; gaps are pits.
# The pit between columns 62 and 69 is too wide to jump: a scripted moving
# platform (PLATFORM below) carries the player across.
GROUND = [(2, 12, 11), (16, 25, 11), (28, 33, 11), (34, 38, 10), (42, 47, 10), (48, 61, 11), (70, 81, 11)]
BLOCKS = [("C", 21, 9), ("C", 22, 9), ("C", 21, 10), ("C", 22, 10),          # crate stack
          ("M", 28, 9), ("M", 29, 9), ("M", 30, 9), ("M", 31, 9),            # low metal platform
          ("M", 44, 8), ("M", 45, 8), ("M", 46, 8)]                          # high metal platform
COINS = [(6, 10), (10, 9), (14, 8), (22, 8), (29, 8), (30, 8), (36, 9), (40, 7), (45, 6), (52, 10), (55, 10), (58, 8),
         (66, 10), (76, 10)]
DECOR = [("c", 7, 1), ("c", 18, 3), ("c", 27, 1), ("c", 42, 3), ("c", 46, 1), ("c", 57, 2), ("c", 66, 2), ("c", 77, 1),
         ("s", 4, 10), ("f", 8, 10), ("b", 24, 10), ("b", 50, 10), ("f", 60, 10), ("f", 37, 9), ("b", 73, 10),
         ("f", 79, 10)]
SPAWN = (3, 10)
# Moving platform: left edge at the first pit column, top level with the ground,
# travelling right until it touches the far side.
PLATFORM_COLUMN, PLATFORM_ROW, PLATFORM_TILES = 62, 11, 3
PLATFORM_TRAVEL = (70 - 62 - PLATFORM_TILES) * 16


def build_level():
    grid = [["." for _ in range(LEVEL_WIDTH)] for _ in range(LEVEL_HEIGHT)]
    for y in range(LEVEL_HEIGHT):
        for x in (0, 1, LEVEL_WIDTH - 2, LEVEL_WIDTH - 1):
            grid[y][x] = "S"
    for first, last, surface in GROUND:
        for x in range(first, last + 1):
            # Exposed segment ends get edge tiles unless they touch a wall or a higher neighbour.
            left_open = x == first and grid[surface][x - 1] == "."
            right_open = x == last and x + 1 < LEVEL_WIDTH and grid[surface][x + 1] == "." and not any(
                f == x + 1 and s <= surface for f, _, s in GROUND)
            grid[surface][x] = "L" if left_open else "R" if right_open else "G"
            for y in range(surface + 1, LEVEL_HEIGHT):
                grid[y][x] = "E" if y == LEVEL_HEIGHT - 1 else "D"
    for ch, x, y in BLOCKS:
        grid[y][x] = ch
    for x, y in COINS:
        assert grid[y][x] == ".", f"coin at {x},{y} overlaps a tile"
        grid[y][x] = "o"
    for ch, x, y in DECOR:
        assert grid[y][x] == ".", f"decoration at {x},{y} overlaps"
        grid[y][x] = ch
    grid[SPAWN[1]][SPAWN[0]] = "P"
    return ["".join(row) for row in grid]


LEVEL = build_level()

TILE_INDEX = {"G": 0, "D": 1, "S": 2, "C": 3, "L": 4, "R": 5, "E": 6, "M": 7}


def tile_rows(level):
    width = max(len(r) for r in level)
    rows = []
    for r in level:
        r = r.ljust(width, ".")
        rows.append(" ".join(str(TILE_INDEX[ch]) if ch in TILE_INDEX else "." for ch in r))
    return rows, width


def num(v):
    return int(v) if float(v).is_integer() else v


def dump_pretty(value, indent=0):
    """Same layout as the engine's serializer (detail::dumpPretty): 2-space
    indent, arrays of numbers/bools on a single line."""
    pad, inner = "  " * indent, "  " * (indent + 1)
    if isinstance(value, dict):
        if not value:
            return "{}"
        items = [f"{inner}{json.dumps(k)}: {dump_pretty(v, indent + 1)}" for k, v in value.items()]
        return "{\n" + ",\n".join(items) + "\n" + pad + "}"
    if isinstance(value, list):
        if all(isinstance(e, (int, float, bool)) or e is None for e in value):
            return "[" + ", ".join(json.dumps(e) for e in value) + "]"
        return "[\n" + ",\n".join(inner + dump_pretty(e, indent + 1) for e in value) + "\n" + pad + "]"
    return json.dumps(value)


def main():
    (ROOT / "assets").mkdir(parents=True, exist_ok=True)
    (ROOT / "scenes").mkdir(parents=True, exist_ok=True)
    make_tiles().save(ROOT / "assets" / "tiles.png")
    make_player().save(ROOT / "assets" / "player.png")
    make_coin().save(ROOT / "assets" / "coin.png")
    make_scenery().save(ROOT / "assets" / "scenery.png")
    make_platform().save(ROOT / "assets" / "platform.png")

    tex = {name: gid("asset/" + name) for name in ("tiles", "player", "coin", "scenery", "platform")}
    script = {name: gid("script/" + name) for name in ("coin", "level_rules", "moving_platform")}
    scene_id = gid("asset/level1")
    player_id = gid("entity/player")

    rows, width = tile_rows(LEVEL)
    entities = [
        {"id": gid("entity/camera"), "name": "Main Camera", "components": {
            "Transform": {"position": [160, 112]},
            "Camera": {"follow": player_id, "zoom": 1, "clampToBounds": True}}},
        {"id": gid("entity/level"), "name": "Level", "components": {
            "Transform": {"position": [0, 0]},
            "Tilemap": {"tileset": tex["tiles"], "tileSize": [16, 16], "tilesetColumns": 4,
                        "solid": True, "layer": 0, "rows": rows}}},
    ]

    scenery_sources = {"c": ([0, 0, 32, 16], [32, 16], -20), "b": ([32, 0, 32, 16], [32, 16], -1),
                       "f": ([0, 16, 16, 16], [16, 16], -1), "s": ([16, 16, 16, 16], [16, 16], -1)}
    # Group entities (no components) keep the hierarchy tidy in Studio.
    # Children of a group without a Transform keep their own positions.
    coins_group = gid("entity/group/coins")
    scenery_group = gid("entity/group/scenery")
    coins = []
    scenery = []
    coin_count = 0
    spawn = None
    for y, row in enumerate(LEVEL):
        for x, ch in enumerate(row):
            cx, cy = x * 16 + 8, y * 16 + 8
            if ch == "o":
                coin_count += 1
                coins.append({"id": gid(f"entity/coin/{coin_count}"), "name": f"Coin {coin_count}",
                              "parent": coins_group, "components": {
                    "Transform": {"position": [cx, cy]},
                    "Sprite": {"texture": tex["coin"], "source": [0, 0, 16, 16], "size": [16, 16], "layer": 5},
                    "SpriteAnimation": {"frames": [[i * 16, 0, 16, 16] for i in range(4)], "framesPerSecond": 8},
                    "BoxCollider": {"size": [10, 12], "isTrigger": True},
                    "Tags": ["coin"],
                    "Scripts": [{"script": script["coin"], "properties": {"value": 1}}]}})
            elif ch in scenery_sources:
                source, size, layer = scenery_sources[ch]
                name = {"c": "Cloud", "b": "Bush", "f": "Flowers", "s": "Sign"}[ch]
                # Ground decorations sit on the tile below; clouds float.
                pos = [cx + (8 if size[0] == 32 else 0), cy]
                scenery.append({"id": gid(f"entity/{name.lower()}/{x}/{y}"), "name": name,
                                "parent": scenery_group, "components": {
                    "Transform": {"position": pos},
                    "Sprite": {"texture": tex["scenery"], "source": source, "size": size, "layer": layer}}})
            elif ch == "P":
                spawn = [cx, cy]
    entities.append({"id": scenery_group, "name": "Scenery", "components": {}})
    entities.extend(scenery)
    entities.append({"id": coins_group, "name": "Coins", "components": {}})
    entities.extend(coins)

    width_px = PLATFORM_TILES * 16
    entities.append({"id": gid("entity/moving-platform"), "name": "Moving Platform", "components": {
        "Transform": {"position": [PLATFORM_COLUMN * 16 + width_px // 2, PLATFORM_ROW * 16 + 8]},
        "Sprite": {"texture": tex["platform"], "size": [width_px, 16], "layer": 1},
        "BoxCollider": {"size": [width_px, 16]},
        "Scripts": [{"script": script["moving_platform"],
                     "properties": {"offset": [PLATFORM_TRAVEL, 0], "period": 4}}]}})
    entities.append({"id": gid("entity/level-rules"), "name": "Level Rules", "components": {
        "Scripts": [{"script": script["level_rules"],
                     "properties": {"tag": "coin", "message": "ALL COINS COLLECTED!"}}]}})

    entities.append({"id": player_id, "name": "Player", "components": {
        "Transform": {"position": spawn},
        "Sprite": {"texture": tex["player"], "size": [16, 16], "layer": 10},
        "BoxCollider": {"size": [10, 14], "offset": [0, 1]},
        "KinematicBody": {},
        "PlatformerController": {"maxSpeed": 110, "jumpSpeed": 300, "gravity": 900},
        "Player": {}}})

    scene = {
        "format": "px.scene",
        "formatVersion": 3,
        "name": "Level 1",
        "settings": {"backgroundColor": "#9bd4f0", "killPlaneY": num(len(LEVEL) * 16 + 40)},
        "entities": entities,
    }
    (ROOT / "scenes" / "level1.scene.json").write_text(dump_pretty(scene) + "\n")

    project = {
        "format": "px.project",
        "formatVersion": 2,
        "id": gid("project/example-platformer"),
        "name": "Example Platformer",
        "engineVersion": "0.3.0",
        "startScene": scene_id,
        "settings": {"gameType": "platformer", "tickRate": 60,
                     "display": {"virtualResolution": [320, 180], "scaling": "integer", "windowScale": 3},
                     "scripting": {"apiVersion": 1}},
        "input": {"actions": {
            "move_left": ["Left", "A"],
            "move_right": ["Right", "D"],
            "move_up": ["Up", "W"],
            "move_down": ["Down", "S"],
            "jump": ["Space", "Z", "Up", "W"]}},
        "assets": [
            {"id": tex["tiles"], "type": "texture", "name": "Tiles", "path": "assets/tiles.png", "filter": "nearest"},
            {"id": tex["player"], "type": "texture", "name": "Player", "path": "assets/player.png", "filter": "nearest"},
            {"id": tex["coin"], "type": "texture", "name": "Coin", "path": "assets/coin.png", "filter": "nearest"},
            {"id": tex["scenery"], "type": "texture", "name": "Scenery", "path": "assets/scenery.png", "filter": "nearest"},
            {"id": tex["platform"], "type": "texture", "name": "Platform", "path": "assets/platform.png", "filter": "nearest"},
            {"id": script["coin"], "type": "script", "name": "Coin", "path": "scripts/coin.luau"},
            {"id": script["level_rules"], "type": "script", "name": "Level Rules", "path": "scripts/level_rules.luau"},
            {"id": script["moving_platform"], "type": "script", "name": "Moving Platform",
             "path": "scripts/moving_platform.luau"},
            {"id": scene_id, "type": "scene", "name": "Level 1", "path": "scenes/level1.scene.json"},
        ],
    }
    (ROOT / "project.json").write_text(dump_pretty(project) + "\n")
    print("\n".join(LEVEL))
    print(f"Wrote example project to {ROOT} ({coin_count} coins, level {width}x{len(LEVEL)} tiles)")
    build = ROOT.parent.parent / "build"
    tools = list(build.glob("*/apps/headless/px-headless")) + list(build.glob("*/apps/headless/px-headless.exe"))
    tools.sort(key=lambda p: p.stat().st_mtime, reverse=True)  # the most recently built one
    if tools:
        subprocess.run([str(tools[0]), str(ROOT), "--resave"], check=True)
    else:
        print("Build px-headless and run: px-headless examples/platformer --resave")


if __name__ == "__main__":
    main()
