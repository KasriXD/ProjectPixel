# Project Vision

*Project Pixel is a working title.*

## What we are building

A universal 2D gaming platform where anyone can create, publish, discover and
play games made by other people. It combines:

- the user-generated ecosystem of Roblox: games made by the community, played
  by the community, with shared accounts, avatars, friends and communities;
- the flexibility of a general-purpose 2D engine: no single genre or art style;
- a beginner-friendly but powerful native creation studio;
- hosted multiplayer sessions run by the platform;
- an asset and creator marketplace;
- native PC and mobile clients, plus browser play for compatible, lighter games;
- optional shared social hubs and persistent worlds alongside independent games.

**Central principle:** making a simple game must be approachable, while
experienced developers get the tools to build sophisticated multiplayer games.
Beginners and experts share one coherent environment, not two products.

## Creative range

Games must not be limited to one genre. Targets include platformers and
obstacle courses, top-down RPGs and adventures, survival/crafting/farming,
shooters and action, racing, strategy and puzzle, card and turn-based games,
social hangouts and virtual worlds, and experimental formats.

Art styles include pixel art, hand-drawn and high-resolution 2D, skeletal
animation, particles, lighting and other 2D techniques.

## Product principles

1. It is a platform, not a standalone game.
2. The engine supports diverse 2D genres and art styles.
3. Beginners and advanced developers share one creation environment.
4. Multiplayer is a foundational engine capability, not an add-on.
5. Game servers and platform services have separate responsibilities.
6. PC, mobile and browser are considered in the architecture from the start.
7. Independent games and optional shared worlds coexist.
8. Security, moderation and creator trust are essential.
9. Modular and extensible without becoming needlessly complicated.
10. A small working system beats a large collection of untested abstractions.

## Long-term feature set

**Engine:** sprites, tilemaps, layered scenes, cameras, animation (frame and
skeletal), particles, lighting and shaders, text and UI, audio, collision and
rigid-body physics, input from keyboard, mouse, gamepad and touch.

**Creator scripting:** Luau-based scripting with a Project Pixel-owned,
versioned gameplay API; visual scripting built on the same API; strong
sandboxing and resource limits.

**Studio:** scene/level editing, entity and component inspection, asset
import and browsing, tilemap and animation editors, script and visual-script
editing, debugging and logging, local and multiplayer playtesting, project
settings, build and publishing.

**Multiplayer:** dedicated, server-authoritative game sessions, state
replication, connection handling, local multiplayer testing.

**Platform:** accounts, profiles and avatars, game catalog and discovery,
publishing and versioning, asset storage and distribution, matchmaking,
persistent data, friends and invitations, moderation and administration,
and later a marketplace, analytics and monetization.

The order in which these are built is in [ROADMAP.md](ROADMAP.md).
