# ADR 0008: Luau for creator scripting

- Status: Accepted, implemented in Milestone 2 (see ADRs 0009-0011 for the details)
- Date: 2026-10-22, updated 2026-10-23

## Context
Creators need a scripting language that is approachable, fast, safe to run
untrusted, and embeddable on desktop, mobile and web. Visual scripting must
share the same capabilities.

## Decision
Embed **Luau** unmodified as the language runtime (version 0.743, vendored;
ADR 0009). Project Pixel defines and versions its own gameplay API on top
(`docs/scripting.md`, `docs/scripting-api.md`).

Verified from the Luau source and by our tests:
- MIT license; CMake; C++17 libraries (VM C++11).
- Sandboxing (`luaL_sandbox`, `luaL_sandboxthread`), execution limits via the
  `interrupt` callback at safepoints, memory limits via custom allocators.
- Native code generation is optional and is not built (no JIT on iOS or in
  browsers).
- Compiles and runs in WebAssembly, including errors, interrupts and the
  memory cap (ADR 0012).

## Alternatives considered
- **Lua 5.4 / LuaJIT:** no gradual typing; LuaJIT is not maintained for all
  our targets and its JIT is unusable on iOS.
- **JavaScript (QuickJS) or Python:** larger runtimes, weaker sandboxing
  story for untrusted code, slower.
- **WebAssembly for creator code:** strong isolation, but a poor first
  language for beginners; may be reconsidered for advanced native modules.
- **A new language:** large cost, no demonstrated need.

## Consequences
- Creators familiar with Roblox scripting get a familiar language, while the
  API is our own.
- Luau's type checker (`Luau.Analysis`) can power Studio autocompletion; it
  is not vendored yet.
- We must keep the API surface small and versioned, and keep authority-only
  APIs out of client VMs entirely (ADR 0011).
