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
- Prefer clear code over clever code; comment *why*, not *what*.

## Adding a built-in component

1. Define the struct in `engine/scene/include/px/scene/Components.hpp`; mark
   runtime-only fields with `// runtime`.
2. Add a read/write codec in `engine/project/src/SceneFormat.cpp` (validate
   ranges, reject unknown fields via `ObjectReader::finish`).
3. Document the JSON in `docs/project-format.md`.
4. Add tests for parsing, validation errors and round-tripping.
5. Changing the meaning of an existing field requires a format version bump
   and a migration (see `docs/project-format.md#versioning`).

## Adding a dependency

Evaluate and record in `docs/dependencies.md` (and an ADR if significant):
purpose, license, maintenance status, platform support (desktop, mobile,
browser), build impact, and why existing code or dependencies do not suffice.
Pin an exact version and SHA-256 in `cmake/PxDependencies.cmake`, or vendor a
single header in `third_party/` and record it in `third_party/VERSIONS.md`.

## Tests

- Unit tests live in `tests/test_<module>.cpp` (doctest).
- `example_platformer_playthrough` replays `examples/platformer/tests/full_run.input`
  and requires every coin to be collected. If you change the level or the
  movement tuning, re-tune the script (see comments in the file).
- `player_smoke_headless_display` runs the real client for 120 frames with
  SDL's dummy video driver and software renderer, no display needed.

## Commit messages

Imperative summary line under 72 characters, blank line, then what and why.
