# ADR 0001: C++20, CMake, and brand-neutral naming

- Status: Accepted
- Date: 2026-10-22

## Context
The engine must run on desktop, mobile and in browsers, embed a scripting VM,
and be maintainable for years. "Project Pixel" is a temporary product name.

## Decision
- **C++20** for the engine and tools: mature compilers on every target
  (MSVC, Clang, GCC, Emscripten, Android NDK, Xcode), direct use of C
  libraries (SDL, Luau, Box2D), predictable performance.
- **CMake ≥ 3.24** with presets; Ninja preferred, Visual Studio supported.
- **Naming:** code uses the neutral prefix `px` (namespaces `px::`, libraries
  `px_<module>`, executables `px-player`, `px-headless`, file format tags
  `px.project`/`px.scene`). The user-facing name exists only in the CMake
  cache variable `PX_PRODUCT_NAME`, exposed to code as
  `px::version::productName`, plus documentation.

## Alternatives considered
- **Rust:** strong safety, but weaker console/mobile/web game tooling for our
  needs and more friction embedding C++ libraries such as Luau.
- **C++17:** loses `std::format`, concepts, `<=>`, `std::span`.
- **C++23:** `std::expected` would be welcome, but support is uneven across
  Apple Clang, MSVC and the Android NDK today.
- **Naming code after the product:** a rename would touch every file.

## Consequences
- A product rename = change `PX_PRODUCT_NAME` and the docs; file format tags
  can stay `px.*` (they are internal identifiers).
- macOS needs deployment target 13.3+ for floating-point `std::format`/`to_chars`.
- We maintain a small `Result<T>` instead of `std::expected`.
