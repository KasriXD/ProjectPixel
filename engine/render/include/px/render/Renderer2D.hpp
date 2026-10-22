#pragma once

#include "px/assets/AssetRegistry.hpp"
#include "px/assets/Image.hpp"
#include "px/core/Color.hpp"
#include "px/core/Math.hpp"

#include <cstdint>
#include <string_view>

namespace px::render {

using TextureHandle = std::uint32_t;
inline constexpr TextureHandle kNoTexture = 0;

// One textured quad in screen space (virtual-resolution pixels).
struct SpriteDraw {
    TextureHandle texture = kNoTexture;
    Rect source{};           // texels; empty = whole texture
    Rect dest{};             // screen pixels
    float rotation = 0.0f;   // degrees clockwise around `pivot`
    Vec2 pivot{0.5f, 0.5f};  // normalized point within dest
    bool flipX = false;
    bool flipY = false;
    Color tint = colors::white;
};

// Minimal 2D renderer interface. Everything above this interface (scene
// extraction, camera math, HUD) is backend-independent; backends only draw
// what they are given. Coordinates are virtual-resolution pixels; the backend
// handles scaling to the real window.
//
// Planned extensions (not implemented): batched submission with a view
// transform, custom shaders/materials, render targets, lighting.
class IRenderer2D {
public:
    virtual ~IRenderer2D() = default;

    [[nodiscard]] virtual TextureHandle createTexture(const assets::Image& image, assets::TextureFilter filter) = 0;
    virtual void destroyTexture(TextureHandle texture) = 0;
    [[nodiscard]] virtual Vec2i textureSize(TextureHandle texture) const = 0;

    virtual void beginFrame(Color clearColor) = 0;
    virtual void drawSprite(const SpriteDraw& draw) = 0;
    virtual void fillRect(const Rect& rect, Color color) = 0;
    virtual void drawRect(const Rect& rect, Color color) = 0; // 1-pixel outline
    // Fixed 8x8 debug font, ASCII only. Not the final text system.
    virtual void drawDebugText(Vec2 position, std::string_view text, Color color) = 0;
    virtual void endFrame() = 0;

    static constexpr float kDebugGlyphSize = 8.0f;
};

} // namespace px::render
