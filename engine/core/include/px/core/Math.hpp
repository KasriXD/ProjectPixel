#pragma once

#include <algorithm>
#include <cmath>

// Basic 2D math. World space is Y-down (docs/engine/coordinates.md).
namespace px {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}

    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator-() const { return {-x, -y}; }
    constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(float s) const { return {x / s, y / s}; }
    constexpr Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    constexpr Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
    constexpr Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
    constexpr bool operator==(const Vec2&) const = default;
};

constexpr Vec2 operator*(float s, Vec2 v) { return {v.x * s, v.y * s}; }

// Component-wise product.
constexpr Vec2 mul(Vec2 a, Vec2 b) { return {a.x * b.x, a.y * b.y}; }
inline float length(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

struct Vec2i {
    int x = 0;
    int y = 0;
    constexpr bool operator==(const Vec2i&) const = default;
};

// Axis-aligned rectangle: (x, y) is the top-left corner (Y-down).
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    static constexpr Rect fromCenter(Vec2 center, Vec2 size) {
        return {center.x - size.x * 0.5f, center.y - size.y * 0.5f, size.x, size.y};
    }

    constexpr float left() const { return x; }
    constexpr float right() const { return x + w; }
    constexpr float top() const { return y; }
    constexpr float bottom() const { return y + h; }
    constexpr Vec2 position() const { return {x, y}; }
    constexpr Vec2 size() const { return {w, h}; }
    constexpr Vec2 center() const { return {x + w * 0.5f, y + h * 0.5f}; }
    constexpr bool empty() const { return w <= 0.0f || h <= 0.0f; }

    constexpr bool contains(Vec2 p) const { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }

    // Strict overlap: rectangles that merely touch do not intersect.
    constexpr bool intersects(const Rect& o) const {
        return x < o.x + o.w && x + w > o.x && y < o.y + o.h && y + h > o.y;
    }

    constexpr Rect translated(Vec2 d) const { return {x + d.x, y + d.y, w, h}; }
    constexpr bool operator==(const Rect&) const = default;
};

// Smallest rectangle containing both.
constexpr Rect unite(const Rect& a, const Rect& b) {
    const float l = std::min(a.left(), b.left());
    const float t = std::min(a.top(), b.top());
    const float r = std::max(a.right(), b.right());
    const float btm = std::max(a.bottom(), b.bottom());
    return {l, t, r - l, btm - t};
}

constexpr float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Moves current towards target by at most maxDelta (maxDelta >= 0).
constexpr float approach(float current, float target, float maxDelta) {
    if (current < target) return std::min(current + maxDelta, target);
    if (current > target) return std::max(current - maxDelta, target);
    return target;
}

constexpr float signOf(float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }

} // namespace px
