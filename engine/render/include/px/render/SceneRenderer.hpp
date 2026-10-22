#pragma once

#include "px/render/Camera2D.hpp"
#include "px/render/Renderer2D.hpp"
#include "px/render/TextureCache.hpp"
#include "px/scene/Scene.hpp"

namespace px::render {

struct SceneRenderStats {
    int sprites = 0;       // sprites submitted
    int tiles = 0;         // tiles submitted
    int culledSprites = 0; // sprites skipped because they were off-screen
};

// Builds a camera from the scene's first Camera component (or a default
// centred at the origin) for the given viewport size. Camera::position is
// runtime state maintained by runtime::GameSession; for a scene that is not
// being simulated (e.g. a future editor view) supply your own Camera2D.
[[nodiscard]] Camera2D cameraForScene(const scene::Scene& scene, Vec2 viewport);

// Draws tilemaps and sprites ordered by layer (stable within a layer:
// tilemaps first, then sprites in entity creation order), culling anything
// outside the camera view.
SceneRenderStats renderScene(const scene::Scene& scene, const Camera2D& camera, IRenderer2D& renderer,
                             TextureCache& textures);

// Debug overlay: outlines of colliders (solid = green, trigger = yellow) and
// solid tilemap cells (blue) in the camera view.
void renderCollisionOverlay(const scene::Scene& scene, const Camera2D& camera, IRenderer2D& renderer);

} // namespace px::render
