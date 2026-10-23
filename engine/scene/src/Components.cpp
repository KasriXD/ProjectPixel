#include "px/scene/Components.hpp"

#include <cmath>
#include <numbers>

namespace px::scene {

namespace {

// Rotation by `degrees` clockwise in Y-down coordinates. Exactly zero
// rotation skips the trigonometry so unrotated hierarchies stay exact (and
// therefore identical on every platform).
Vec2 rotate(Vec2 v, float degrees) {
    if (degrees == 0.0f) return v;
    const double radians = static_cast<double>(degrees) * std::numbers::pi / 180.0;
    const auto c = static_cast<float>(std::cos(radians));
    const auto s = static_cast<float>(std::sin(radians));
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}

float safeDivide(float value, float by) { return by == 0.0f ? 0.0f : value / by; }

} // namespace

Vec2 transformPoint(const Transform& space, Vec2 local) {
    return space.position + rotate(mul(space.scale, local), space.rotation);
}

Vec2 inverseTransformPoint(const Transform& space, Vec2 point) {
    const Vec2 unrotated = rotate(point - space.position, -space.rotation);
    return {safeDivide(unrotated.x, space.scale.x), safeDivide(unrotated.y, space.scale.y)};
}

Transform compose(const Transform& parent, const Transform& local) {
    Transform world;
    world.position = transformPoint(parent, local.position);
    world.rotation = parent.rotation + local.rotation;
    world.scale = mul(parent.scale, local.scale);
    return world;
}

} // namespace px::scene
