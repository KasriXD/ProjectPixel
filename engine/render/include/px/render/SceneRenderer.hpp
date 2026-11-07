#pragma once

#include "px/render/Camera2D.hpp"
#include "px/render/Renderer2D.hpp"
#include "px/render/Text.hpp"
#include "px/render/TextureCache.hpp"
#include "px/scene/Scene.hpp"

namespace px::render {

struct SceneRenderStats {
    int sprites = 0;       // sprites submitted
    int tiles = 0;         // tiles submitted
    int culledSprites = 0; // sprites skipped because they were off-screen
    int texts = 0;         // Text components drawn
};

struct SceneRenderOptions {
    FontCache* fonts = nullptr; // nullptr: Text components are not drawn
    bool screenTexts = true;    // the HUD (Text with `screen`); off for an editor's world view
    // The player the scene is shown to: screen texts inside another player's
    // character are not drawn (0: draw every screen text).
    int viewer = 0;
    // Where the game's screen is (an editor shows the HUD inside the game
    // camera's frame): its top-left and screen pixels per game pixel. A zero
    // size means the whole viewport at scale 1.
    Rect screenArea{};
    float screenScale = 1.0f;
};

// Builds a camera from the scene's first Camera component (or a default
// centred at the origin) for the given viewport size. Camera::position is
// runtime state maintained by runtime::GameSession; for a scene that is not
// being simulated (e.g. a future editor view) supply your own Camera2D.
[[nodiscard]] Camera2D cameraForScene(const scene::Scene& scene, Vec2 viewport);

// Draws tilemaps, sprites and world texts ordered by layer (stable within a
// layer: tilemaps, then sprites and texts in hierarchy order), culling
// anything outside the camera view; then the screen texts by layer.
SceneRenderStats renderScene(const scene::Scene& scene, const Camera2D& camera, IRenderer2D& renderer,
                             TextureCache& textures, const SceneRenderOptions& options = {});

// Where a Text is drawn: the top-left of its box on the screen and screen
// pixels per font pixel. World texts sit at the entity with their anchor
// point; screen texts are measured from that point of the screen by the
// entity's own Transform position.
struct TextPlacement {
    Vec2 topLeft{};
    float pixelScale = 1.0f;
};
[[nodiscard]] TextPlacement placeText(const scene::Scene& scene, scene::Entity entity, const scene::Text& text,
                                      const TextLayout& layout, const Camera2D& camera,
                                      const SceneRenderOptions& options = {});

// Debug overlay: outlines of colliders (solid = green, trigger = yellow) and
// solid tilemap cells (blue) in the camera view.
void renderCollisionOverlay(const scene::Scene& scene, const Camera2D& camera, IRenderer2D& renderer);

} // namespace px::render
