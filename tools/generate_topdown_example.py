#!/usr/bin/env python3
"""Generates the top-down example project in examples/topdown/.

A walled garden: find the gold key, which opens the gate in the hedge, then
reach the stairs. Gems are optional points; spike traps and a beetle guard
two of them. Everything comes from the starter library (library/), with the
library's asset ids, so inserting library objects into a copy of this
project never duplicates files.

Run tools/generate_library.py first. Requires Pillow (through it).
Usage: python3 tools/generate_topdown_example.py

The project is then rewritten with `px-headless examples/topdown --resave`
(found in build/*/apps/headless) so it is stored exactly as Studio saves it.
"""

import pathlib
import shutil
import subprocess
import uuid

import generate_example_project as example
import generate_library as lib

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "examples" / "topdown"
NS = uuid.UUID("5d0b7f0e-3f7a-4d55-8f1c-2a4c9c6e1b77")

W, H = 24, 16  # tiles
GATE = (12, 7)
POND = (6, 2, 4, 3)  # x, y, width, height: bank row, then water
PATH = [(2, y) for y in range(1, 13)] + [(x, 7) for x in range(2, 23)] + [(22, y) for y in range(7, 13)]


def gid(name):
    return str(uuid.uuid5(NS, name))


def centre(cx, cy):
    return [cx * 16 + 8, cy * 16 + 8]


def ground_rows():
    rows = []
    for y in range(H):
        row = []
        for x in range(W):
            t = 1 if lib.scatter(x, y, 12) else 0  # flowers here and there
            px, py, pw, ph = POND
            if px - 1 <= x <= px + pw and py - 1 <= y <= py + ph:
                t = 3  # sand around the pond
            if (x, y) in PATH:
                t = 2
            row.append(t)
        rows.append(row)
    return rows


def wall_rows():
    rows = []
    for y in range(H):
        row = []
        for x in range(W):
            t = None
            if x in (0, W - 1) or y in (0, H - 1):
                t = 8  # stone wall
            elif x == GATE[0] and y != GATE[1]:
                t = 10  # hedge, with a gap for the gate
            px, py, pw, ph = POND
            if px <= x < px + pw and py <= y < py + ph:
                t = 11 if y == py else 4
            row.append(t)
        rows.append(row)
    return rows


def main():
    for sub in ("assets", "scripts", "scenes", "tests"):
        (OUT / sub).mkdir(parents=True, exist_ok=True)

    textures = ["topdown_tiles", "hero", "gem", "key", "lock_block", "spike_trap", "beetle", "stairs", "tree",
                "rock", "shrub", "scenery"]
    scripts = ["coin", "level_rules", "key", "lock_block", "hazard", "patrol", "goal", "sign"]
    assets = []
    for key in textures:
        file, name, *_ = lib.SHARED_TEXTURES.get(key) or lib.LIBRARY_TEXTURES[key]
        shutil.copyfile(lib.LIB / "art" / file, OUT / "assets" / file)
        assets.append({"id": lib.TEX[key], "type": "texture", "name": name, "path": "assets/" + file,
                       "filter": "nearest"})
    for key in scripts:
        file, name, _ = lib.SHARED_SCRIPTS.get(key) or lib.LIBRARY_SCRIPTS[key]
        shutil.copyfile(lib.LIB / "scripts" / file, OUT / "scripts" / file)
        assets.append({"id": lib.SCRIPT[key], "type": "script", "name": name, "path": "scripts/" + file})
    scene_id = gid("asset/garden")
    assets.append({"id": scene_id, "type": "scene", "name": "Garden", "path": "scenes/garden.scene.json"})

    T, S = lib.TEX, lib.SCRIPT
    hero_id = gid("entity/hero")

    def e(key, name, components, parent=None):
        out = {"id": gid("entity/" + key), "name": name}
        if parent:
            out["parent"] = gid("entity/" + parent)
        out["components"] = components
        return out

    def sprite(tex, size, source=None, layer=5):
        s = {"texture": T[tex], "size": size, "layer": layer}
        if source:
            s["source"] = source
        return s

    def trigger(size, offset=(0, 0)):
        return {"size": list(size), "offset": list(offset), "isTrigger": True}

    entities = [
        e("camera", "Main Camera", {"Transform": {"position": [160, 90]},
                                    "Camera": {"follow": hero_id, "zoom": 1, "clampToBounds": True}}),
        e("ground", "Ground", {"Transform": {"position": [0, 0]},
                               "Tilemap": {"tileset": T["topdown_tiles"], "tileSize": [16, 16], "tilesetColumns": 8,
                                           "solid": False, "layer": -10, "rows": lib.tile_rows(ground_rows())}}),
        e("walls", "Walls", {"Transform": {"position": [0, 0]},
                             "Tilemap": {"tileset": T["topdown_tiles"], "tileSize": [16, 16], "tilesetColumns": 8,
                                         "solid": True, "layer": 0, "rows": lib.tile_rows(wall_rows())}}),
        e("rules", "Level Rules", {"Scripts": [{"script": S["level_rules"], "properties": {
            "tag": "coin", "label": "GEMS", "message": "ALL GEMS FOUND!", "complete": False}}]}),
        e("sign", "Sign", {"Transform": {"position": centre(4, 12)},
                           "Sprite": sprite("scenery", [16, 16], [16, 16, 16, 16], -1),
                           "BoxCollider": trigger((16, 16)),
                           "Scripts": [{"script": S["sign"], "properties": {
                               "message": "Find the key, open the gate, take the stairs!"}}]}),
        e("key", "Gold Key", {"Transform": {"position": centre(2, 1)},
                              "Sprite": sprite("key", [16, 16]), "BoxCollider": trigger((12, 8)),
                              "Scripts": [{"script": S["key"], "properties": {"keyName": "gold"}}]}),
        e("gate", "Gate", {"Transform": {"position": centre(*GATE)}, "Sprite": sprite("lock_block", [16, 16], layer=2),
                           "BoxCollider": {"size": [16, 16]},
                           "Scripts": [{"script": S["lock_block"], "properties": {"keyName": "gold"}}]}),
        e("stairs", "Exit Stairs", {"Transform": {"position": centre(22, 12)},
                                    "Sprite": sprite("stairs", [16, 16], layer=1), "BoxCollider": trigger((10, 10)),
                                    "Scripts": [{"script": S["goal"],
                                                 "properties": {"message": "YOU ESCAPED THE GARDEN!"}}]}),
        e("gems", "Gems", {}),
    ]
    for i, (cx, cy) in enumerate([(2, 7), (7, 7), (17, 7), (22, 2), (9, 13)], start=1):
        entities.append(e(f"gem/{i}", f"Gem {i}", {
            "Transform": {"position": centre(cx, cy)},
            "Sprite": sprite("gem", [16, 16], [0, 0, 16, 16]),
            "SpriteAnimation": {"frames": [[k * 16, 0, 16, 16] for k in range(4)], "framesPerSecond": 6},
            "BoxCollider": trigger((10, 10)), "Tags": ["coin"],
            "Scripts": [{"script": S["coin"], "properties": {"value": 5}}]}, parent="gems"))
    entities.append(e("hazards", "Hazards", {}))
    for i, (cx, cy) in enumerate([(21, 2), (22, 3), (21, 3)], start=1):
        entities.append(e(f"trap/{i}", "Spike Trap", {
            "Transform": {"position": centre(cx, cy)}, "Sprite": sprite("spike_trap", [16, 16], layer=1),
            "BoxCollider": trigger((12, 12)), "Scripts": [{"script": S["hazard"], "properties": {}}]},
            parent="hazards"))
    entities.append(e("beetle", "Beetle", {
        "Transform": {"position": centre(8, 10)},
        "Sprite": sprite("beetle", [16, 16], [0, 0, 16, 16], 6),
        "SpriteAnimation": {"frames": [[0, 0, 16, 16], [16, 0, 16, 16]], "framesPerSecond": 6},
        "BoxCollider": trigger((10, 10)),
        "Scripts": [{"script": S["patrol"], "properties": {"offset": [0, 48], "period": 3}},
                    {"script": S["hazard"], "properties": {}}]}, parent="hazards"))
    entities.append(e("nature", "Nature", {}))
    for i, (x, y) in enumerate([(296, 72), (248, 40), (232, 208), (312, 216)], start=1):
        entities.append(e(f"tree/{i}", "Tree", {
            "Transform": {"position": [x, y]}, "Sprite": sprite("tree", [32, 32], layer=8),
            "BoxCollider": {"size": [10, 8], "offset": [0, 10]}}, parent="nature"))
    for i, (cx, cy) in enumerate([(5, 10), (20, 9)], start=1):
        entities.append(e(f"rock/{i}", "Rock", {
            "Transform": {"position": centre(cx, cy)}, "Sprite": sprite("rock", [16, 16], layer=4),
            "BoxCollider": {"size": [12, 10], "offset": [0, 2]}}, parent="nature"))
    for i, (cx, cy) in enumerate([(11, 13), (9, 2), (15, 11), (19, 13)], start=1):
        entities.append(e(f"shrub/{i}", "Shrub", {
            "Transform": {"position": centre(cx, cy)}, "Sprite": sprite("shrub", [16, 16], layer=4)},
            parent="nature"))
    entities.append(e("hero", "Hero", {
        "Transform": {"position": centre(2, 12)}, "Sprite": sprite("hero", [16, 16], layer=10),
        "BoxCollider": {"size": [10, 8], "offset": [0, 4]}, "KinematicBody": {}, "TopDownController": {},
        "Player": {}}))
    assert entities[-1]["id"] == hero_id

    scene = {"format": "px.scene", "formatVersion": 3, "name": "Garden",
             "settings": {"backgroundColor": "#3e8e41"}, "entities": entities}
    (OUT / "scenes" / "garden.scene.json").write_text(example.dump_pretty(scene) + "\n")
    project = {
        "format": "px.project", "formatVersion": 2, "id": gid("project"), "name": "Example Top-down",
        "engineVersion": "0.4.0", "startScene": scene_id,
        "settings": {"gameType": "topdown", "tickRate": 60,
                     "display": {"virtualResolution": [320, 180], "scaling": "integer", "windowScale": 3},
                     "scripting": {"apiVersion": 1}},
        "input": {"actions": {"move_left": ["Left", "A"], "move_right": ["Right", "D"],
                              "move_up": ["Up", "W"], "move_down": ["Down", "S"]}},
        "assets": assets,
    }
    (OUT / "project.json").write_text(example.dump_pretty(project) + "\n")
    print(f"Wrote {OUT}")
    build = ROOT / "build"
    tools = list(build.glob("*/apps/headless/px-headless")) + list(build.glob("*/apps/headless/px-headless.exe"))
    tools.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    if tools:
        subprocess.run([str(tools[0]), str(OUT), "--resave"], check=True)
    else:
        print("Build px-headless and run: px-headless examples/topdown --resave")


if __name__ == "__main__":
    main()
