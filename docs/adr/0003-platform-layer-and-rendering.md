# ADR 0003: SDL3 platform layer and the renderer abstraction

- Status: Accepted
- Date: 2026-10-22

## Context
We need windows, input and 2D rendering on desktop now, and on mobile and in
browsers later, without spreading platform or graphics API code through the
engine. Rendering must handle pixel art without blur, high-resolution art,
layers, tilemaps, and later lighting and shaders.

## Decision
- **SDL3 (3.4.x)** for windows, events, input and the first rendering backend.
  All SDL code lives in the `platform_sdl` module.
- Applications use **SDL main callbacks** (`SDL_AppInit/Iterate/Event/Quit`)
  instead of a blocking loop, because browsers (Emscripten) and iOS require
  the platform to own the main loop.
- Rendering goes through `render::IRenderer2D`, a small interface taking
  screen-space quads, rectangles and debug text. Scene traversal, culling,
  layer sorting, camera math and pixel snapping are backend-independent code
  in `render` and are unit-tested with a recording renderer.
- The first backend is **`SDL_Renderer`** (`SdlRenderer2D`): SDL selects
  Direct3D, Metal, Vulkan, OpenGL/ES, WebGL or software per platform and
  batches draws. The game draws at a virtual resolution; SDL's logical
  presentation scales it (integer scaling for pixel art, letterbox otherwise).

## Alternatives considered
- **SDL_GPU** (SDL3's modern GPU API): custom shaders and full control, but
  in SDL 3.4.18 its only drivers are Direct3D 12, Metal and Vulkan (no
  browser backend, and no OpenGL ES for older Android devices), and it needs
  more code than the prototype does. Planned as the
  second backend when lighting and shader effects are scheduled; the
  interface will gain material/shader and batching concepts then.
- **Raw OpenGL/GLES/WebGL2:** one API for desktop, mobile and web, but
  deprecated on macOS and more boilerplate.
- **bgfx / sokol_gfx:** capable abstractions, but another dependency and
  shader toolchain before we need custom shaders.
- **GLFW instead of SDL:** desktop only; no mobile, no audio, weaker web story.

## Consequences
- Works on every SDL platform with no shader toolchain today.
- `SDL_Renderer` has no custom shaders, so lighting/post-processing require
  the planned GPU backend. `IRenderer2D` will need to grow (view transforms,
  materials, render targets); this is expected and localized.
- Text is SDL's 8x8 debug font for now; a font/text system is still needed.
