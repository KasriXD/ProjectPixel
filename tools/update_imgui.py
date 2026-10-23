#!/usr/bin/env python3
"""Vendors Dear ImGui (docking branch) into third_party/imgui/.

Copies the core library, the SDL3 platform and SDL_Renderer backends, the
std::string helpers and the license. Nothing else from the repository is used.

Usage:
    python3 tools/update_imgui.py v1.92.9b-docking

Then: rebuild Studio, run the tests and the Studio smoke test, look at Studio,
update the version in docs/dependencies.md and third_party/VERSIONS.md, and
commit. Read the "Breaking Changes" section of the new docs/CHANGELOG.txt
upstream before updating. Requires git and network access to github.com.
"""

import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO = "https://github.com/ocornut/imgui.git"
ROOT = pathlib.Path(__file__).resolve().parent.parent
DEST = ROOT / "third_party" / "imgui"
FILES = [
    "LICENSE.txt",
    "imconfig.h",
    "imgui.cpp",
    "imgui.h",
    "imgui_demo.cpp",
    "imgui_draw.cpp",
    "imgui_internal.h",
    "imgui_tables.cpp",
    "imgui_widgets.cpp",
    "imstb_rectpack.h",
    "imstb_textedit.h",
    "imstb_truetype.h",
    "backends/imgui_impl_sdl3.cpp",
    "backends/imgui_impl_sdl3.h",
    "backends/imgui_impl_sdlrenderer3.cpp",
    "backends/imgui_impl_sdlrenderer3.h",
    "misc/cpp/imgui_stdlib.cpp",
    "misc/cpp/imgui_stdlib.h",
]


def run(*args, cwd=None):
    return subprocess.run(args, cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    tag = sys.argv[1]
    if not tag.endswith("-docking"):
        sys.exit("Use a docking-branch tag, e.g. v1.92.9b-docking (Studio uses docking).")
    with tempfile.TemporaryDirectory() as tmp:
        checkout = pathlib.Path(tmp) / "imgui"
        run("git", "clone", "--quiet", "--depth", "1", "--branch", tag, REPO, str(checkout))
        commit = run("git", "rev-parse", "HEAD", cwd=checkout)
        if DEST.exists():
            for child in DEST.iterdir():
                if child.name != "CMakeLists.txt":  # our build file is maintained by hand
                    shutil.rmtree(child) if child.is_dir() else child.unlink()
        for name in FILES:
            target = DEST / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(checkout / name, target)
    (DEST / "UPSTREAM_VERSION.txt").write_text(f"{tag}\n{commit}\n")
    print(f"Vendored Dear ImGui {tag} ({commit}) into {DEST}")


if __name__ == "__main__":
    main()
