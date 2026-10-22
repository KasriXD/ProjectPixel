# ADR 0009: Vendor a minimal Luau subset

- Status: Accepted
- Date: 2026-10-23

## Context
ADR 0002 prefers fetching larger dependencies by URL and hash. Luau's own
CMake project, however, defines every library at the top level, including
the 3.5 MB type checker (Analysis), native code generator (CodeGen), REPL
and tests, and they all build by default. Its source archive comes from
codeload.github.com, which some build environments (including the one this
project was developed in) cannot reach, and the browser build needs the same
sources with different flags.

## Decision
- Vendor only what the engine uses: `Common`, `Ast`, `Bytecode`, `Compiler`
  and `VM` (about 2.7 MB) into `third_party/luau/`, with `LICENSE.txt` and
  `lua_LICENSE.txt`.
- `tools/update_luau.py <tag>` performs every update: it clones the tag,
  copies those directories, records the tag and commit in `VERSION`, and
  generates `sources.cmake` from upstream `Sources.cmake`, so the build's
  file lists always match the vendored files.
- `third_party/luau/CMakeLists.txt` is ours: static libraries
  `px_luau_*`, upstream's compile options (`-fno-math-errno`, the MSVC
  `/d2ssa-pre-` workaround), warnings off for third-party code, `-fexceptions`
  under Emscripten.
- Only `px_scripting` links Luau (privately); no Luau header is visible
  outside that module.
- Pinned version: **0.743**, commit `9d7748a4f324213a18cb8283fa2a77c330225fa0`.

## Alternatives considered
- **FetchContent of the release archive:** consistent with ADR 0002, but
  builds unused targets unless patched, and needs codeload access.
- **FetchContent from git by commit:** clones the full history on every
  fresh build.
- **Git submodule:** same unused-target problem, plus submodule friction.

## Consequences
- Builds need no network for Luau and are identical on every platform,
  including Emscripten.
- The repository grows by 2.7 MB per Luau version in history.
- Updating is a scripted, reviewable diff; after updating, run the full test
  suite (the sandbox tests catch behaviour changes such as new globals).
- Luau.Analysis (for editor autocompletion) is not vendored; add it when
  Studio needs it.
