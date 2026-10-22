#pragma once

#include "px/core/Math.hpp"

namespace px::render {

// Maps world coordinates to screen (virtual-resolution) pixels.
// Screen = (world - viewOrigin) * zoom, where viewOrigin is the world point
// at the top-left of the screen.
struct Camera2D {
    Vec2 center{};                // world point at the centre of the screen
    float zoom = 1.0f;            // screen pixels per world unit
    Vec2 viewport{320.0f, 180.0f}; // screen size in virtual pixels
    bool pixelSnap = true;        // align the view to whole screen pixels (crisp pixel art)

    [[nodiscard]] Vec2 viewOrigin() const;
    [[nodiscard]] Vec2 worldToScreen(Vec2 world) const;
    [[nodiscard]] Vec2 screenToWorld(Vec2 screen) const;
    [[nodiscard]] Rect worldRectToScreen(const Rect& world) const;
    [[nodiscard]] Rect visibleWorldRect() const;
};

} // namespace px::render
