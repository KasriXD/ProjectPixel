# Contributing

## Workflow

Work in short, verifiable steps:

1. Read the code you are about to change, and the relevant ADRs.
2. Make the smallest change that delivers something useful.
3. Build with warnings as errors: `cmake --preset dev && cmake --build --preset dev`.
4. Run the tests: `ctest --preset dev`. Add or update tests for what you changed.
5. For engine changes affecting memory or lifetimes, also run the `asan` preset.
6. Update documentation in the same change (README status, ROADMAP checkboxes,
   ARCHITECTURE if boundaries move, `docs/` for formats and APIs).
7. Never describe something as working unless it was built and tested; say
   "untested" otherwise.

Significant decisions (new dependency, new module, format change, changing a
boundary) get an Architecture Decision Record in `docs/adr/` (copy
`docs/adr/0000-template.md`).

## Code style

- C++20. Formatting follows `.clang-format` (4 spaces, 120 columns).
- Names: `PascalCase` types, `camelCase` functions and variables, `m_` prefix
  for private members, `kPascalCase` constants, `snake_case` only for file
  formats and action names.
- One module = one folder in `engine/`, one static library `px_<module>`,
  public headers in `include/px/<module>/`, everything in `namespace px::<module>`
  (core types live directly in `px`).
- A module may only include headers of modules it links. Third-party headers
  (SDL, nlohmann/json, stb) stay inside one module's `.cpp` files.
- Return `Result<T>` / `Status` across module boundaries. Do not let exceptions
  cross them. Use `makeError(code, "...", args)` with messages that tell a
  creator what is wrong and where.
- No global mutable state except logging. No singletons for engine systems.
- Components are plain data structs. Behaviour belongs in systems.
- `scene.get<T>(e)` when the component must exist (a missing one terminates
  with a message), `scene.tryGet<T>(e)` with a null check when it may be
  missing. Never dereference `tryGet` unchecked.
- Prefer clear code over clever code; comment *why*, not *what*.

## Adding a built-in component

Components are described once, in a descriptor table
([ADR 0014](docs/adr/0014-component-descriptors.md)); the file format,
validation, Studio's Properties panel and the reference docs follow from it.

1. Define the struct in `engine/scene/include/px/scene/Components.hpp`.
2. Add its descriptor in `engine/scene/src/ComponentInfo.cpp`: name,
   category (Studio's Add Component menu), description, and one `field<>`
   per saved field with a description, range, step, and flags
   (`kFieldRuntime` for simulation state that is not saved). Declare
   required components only if the scene is invalid without them.
3. If a field needs a new `FieldKind`, add reading/writing in
   `engine/project/src/SceneFormat.cpp`, validation in `ComponentInfo.cpp`
   and a widget in `apps/studio/InspectorPanel.cpp`.
4. Regenerate the reference: `px-headless --print-component-reference >
   docs/components.md` (a test fails when it is out of date).
5. Add tests for parsing, validation errors and round-tripping; add the
   component to the editor's presets if creators will use it often.
6. Changing the meaning of an existing field, or adding a requirement,
   needs a scene format version bump and a migration
   (see `docs/project-format.md#versioning`).

## Changing Studio

Editing logic goes into `engine/editor` (no UI, unit tested in
`tests/test_editor.cpp`); `apps/studio` only draws and calls it. Every
change to a scene goes through a `SceneDocument` transaction
(`beginEdit`/`markChanged`/`endEdit`, or one of its operations), which is
what makes it undoable ([ADR 0015](docs/adr/0015-studio-architecture.md)).
Try changes in Studio itself; `px-studio --smoke-test` must keep passing.

## Starter library

`library/` (Studio's Toolbox, [format](docs/library-format.md)) and
`examples/` are generated; edit the generators in `tools/`, not the files:

```sh
pip install pillow                          # once
python3 tools/generate_example_project.py   # examples/platformer (art + level)
python3 tools/generate_library.py           # library/ (copies the platformer art and scripts)
python3 tools/generate_topdown_example.py   # examples/topdown (uses library/)
```

Run them in that order and commit the output. The example generators then
rewrite their project through `px-headless --resave` when a build exists, so
files are stored exactly as Studio saves them. To add a Toolbox item, draw
its art in `tools/library_art.py` and list it in `LIBRARY_TEXTURES` (or reuse
an image), write its behaviour in `library/scripts/` if it needs one
(hand-written, short, commented) and list it in `LIBRARY_SCRIPTS`, then add
the object with `add(...)` in `generate_library.py`, giving the game types it
fits. Ids are derived from
names (`uuid5`), so regenerating never changes them. The editor tests insert
every object into empty projects of both kinds and play them, so a broken
item fails `unit_tests`. A new kind of game needs a controller component, a
`GameType` value, default input in `engine/editor`, a player object and an
example.

## Adding to the scripting API

Scripts are untrusted and the API is versioned, so API changes get extra care:

1. Add one entry to `kBindings` in `engine/scripting/src/Api.cpp`: group,
   name, kind, availability (`Everywhere` or `AuthorityOnly`), a typed
   signature, a one-sentence description, and `pure` if it has no side
   effects. That entry registers the function and documents it.
2. Validate every argument (`luaL_check*`, finite numbers, string lengths)
   and raise errors with `luaL_errorL` that tell a creator what went wrong.
3. Anything that changes game state goes through `IScriptHost`; make it
   `AuthorityOnly` unless clients may legitimately do it.
4. Values crossing the API are nil, number, boolean, string, vector or
   Entity. Do not expose engine pointers, file paths or unbounded containers.
5. Regenerate the reference and commit it:
   `px-headless --print-api-reference > docs/scripting-api.md`
   (the test "API reference document is up to date" enforces this).
6. Add tests in `tests/test_scripting.cpp`, including misuse (wrong types,
   destroyed entities, client realm).
7. Additive changes keep API version 1. Renames, removals and semantic
   changes need a new API version (docs/scripting.md#api-versioning).

## Adding a dependency

Evaluate and record in `docs/dependencies.md` (and an ADR if significant):
purpose, license, maintenance status, platform support (desktop, mobile,
browser), build impact, and why existing code or dependencies do not suffice.
Pin an exact version and SHA-256 in `cmake/PxDependencies.cmake`, or vendor a
single header in `third_party/` and record it in `third_party/VERSIONS.md`.

## Tests

- Unit tests live in `tests/test_<module>.cpp` (doctest).
- `tests/test_scripting.cpp` covers the sandbox, limits, lifecycle, API
  and realms. Scripts used by tests are embedded as `R"lua( ... )lua"` strings.
- `example_platformer_playthrough` replays `examples/platformer/tests/full_run.input`
  and requires the level to be completed (by its Luau scripts) with no
  script errors. If you change the level, scripts or movement tuning,
  re-tune the route (see comments in the file). `example_topdown_playthrough`
  does the same for `examples/topdown` (key, gate, stairs).
- `player_smoke_headless_display` runs the real client for 120 frames with
  SDL's dummy video driver and software renderer, no display needed.
- `studio_smoke_headless_display` runs Studio the same way through a scripted
  session (create, insert Toolbox objects, duplicate, paste, undo everything,
  play, stop) on the example project, without writing to it.
- `tests/test_editor.cpp` covers the editor model, including building and
  playing a game from an empty project without any UI.
- The experimental WebAssembly CI job builds with Emscripten, runs the unit
  tests and playthrough under Node.js, and plays the browser build in
  headless Chromium.

## Commit messages

Imperative summary line under 72 characters, blank line, then what and why.
