#!/usr/bin/env python3
"""Generates Studio's starter library in library/.

The library is what Studio's Toolbox offers: images, tilesets, Luau
behaviours and ready-made objects (a coin with its collider, animation and
script; a patrolling enemy; a locked block and its key; ...). Inserting an
object copies the files it needs into the project, keeping their ids, so
inserting the same thing twice never duplicates files.

Produces:
  library/art/*.png                 images (platformer art is shared with examples/platformer)
  library/scripts/*.luau            behaviours (the hand-written ones are not generated;
                                    coin, level_rules and moving_platform are copied from the example)
  library/objects/*.entities.json   ready-made objects ("px.entities" documents, like Studio's clipboard)
  library/library.json              the index (format "px.library")

Ids are deterministic (UUIDv5), and assets shared with the platformer example
keep the example's ids, so a project made from that example already has them.

Requires Pillow:  python3 -m pip install pillow
Usage:            python3 tools/generate_library.py
"""

import json
import pathlib
import shutil
import uuid

import generate_example_project as example
import library_art

ROOT = pathlib.Path(__file__).resolve().parent.parent
LIB = ROOT / "library"
EXAMPLE = ROOT / "examples" / "platformer"
NS = uuid.UUID("0c1a9a3e-56f1-4f0c-9d0e-7b7f3c2a8e41")

P, T = "platformer", "topdown"
BOTH = [P, T]


def lid(name: str) -> str:
    return str(uuid.uuid5(NS, name))


# --------------------------------------------------------------------------- assets

# key: (file, type, display name, category, game types, extra)
# Platformer art and three scripts come from the example (same files, same ids).
SHARED_TEXTURES = {
    "tiles": ("tiles.png", "Platformer Tiles", "Tiles", [P], {"tileSize": [16, 16], "tilesetColumns": 4}),
    "player": ("player.png", "Platformer Hero", "Characters", [P], {}),
    "coin": ("coin.png", "Coin", "Items", BOTH, {}),
    "scenery": ("scenery.png", "Scenery", "Decoration", [P], {}),
    "platform": ("platform.png", "Platform", "Platforms", [P], {}),
}
SHARED_SCRIPTS = {
    "coin": ("coin.luau", "Coin", "Collect a coin: adds points, then disappears"),
    "level_rules": ("level_rules.luau", "Level Rules", "Counts collected coins and can end the level"),
    "moving_platform": ("moving_platform.luau", "Moving Platform", "Glides back and forth, carrying riders"),
}
LIBRARY_TEXTURES = {
    "topdown_tiles": ("topdown_tiles.png", "Top-down Tiles", "Tiles", [T],
                      {"tileSize": [16, 16], "tilesetColumns": 8}),
    "hero": ("hero.png", "Top-down Hero", "Characters", [T], {}),
    "slime": ("slime.png", "Slime", "Enemies", [P], {}),
    "beetle": ("beetle.png", "Beetle", "Enemies", [T], {}),
    "gem": ("gem.png", "Gem", "Items", BOTH, {}),
    "key": ("key.png", "Key", "Items", BOTH, {}),
    "lock_block": ("lock_block.png", "Lock Block", "Items", BOTH, {}),
    "spikes": ("spikes.png", "Spikes", "Hazards", [P], {}),
    "spike_trap": ("spike_trap.png", "Spike Trap", "Hazards", [T], {}),
    "spring": ("spring.png", "Spring", "Platforms", [P], {}),
    "checkpoint": ("checkpoint.png", "Checkpoint", "Goals", BOTH, {}),
    "goal_flag": ("goal_flag.png", "Goal Flag", "Goals", [P], {}),
    "stairs": ("stairs.png", "Stairs", "Goals", [T], {}),
    "tree": ("tree.png", "Tree", "Decoration", [T], {}),
    "rock": ("rock.png", "Rock", "Decoration", [T], {}),
    "shrub": ("shrub.png", "Shrub", "Decoration", [T], {}),
}
LIBRARY_SCRIPTS = {
    "hazard": ("hazard.luau", "Hazard", "Sends the player back to its spawn point on touch"),
    "patrol": ("patrol.luau", "Patrol", "Walks back and forth"),
    "spring": ("spring.luau", "Spring", "Launches what lands on it"),
    "goal": ("goal.luau", "Goal", "Completes the level when the player arrives"),
    "checkpoint": ("checkpoint.luau", "Checkpoint", "The player respawns here from now on"),
    "key": ("key.luau", "Key", "Picked up; opens lock blocks with the same name"),
    "lock_block": ("lock_block.luau", "Lock Block", "Disappears when its key is collected"),
    "sign": ("sign.luau", "Sign", "Shows a message while the player stands at it"),
}

TEX = {k: example.gid("asset/" + k) for k in SHARED_TEXTURES}
TEX.update({k: lid("asset/texture/" + k) for k in LIBRARY_TEXTURES})
SCRIPT = {k: example.gid("script/" + k) for k in SHARED_SCRIPTS}
SCRIPT.update({k: lid("asset/script/" + k) for k in LIBRARY_SCRIPTS})


# --------------------------------------------------------------------------- components

def transform(x=0, y=0):
    return {"Transform": {"position": [x, y]}}


def sprite(texture, size, source=None, layer=5):
    s = {"texture": TEX[texture], "size": list(size), "layer": layer}
    if source:
        s["source"] = list(source)
    return {"Sprite": s}


def animation(frames, fps=8):
    return {"SpriteAnimation": {"frames": [list(f) for f in frames], "framesPerSecond": fps}}


def collider(size, offset=(0, 0), trigger=False):
    return {"BoxCollider": {"size": list(size), "offset": list(offset), "isTrigger": trigger}}


def scripts(*items):
    return {"Scripts": [{"script": SCRIPT[name], "properties": props} for name, props in items]}


def strip(n, w=16, h=16):
    return [[i * w, 0, w, h] for i in range(n)]


def entity(key, name, *component_dicts, parent=None):
    components = {}
    for c in component_dicts:
        components.update(c)
    e = {"id": lid("object/entity/" + key), "name": name}
    if parent:
        e["parent"] = lid("object/entity/" + parent)
    e["components"] = components
    return e


def scatter(x, y, one_in):
    """True for about one cell in `one_in`, spread without visible lines (an integer hash)."""
    h = (x * 374761393 + y * 668265263) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return (h ^ (h >> 16)) % one_in == 0


def tile_rows(grid):
    return [" ".join("." if t is None else str(t) for t in row) for row in grid]


def tilemap(texture, columns, grid, solid, layer):
    return {"Tilemap": {"tileset": TEX[texture], "tileSize": [16, 16], "tilesetColumns": columns,
                        "solid": solid, "layer": layer, "rows": tile_rows(grid)}}


# --------------------------------------------------------------------------- objects

def objects():
    """(key, name, category, game types, description, icon (texture, source), entities)."""
    out = []

    def add(key, name, category, types, description, icon, *entities):
        out.append((key, name, category, types, description, icon, list(entities)))

    # Characters. (Studio's Create > Player makes the same for the project's game type.)
    add("platformer_player", "Platformer Hero", "Characters", [P],
        "The player: runs and jumps with the arrow keys and Space.", ("player", None),
        entity("platformer_player", "Player", transform(), sprite("player", (16, 16), layer=10),
               collider((10, 14), (0, 1)), {"KinematicBody": {}}, {"PlatformerController": {}}, {"Player": {}}))
    add("topdown_player", "Top-down Hero", "Characters", [T],
        "The player: walks in every direction with the arrow keys or WASD.", ("hero", None),
        entity("topdown_player", "Player", transform(), sprite("hero", (16, 16), layer=10),
               collider((10, 8), (0, 4)), {"KinematicBody": {}}, {"TopDownController": {}}, {"Player": {}}))

    # Items.
    add("coin", "Coin", "Items", BOTH, "Collect for 1 point. Counted by Level Rules.", ("coin", (0, 0, 16, 16)),
        entity("coin", "Coin", transform(), sprite("coin", (16, 16), (0, 0, 16, 16)), animation(strip(4)),
               collider((10, 12), trigger=True), {"Tags": ["coin"]}, scripts(("coin", {"value": 1}))))
    add("gem", "Gem", "Items", BOTH, "Collect for 5 points. Counted by Level Rules like coins.",
        ("gem", (0, 0, 16, 16)),
        entity("gem", "Gem", transform(), sprite("gem", (16, 16), (0, 0, 16, 16)), animation(strip(4), 6),
               collider((10, 10), trigger=True), {"Tags": ["coin"]}, scripts(("coin", {"value": 5}))))
    add("key", "Key", "Items", BOTH, "Picked up by the player; opens Lock Blocks with the same key name.",
        ("key", None),
        entity("key", "Key", transform(), sprite("key", (16, 16)), collider((12, 8), trigger=True),
               scripts(("key", {"keyName": "gold"}))))
    add("lock_block", "Lock Block", "Items", BOTH, "A solid block that disappears when its Key is collected.",
        ("lock_block", None),
        entity("lock_block", "Lock Block", transform(), sprite("lock_block", (16, 16), layer=2),
               collider((16, 16)), scripts(("lock_block", {"keyName": "gold"}))))

    # Hazards and enemies.
    add("spikes", "Spikes", "Hazards", [P], "Touching them sends the player back to its spawn point.",
        ("spikes", None),
        entity("spikes", "Spikes", transform(), sprite("spikes", (16, 16), layer=2),
               collider((14, 8), (0, 4), trigger=True), scripts(("hazard", {}))))
    add("spike_trap", "Spike Trap", "Hazards", [T], "Stepping on it sends the player back to its spawn point.",
        ("spike_trap", None),
        entity("spike_trap", "Spike Trap", transform(), sprite("spike_trap", (16, 16), layer=1),
               collider((12, 12), trigger=True), scripts(("hazard", {}))))
    add("slime", "Slime", "Enemies", [P], "Patrols left and right; touching it sends the player back.",
        ("slime", (0, 0, 16, 16)),
        entity("slime", "Slime", transform(), sprite("slime", (16, 16), (0, 0, 16, 16), layer=6),
               animation(strip(2), 3), collider((12, 9), (0, 3), trigger=True),
               scripts(("patrol", {"offset": [48, 0], "period": 3}), ("hazard", {}))))
    add("beetle", "Beetle", "Enemies", [T], "Patrols up and down; touching it sends the player back.",
        ("beetle", (0, 0, 16, 16)),
        entity("beetle", "Beetle", transform(), sprite("beetle", (16, 16), (0, 0, 16, 16), layer=6),
               animation(strip(2), 6), collider((10, 10), trigger=True),
               scripts(("patrol", {"offset": [0, 48], "period": 3}), ("hazard", {}))))

    # Platforms.
    add("spring", "Spring", "Platforms", [P], "Launches the player high into the air.", ("spring", None),
        entity("spring", "Spring", transform(), sprite("spring", (16, 16), layer=2),
               collider((12, 6), (0, 3), trigger=True), scripts(("spring", {"strength": 420}))))
    add("moving_platform", "Moving Platform", "Platforms", [P], "Glides back and forth, carrying the player.",
        ("platform", None),
        entity("moving_platform", "Moving Platform", transform(), sprite("platform", (48, 16), layer=1),
               collider((48, 16)), scripts(("moving_platform", {"offset": [80, 0], "period": 4}))))
    add("crate", "Crate", "Platforms", BOTH, "A solid block to stand on or walk around.", ("tiles", (48, 0, 16, 16)),
        entity("crate", "Crate", transform(), sprite("tiles", (16, 16), (48, 0, 16, 16), layer=2),
               collider((16, 16))))

    # Goals and rules.
    add("checkpoint", "Checkpoint", "Goals", BOTH, "After touching it, the player respawns here.",
        ("checkpoint", (0, 0, 16, 16)),
        entity("checkpoint", "Checkpoint", transform(), sprite("checkpoint", (16, 16), (0, 0, 16, 16), layer=2),
               animation(strip(2), 3), collider((10, 16), trigger=True), scripts(("checkpoint", {}))))
    add("goal_flag", "Goal Flag", "Goals", [P], "Reaching it completes the level.", ("goal_flag", None),
        entity("goal_flag", "Goal Flag", transform(), sprite("goal_flag", (16, 32), layer=2),
               collider((10, 28), (0, 2), trigger=True), scripts(("goal", {"message": "LEVEL COMPLETE!"}))))
    add("stairs", "Exit Stairs", "Goals", [T], "Reaching them completes the level.", ("stairs", None),
        entity("stairs", "Exit Stairs", transform(), sprite("stairs", (16, 16), layer=1),
               collider((10, 10), trigger=True), scripts(("goal", {"message": "LEVEL COMPLETE!"}))))
    add("level_rules", "Collect-All Rules", "Goals", BOTH,
        "Shows how many coins and gems are left on the HUD and completes the level when all are collected.",
        ("coin", (0, 0, 16, 16)),
        entity("level_rules", "Level Rules",
               scripts(("level_rules", {"tag": "coin", "label": "COINS", "message": "ALL COINS COLLECTED!",
                                        "complete": True}))))
    add("sign", "Sign", "Goals", BOTH, "Shows a message while the player stands at it.", ("scenery", (16, 16, 16, 16)),
        entity("sign", "Sign", transform(), sprite("scenery", (16, 16), (16, 16, 16, 16), layer=-1),
               collider((16, 16), trigger=True), scripts(("sign", {"message": "Hello! Collect everything you can."}))))

    # Ground and walls.
    ground = [[0] * 16, [1] * 16, [6] * 16]
    add("ground_strip", "Ground", "Ground", [P], "A solid strip of grass and dirt, 16 tiles wide. Paint more in it.",
        ("tiles", (0, 0, 16, 16)),
        entity("ground_strip", "Ground", transform(), tilemap("tiles", 4, ground, True, 0)))
    meadow = [[1 if scatter(x, y, 10) else 0 for x in range(20)] for y in range(12)]
    add("meadow", "Meadow", "Ground", [T], "A 20 x 12 field of grass to walk on (not solid).",
        ("topdown_tiles", (0, 0, 16, 16)),
        entity("meadow", "Meadow", transform(), tilemap("topdown_tiles", 8, meadow, False, -10)))
    walls = [[8 if x in (0, 19) or y in (0, 11) else None for x in range(20)] for y in range(12)]
    add("wall_ring", "Wall Ring", "Ground", [T], "Solid stone walls around a 20 x 12 area. Paint more in it.",
        ("topdown_tiles", (0, 16, 16, 16)),
        entity("wall_ring", "Walls", transform(), tilemap("topdown_tiles", 8, walls, True, 0)))
    pond = [[11] * 4] + [[4] * 4 for _ in range(2)]
    add("pond", "Pond", "Ground", [T], "A small pond; the water is solid.", ("topdown_tiles", (64, 0, 16, 16)),
        entity("pond", "Pond", transform(), tilemap("topdown_tiles", 8, pond, True, -5)))

    # Decoration.
    add("cloud", "Cloud", "Decoration", [P], "A cloud in the background.", ("scenery", (0, 0, 32, 16)),
        entity("cloud", "Cloud", transform(), sprite("scenery", (32, 16), (0, 0, 32, 16), layer=-20)))
    add("bush", "Bush", "Decoration", [P], "A bush to stand in front of.", ("scenery", (32, 0, 32, 16)),
        entity("bush", "Bush", transform(), sprite("scenery", (32, 16), (32, 0, 32, 16), layer=-1)))
    add("flowers", "Flowers", "Decoration", BOTH, "A few flowers.", ("scenery", (0, 16, 16, 16)),
        entity("flowers", "Flowers", transform(), sprite("scenery", (16, 16), (0, 16, 16, 16), layer=-1)))
    add("tree", "Tree", "Decoration", [T], "A tree; its trunk is solid.", ("tree", None),
        entity("tree", "Tree", transform(), sprite("tree", (32, 32), layer=8), collider((10, 8), (0, 10))))
    add("rock", "Rock", "Decoration", [T], "A solid rock.", ("rock", None),
        entity("rock", "Rock", transform(), sprite("rock", (16, 16), layer=4), collider((12, 10), (0, 2))))
    add("shrub", "Shrub", "Decoration", [T], "A small berry bush (not solid).", ("shrub", None),
        entity("shrub", "Shrub", transform(), sprite("shrub", (16, 16), layer=4)))
    return out


# --------------------------------------------------------------------------- output

def main():
    for sub in ("art", "objects"):
        if (LIB / sub).exists():
            shutil.rmtree(LIB / sub)
        (LIB / sub).mkdir(parents=True)
    (LIB / "scripts").mkdir(parents=True, exist_ok=True)

    assets = []
    for key, (file, name, category, types, extra) in SHARED_TEXTURES.items():
        shutil.copyfile(EXAMPLE / "assets" / file, LIB / "art" / file)
        assets.append({"id": TEX[key], "type": "texture", "name": name, "path": "art/" + file, "filter": "nearest",
                       "category": category, "gameTypes": types, **extra})
    images = library_art.make_all()
    for key, (file, name, category, types, extra) in LIBRARY_TEXTURES.items():
        images[file].save(LIB / "art" / file)
        assets.append({"id": TEX[key], "type": "texture", "name": name, "path": "art/" + file, "filter": "nearest",
                       "category": category, "gameTypes": types, **extra})
    for key, (file, name, description) in SHARED_SCRIPTS.items():
        shutil.copyfile(EXAMPLE / "scripts" / file, LIB / "scripts" / file)
        assets.append({"id": SCRIPT[key], "type": "script", "name": name, "path": "scripts/" + file,
                       "category": "Behaviours", "gameTypes": BOTH, "description": description})
    for key, (file, name, description) in LIBRARY_SCRIPTS.items():
        assert (LIB / "scripts" / file).exists(), f"library/scripts/{file} is missing"
        assets.append({"id": SCRIPT[key], "type": "script", "name": name, "path": "scripts/" + file,
                       "category": "Behaviours", "gameTypes": BOTH, "description": description})

    object_list = []
    for key, name, category, types, description, icon, entities in objects():
        path = f"objects/{key}.entities.json"
        doc = {"format": "px.entities", "formatVersion": 1, "sceneFormatVersion": 3, "entities": entities}
        (LIB / path).write_text(example.dump_pretty(doc) + "\n")
        texture, source = icon
        item = {"id": lid("object/" + key), "name": name, "category": category, "gameTypes": types,
                "description": description, "path": path, "icon": {"texture": TEX[texture]}}
        if source:
            item["icon"]["source"] = list(source)
        object_list.append(item)

    library = {"format": "px.library", "formatVersion": 1, "name": "Starter Library", "assets": assets,
               "objects": object_list}
    (LIB / "library.json").write_text(example.dump_pretty(library) + "\n")
    print(f"Wrote {LIB}: {len(assets)} assets, {len(object_list)} objects")


if __name__ == "__main__":
    main()
