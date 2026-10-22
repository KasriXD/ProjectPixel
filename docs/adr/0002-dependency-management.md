# ADR 0002: Dependency management

- Status: Accepted
- Date: 2026-10-22

## Context
We need reproducible builds on Windows, macOS, Linux, and later Android, iOS
and Emscripten, with few dependencies, each evaluated before adoption.

## Decision
- Larger libraries (SDL3, nlohmann/json) use **CMake FetchContent** with an
  exact release URL and **SHA-256 hash**, and `FIND_PACKAGE_ARGS`, so an
  installed compatible version (system package, vcpkg, Homebrew) is used when
  present and the pinned source is built otherwise.
- Small single-header libraries (stb_image, doctest) are **vendored** in
  `third_party/` with versions recorded in `third_party/VERSIONS.md`.
- Exception: a minimal subset of Luau is vendored with an update script,
  because its upstream build defines many unused targets (ADR 0009).
- SDL3 is linked statically.
- Every dependency is documented in `docs/dependencies.md`.

## Alternatives considered
- **vcpkg or Conan from the start:** better binary caching and many packages,
  but an extra tool for every contributor and more moving parts for
  Emscripten/Android before we need them. Revisit when dependencies grow
  (Luau, ImGui, a networking library, an audio decoder) or CI build times
  become a problem; FetchContent's `FIND_PACKAGE_ARGS` makes the switch easy.
- **Git submodules:** pin well but are easy to forget to update/initialise
  and do not verify archive integrity.
- **Vendoring everything:** bloats the repository with SDL's sources.

## Consequences
- The first configure needs internet access to github.com.
- Linux contributors install SDL's X11/Wayland development headers
  (README lists them).
- Updating a dependency = change URL + hash, rebuild, run all tests.
