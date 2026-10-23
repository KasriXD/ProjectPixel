# ADR 0013: Dear ImGui for Studio's UI, behind an editor model

- Status: Accepted
- Date: 2026-10-23

## Context
Milestone 3 needs a native editor (Studio) with docked panels: hierarchy,
scene viewport, inspector, asset browser, console, script editor and
settings. Requirements: runs on Windows, macOS and Linux; uses the engine's
own renderer for the scene view; fast to build with a small team; and a UI
that can be replaced or re-skinned later without rewriting the engine or
the editing logic. The vision is a polished creator tool, so the first UI
toolkit must not become a trap.

## Decision
- **Dear ImGui 1.92.9b (docking branch)**, vendored with its SDL3 platform
  backend and SDL_Renderer backend (MIT). Studio draws through the same
  `SDL_Renderer` as `px-player`; the scene view renders the real scene into a
  render target (`SdlRenderer2D::createRenderTarget`) shown as an ImGui image.
- **All editing logic lives in `px_editor`**, a UI-free engine module:
  projects, assets, scene documents, undo, selection, picking, clipboard and
  play mode ([ADR 0015](0015-studio-architecture.md)). It is unit tested
  without a window. Studio's panels only draw and call it; they keep
  nothing but UI state (filters, text being typed, which popup is open).
- **The inspector is generated from component descriptors**
  ([ADR 0014](0014-component-descriptors.md)), not written per component, so a
  new UI toolkit needs one generic field editor per field kind, not one form
  per component.
- Studio is a desktop app (`apps/studio`), built when SDL is available and the
  target is not the browser (`PX_BUILD_STUDIO`). Games never link ImGui.

## Alternatives considered
- **Qt (Widgets or QML):** mature, native-looking, excellent text editing and
  docking. But LGPL/commercial licensing, a large dependency (hundreds of MB
  of SDK, its own build tooling), a second event loop and renderer to
  reconcile with SDL, and a much heavier contributor setup. Reconsider if
  Studio needs complex text tooling, accessibility or native dialogs that
  ImGui cannot provide.
- **A web UI (Electron/Tauri/webview) around a native engine view:** the
  most flexible styling and a path to a browser-based Studio, but embedding a
  native GPU view in a webview is fragile across platforms, adds a JS
  toolchain, and doubles the process/IPC complexity for an MVP.
- **Own retained-mode UI on the engine renderer:** full control and a single
  look for games and Studio, but months of work (text input, IME, docking,
  tables, scrolling) before any editor feature.
- **RmlUi / Nuklear / other immediate-mode libraries:** RmlUi (HTML/CSS-like)
  suits game UI more than dense tools; Nuklear lacks docking and the tooling
  ecosystem of ImGui.

## Consequences
- Fast iteration: the whole Studio is about 4,400 lines of UI code on top of
  `px_editor`. Debug builds of ImGui compile in about 20 seconds.
- The look is "developer tool" by default. Styling, fonts and icons can be
  improved within ImGui; a native look cannot.
- Text editing is basic (no syntax highlighting or completion); a code editor
  widget or an external-editor workflow is needed later.
- Accessibility (screen readers) and IME support are weak in ImGui; they must
  be revisited before a broad public release of Studio.
- Synthetic input (tests driving the UI through the OS) is unreliable with
  immediate-mode UIs; automated Studio tests drive the editor model and use a
  scripted smoke run (`--smoke-test`) instead.
- Multi-viewport (panels as separate OS windows) is unavailable with the
  SDL_Renderer backend; it would require the SDL GPU or OpenGL backend.
- Replacing the UI later means rewriting `apps/studio` only; `px_editor`,
  descriptors and file formats stay.
