# ADR 0026: Android and iOS apps from the same player

- Status: Accepted
- Date: 2026-11-04

## Context

Milestone 7 asks for Android and iOS apps built in CI. SDL3 runs on both;
`px-player` already uses SDL's main callbacks, which mobile systems need.
What differs is packaging: Android apps are Gradle projects with a Java
activity and native libraries, iOS apps are bundles built by Xcode, and on
Android the app's files are inside the APK, not on a file system.

## Decision

1. **One player**: the same `px_player` target. On Android it is a shared
   library `libmain.so` with SDL linked in statically, loaded by a small
   `SDLActivity` subclass (`platforms/android`); on iOS an app bundle built
   with CMake's Xcode generator.
2. **SDL's Java side** is not copied into the repository: the Gradle build
   fetches it from the same SDL release as the native build (one version in
   `cmake/PxSdlVersion.cmake`), because SDL refuses to start when they
   differ.
3. **Games as packages**: the app plays `game.pxpkg` from its assets (Android)
   or bundle (iOS), read through `SDL_LoadFile`, which reaches both. The
   package is made with `px-headless <project> --pack` on the build machine.
4. **Input**: touch controls generated from the game's actions and shown
   from the start on phones; gamepads work as on desktop
   ([input-devices.md](../input-devices.md)).
5. **CI** builds a release APK (arm64-v8a and x86_64, signed with the
   development key), starts it on an emulated phone, and builds an iOS
   simulator app, which it runs on a simulated iPhone. Device testing,
   store signing and publishing are left for later.
6. **Orientation**: the window is fixed and full screen on phones, so the
   app keeps the orientation it declares (landscape); a resizable window
   made SDL request a new orientation at run time, and Android recreated
   the activity, which ends an SDL game.

## Consequences

- One codebase for all clients; mobile-only behaviour stays in a few
  `#if` blocks (log output, the bundled package).
- The apps play one bundled game. Browsing and joining platform games from
  the apps needs a launcher screen (planned) and the platform client on
  mobile (HTTP and TLS are portable; not enabled in the apps yet).
- Without devices in the loop, performance, safe areas (notches),
  lifecycle edge cases and store rules are unverified.
