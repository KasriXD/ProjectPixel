# Scripting (plan for Milestone 2)

**Status: planned, not implemented.**

## Decision

Creator scripting uses **Luau** as the language runtime, embedded unmodified
(no fork). Project Pixel owns the **gameplay API** that scripts call: names,
semantics, documentation and versioning are ours. Language and API are
separate concerns: the same API will back visual scripting (Milestone 5).

Verified facts (Luau 0.743, October 2026 source checkout):

- MIT license.
- CMake build; libraries `Luau.Ast`, `Luau.Compiler`, `Luau.VM`
  (C++11), `Luau.CodeGen` (optional native code), `Luau.Analysis` (type
  checking, useful for Studio). CLI and tests can be switched off.
- Sandboxing: `luaL_sandbox` / `luaL_sandboxthread` make globals read-only
  and isolate script environments.
- Execution limits: the `interrupt` callback runs at safepoints (loop back
  edges, calls, returns, GC) and can abort a script that exceeds its budget.
- Memory limits: `lua_newstate` takes a custom allocator, which can refuse
  allocations beyond a per-VM quota.
- Web: a `LUAU_BUILD_WEB` option exists.

## Proposed design

- Module `px::scripting`, depending on `runtime` and `scene`.
- One Luau VM per `GameSession`; scripts compiled to bytecode at load time.
  Clients will eventually receive bytecode only for client scripts.
- `Script` component: `{ "script": "<script asset id>", "properties": {...} }`.
- Lifecycle callbacks, all called during the fixed tick in a defined order:
  `onStart()`, `onTick(dt)`, `onTriggerEnter(other)`, `onCollected(...)`,
  `onDestroy()`.
- Budgets per tick (instruction count via `interrupt`) and per VM (memory via
  allocator). A script that exceeds a budget is stopped and reported, the game
  keeps running.
- Only the gameplay API is exposed: no `io`, `os`, `debug`, `require` of
  arbitrary files, `loadstring` of untrusted strings, or native modules.
- Server-only APIs (persistent data, purchases, kicking players) exist only in
  server VMs; client VMs do not have them at all (not just permission checks).
- Errors report script asset name, line and a stack trace, in the log and as
  an on-screen notice in development builds.

## API versioning

- The gameplay API carries a version (`api: 1` in the project manifest).
- Additive changes keep the version. Breaking changes add a new version and
  keep the old one working until projects are migrated.
- Deprecations are reported as warnings in Studio first.
