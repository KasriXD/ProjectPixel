# Platform support

SDL3 runs on many platforms. That does **not** mean every engine feature works
on all of them: each platform needs its own build, packaging, input and
testing. This page states what is verified and what work remains.

## Status

| Platform | Builds | Runs | Verified how |
|---|---|---|---|
| Linux x64 (X11) | Yes: GCC 13, Clang 18 | Yes: SDL `opengl` renderer on Mesa | Built and tested locally (Ubuntu 24.04), screenshots inspected |
| No display (any OS) | Yes | Yes: SDL `dummy` driver + `software` renderer | CTest `player_smoke_headless_display` |
| Linux (Wayland) | Compiled in | **Not tested** | |
| Headless (no SDL) | Yes | Yes | `headless` preset, `px-headless` playthrough test |
| Windows x64 (MinGW-w64 GCC 13) | Yes, `-O3 -Werror` | Under Wine 9: all unit tests, scripted playthrough, and the client (SDL `windows` driver, `direct3d` renderer) | Cross-compiled on Linux; screenshots inspected. Not yet run on real Windows |
| Windows x64 (MSVC 2022) | Yes | Unit tests, playthrough and windowless client test pass | GitHub Actions `windows-2022`. Not yet run interactively on real hardware |
| macOS arm64 (Apple Clang) | Yes | Unit tests, playthrough and windowless client test pass | GitHub Actions `macos-15`. Not yet run in a real window; needs macOS 13.3+ |
| Android (arm64-v8a, x86_64) | Yes: APK from Gradle + NDK, with the launcher and a bundled game | On an emulated phone (Android 14, x86_64) in CI: opens the launcher (three starts), starts the bundled game from it (SDL's `android` driver and `opengles2` renderer, landscape), the back button opens the game's menu, a game returns to the launcher; **not tested on a device** | GitHub Actions *Android app*, *Android app (emulator)* ([below](#android)) |
| iOS (simulator, arm64) | Yes: app bundle from Xcode, with the launcher | On a simulated iPhone 16 Pro in CI: plays the bundled game 900 frames (SDL's `uikit` driver and `metal` renderer), opens the launcher, starts the game from it and gets it back; **not tested on a device** | GitHub Actions *iOS app (simulator)* ([below](#ios)) |
| WebAssembly under Node.js (engine, Luau, tests) | Yes: Emscripten 6.0.13 | All unit tests and the scripted playthrough pass | Experimental CI job |
| Browser (WebAssembly + WebGL) | Yes: `px-player.html` | Yes in headless Chromium (WebGL 2.0): scripted level completed | Experimental CI job; not yet played by a person |

Multiplayer (`px-server`, `px-player --connect/--host`, UDP via ENet):

| Platform | Builds | Runs | Verified how |
|---|---|---|---|
| Linux x64 | Yes | Server and two or three player windows as separate processes over UDP on one computer; Studio's *Test Multiplayer* | Used interactively on Xvfb; CTest `net_simulated_playthrough`, `net_udp_localhost`, `server_starts_and_stops` (also under ASan) |
| Headless (no SDL) | Yes (`px-server`) | Network tests pass | `headless` preset in CI |
| Windows x64 (MinGW-w64) | Yes, `-O3 -Werror` | Under Wine 9: network tests pass; joined a Linux `px-server` as a bot (level completed, no prediction corrections) and as a game window next to a Linux window | Cross-compiled on Linux |
| Windows x64 (MSVC 2022) | Yes | Network tests pass, including real UDP on localhost | GitHub Actions. Not yet played across computers |
| macOS arm64 | Yes | Network tests pass, including real UDP on localhost | GitHub Actions |
| Browser | Yes: WebSockets (`ws://`, `wss://`) | Joins platform game servers from the `/play` page and plays with desktop players | CI *Browser cross-play*: headless Chromium and a desktop bot in one game server, over HTTP and HTTPS ([ADR 0024](adr/0024-websockets-for-browsers.md)) |
| Android, iOS | Yes (UDP) | Not tested | The apps have no screen to choose a server yet |

Not verified yet: play between different computers (home network,
internet, Wi-Fi), firewalls and NAT.

Studio (`px-studio`) is desktop-only:

| Platform | Builds | Runs | Verified how |
|---|---|---|---|
| Linux x64 (X11) | Yes: GCC 13, Clang 18, ASan+UBSan | Yes: SDL `opengl` renderer | Used interactively on Xvfb (a game built from an empty project), smoke test in CTest |
| No display | Yes | Yes: `dummy` driver + `software` renderer | CTest `studio_smoke_headless_display` (also under ASan) |
| Windows x64 (MinGW-w64 GCC 13) | Yes, `-O3 -Werror` | Under Wine 9: smoke test passes in a real window with the `direct3d` renderer | Not yet run on real Windows |
| Windows x64 (MSVC, Build Tools 2026) | Yes | Yes: SDL `direct3d11` renderer | Used by the founder on a Windows 11 PC: a level played to the end in Studio; all 4 tests pass. (The crash on the Stop button found there is fixed.) |
| Windows x64 (MSVC 2022) | Yes | Smoke test passes (dummy driver) | GitHub Actions |
| macOS arm64 | Yes | Smoke test passes (dummy driver) | GitHub Actions; not yet used in a real window |
| Browser, mobile | Not built | | Studio needs a desktop; a browser-based editor would be a separate decision |

Desktop risks not covered by these checks yet: high-DPI scaling on real
monitors other than the one Windows PC, native file dialogs (they need the XDG desktop portal or zenity on
Linux; Studio falls back to typed paths), keyboard layouts and IME text input.

## Desktop

Expected to work on Windows and macOS with the same code: SDL3 picks
Direct3D 11/12 or Metal for `SDL_Renderer`. Known requirements:

- Windows: Visual Studio 2022 17.8+ (C++20 `<format>`), CMake ≥ 3.24.
- macOS: Xcode 15.3+, deployment target 13.3 (Apple's libc++ gates
  floating-point `std::to_chars`/`std::format` on it).

## Mobile

**Status: built in CI; not tested on devices.** Decision: [ADR 0026](adr/0026-mobile-apps.md).
The apps are px-player: they open the launcher (log in to a platform,
browse its games, friends and leaderboards, play and join friends;
[launcher.md](launcher.md)), and the game that comes with them
(`game.pxpkg`) plays from its *On this device* list without a platform.
Touch controls are generated from the game's actions, and gamepads work
([input-devices.md](input-devices.md)). In a game, the back button
(Android) opens the menu; leaving the app pauses the game.

### Android

`platforms/android` is a Gradle project around the CMake build: px-player
becomes `libmain.so` with SDL linked in, SDL's Java side is downloaded from
the same SDL release, and the game goes into the APK's assets.

```sh
px-headless examples/platformer --pack game.pxpkg
cd platforms/android
gradle assembleRelease -PpxGamePackage=$PWD/../../game.pxpkg \
    -PpxNdkVersion=27.2.12479018 -PpxCmakeVersion=3.31.6   # the NDK and CMake you have in the Android SDK
adb install app/build/outputs/apk/release/app-release.apk
```

Needs JDK 17, Gradle 8.12 (or later), the Android SDK with an NDK (r26 or
later, for C++20 `<format>`) and a CMake 3.24+ package. The release APK is
signed with the development key; publishing needs your own key. The APK
built by CI (arm64-v8a and x86_64) is attached to each run as
`android-apk`; CI then starts it on an emulated phone
(`tools/android/emulator_check.sh`, screenshots in `android-emulator-run`).
Logs: `adb logcat -s SDL SDL/APP`.

### iOS

```sh
px-headless examples/platformer --pack game.pxpkg
cmake -S . -B build/ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DPX_BUILD_STUDIO=OFF -DPX_BUILD_TESTS=OFF \
    -DPX_GAME_PACKAGE=$PWD/game.pxpkg
cmake --build build/ios --config Release --target px_player -- -sdk iphonesimulator CODE_SIGNING_ALLOWED=NO
xcrun simctl install booted build/ios/apps/player/Release-iphonesimulator/px-player.app
xcrun simctl launch --console-pty booted dev.px.player
```

Needs Xcode 15.3+ (iOS 16.3 or later on the device, for C++20 `<format>`).
For a device, open `build/ios/px.xcodeproj` in Xcode, choose your team for
signing and build `px_player` for the device. CI builds the simulator app,
runs it on a simulated iPhone: the game on its own, the launcher, and the
launcher starting the game and getting it back (screenshots and logs
attached as `ios-simulator-app`). To log in to HTTPS platforms from iOS, add
`-DPX_CA_BUNDLE=/path/to/roots.pem` ([launcher.md](launcher.md#phones-and-tablets)).

### Not done yet on mobile

- Testing on real phones and tablets: performance, touch feel, safe areas
  (the on-screen controls ignore notches today), losing the GPU context,
  sound (stopped in the background; on iOS it follows the silent switch,
  [audio.md](audio.md#browsers-and-phones)).
- Using the launcher on real phones (it runs on the emulator and simulator
  in CI); iOS needs a certificate file for HTTPS platforms.
- Store builds: signing keys, app icons for every size, store rules for
  user-generated content.

## Browser

**Status: plays platform games with desktop players, verified in CI; not
yet played by a person.** Decisions: [ADR 0012](adr/0012-browser-runtime.md),
[ADR 0024](adr/0024-websockets-for-browsers.md). The platform serves the
browser player at `/play` ([platform.md](platform.md#browsers)).

### What was verified

- Luau compiles to WebAssembly unmodified. Its error handling (C++
  exceptions), CPU interrupt, memory cap and read-only sandbox were checked
  in WebAssembly under Node.js.
- With Emscripten 6.0.13, the whole engine compiles; all unit tests and the
  scripted playthrough pass under Node.js, and the playthrough ends at the
  same player position as the native Linux and Windows builds.
- SDL3 + `px-player` build as `px-player.html` with the example project
  preloaded: `px-player.wasm` 2.5 MB, `.js` 177 KB, `.data` 30 KB
  (uncompressed).
- Emscripten 3.1.6 (Ubuntu 24.04's package) is **too old**: its C++ library
  lacks `std::format`.

### Browser run

Verified in CI (run of 2026-10-23, Playwright 1.49.1, headless Chromium):
the browser build loads `px-player.html`, reports **WebGL 2.0**, opens with
SDL's `emscripten` video driver and `opengles2` renderer, loads the example
project and its 3 Luau scripts from the preloaded data, plays the scripted
route and completes the level with all 14 coins and 0 script errors. The
in-game completion time (14.83 s of simulated time) is identical to the
native builds. A screenshot taken at the win banner is attached to the CI
run as the `px-player-web` artifact, together with the build.

Not verified yet: a person playing it in a desktop or mobile browser
(keyboard feel, focus handling, mobile browsers, Safari and Firefox).

### Building it yourself

```sh
# Emscripten SDK 6.0.13 (https://emscripten.org/docs/getting_started/downloads.html)
source /path/to/emsdk/emsdk_env.sh
cmake --preset wasm && cmake --build --preset wasm
cd build/wasm/apps/player && python3 -m http.server 8000
# open http://localhost:8000/px-player.html
```

`cmake --preset wasm-headless` builds the engine and tests for Node.js
(`ctest --preset wasm-headless`). URL parameters become command-line
arguments: `px-player.html?arg=--debug` (not on the platform's page).
To play platform games, give the folder to the platform:
`px-platform --web-player build/wasm/apps/player`.

### Cross-play

CI job *Browser cross-play* (`tools/browser/crossplay.sh`): a platform with
the browser player, the top-down example published, headless Chromium logs
in on `/play`, starts the game and plays its route over WebSockets while a
desktop bot joins the same game server over UDP. Both must see two players
and the level completed; then the same over HTTPS with `wss://`.

### Remaining work for the browser

- Download size: compression (the platform serves files uncompressed),
  `-Oz`, excluding unused SDL subsystems.
- Graphics: SDL_Renderer's WebGL path today; WebGL2/WebGPU when a custom
  GPU renderer exists.
- WebTransport (unreliable datagrams) instead of WebSockets, for lossy
  networks.
- Threads only with cross-origin isolation (`SharedArrayBuffer`); the
  engine is single-threaded today.
- Settings last only until the tab closes. Sound starts at the player's
  first click, tap or key press (browsers' rule; SDL resumes the audio
  context then), so the first moments of a game are silent. Not yet
  heard by a person in a browser.
