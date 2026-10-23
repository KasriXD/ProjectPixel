#pragma once

#include "px/render/Renderer2D.hpp"

#include <vector>

struct SDL_Renderer;
struct SDL_Texture;

namespace px::platform {

// IRenderer2D on top of SDL_Renderer. SDL picks the GPU backend
// (Direct3D, Metal, OpenGL, Vulkan, or software) and batches draws.
class SdlRenderer2D final : public render::IRenderer2D {
public:
    explicit SdlRenderer2D(SDL_Renderer* renderer); // does not take ownership
    ~SdlRenderer2D() override;
    SdlRenderer2D(const SdlRenderer2D&) = delete;
    SdlRenderer2D& operator=(const SdlRenderer2D&) = delete;

    [[nodiscard]] render::TextureHandle createTexture(const assets::Image& image, assets::TextureFilter filter) override;
    void destroyTexture(render::TextureHandle texture) override;
    [[nodiscard]] Vec2i textureSize(render::TextureHandle texture) const override;

    void beginFrame(Color clearColor) override;
    void drawSprite(const render::SpriteDraw& draw) override;
    void fillRect(const Rect& rect, Color color) override;
    void drawRect(const Rect& rect, Color color) override;
    void drawDebugText(Vec2 position, std::string_view text, Color color) override;
    void endFrame() override;

    [[nodiscard]] SDL_Renderer* sdlRenderer() const { return m_renderer; }

    // Render targets (used by Studio's viewport). createRenderTarget returns a
    // texture that can be drawn into after setRenderTarget(handle) and then
    // drawn like any texture; setRenderTarget(kNoTexture) draws to the window
    // again. kNoTexture on failure.
    [[nodiscard]] render::TextureHandle createRenderTarget(Vec2i size);
    bool setRenderTarget(render::TextureHandle target);
    // The SDL texture behind a handle (for UI libraries), or nullptr.
    [[nodiscard]] SDL_Texture* sdlTexture(render::TextureHandle texture) const { return lookup(texture); }

private:
    [[nodiscard]] SDL_Texture* lookup(render::TextureHandle texture) const;
    [[nodiscard]] render::TextureHandle store(SDL_Texture* texture);

    SDL_Renderer* m_renderer;
    std::vector<SDL_Texture*> m_textures; // index = handle - 1; nullptr = free slot
};

} // namespace px::platform
