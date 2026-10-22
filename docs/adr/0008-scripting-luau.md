# ADR 0008: Luau for creator scripting

- Status: Accepted (implementation scheduled for Milestone 2)
- Date: 2026-10-22

## Context
Creators need a scripting language that is approachable, fast, safe to run
untrusted, and embeddable on desktop, mobile and web. Visual scripting must
share the same capabilities.

## Decision
Embed **Luau** (0.743 at evaluation time) unmodified as the language runtime.
Project Pixel defines and versions its own gameplay API on top. Details:
`docs/scripting.md`.

Verified from the Luau source:
- MIT license; CMake; C++17 libraries (VM C++11).
- Sandboxing (`luaL_sandbox`, `luaL_sandboxthread`), execution limits via the
  `interrupt` callback at safepoints, memory limits via custom allocators.
- Optional native code generation, which can be left out where JIT is not
  permitted (iOS) or not available (browser).

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
- Luau's type checker (`Luau.Analysis`) can power Studio autocompletion.
- We must keep the API surface small and versioned, and keep server-only
  APIs out of client VMs entirely.
