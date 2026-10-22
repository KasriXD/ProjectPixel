#include "px/render/Camera2D.hpp"

#include <cmath>

namespace px::render {

Vec2 Camera2D::viewOrigin() const {
    Vec2 origin = center - viewport * (0.5f / zoom);
    if (pixelSnap) {
        origin.x = std::round(origin.x * zoom) / zoom;
        origin.y = std::round(origin.y * zoom) / zoom;
    }
    return origin;
}

Vec2 Camera2D::worldToScreen(Vec2 world) const { return (world - viewOrigin()) * zoom; }

Vec2 Camera2D::screenToWorld(Vec2 screen) const { return screen / zoom + viewOrigin(); }

Rect Camera2D::worldRectToScreen(const Rect& world) const {
    const Vec2 p = worldToScreen(world.position());
    return {p.x, p.y, world.w * zoom, world.h * zoom};
}

Rect Camera2D::visibleWorldRect() const {
    const Vec2 origin = viewOrigin();
    return {origin.x, origin.y, viewport.x / zoom, viewport.y / zoom};
}

} // namespace px::render
