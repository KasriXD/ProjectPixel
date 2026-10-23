#include "px/platform_sdl/SdlRenderer2D.hpp"

#include "px/core/Log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <string>

namespace px::platform {

SdlRenderer2D::SdlRenderer2D(SDL_Renderer* renderer) : m_renderer(renderer) {}

SdlRenderer2D::~SdlRenderer2D() {
    for (SDL_Texture* t : m_textures) {
        if (t != nullptr) SDL_DestroyTexture(t);
    }
}

render::TextureHandle SdlRenderer2D::createTexture(const assets::Image& image, assets::TextureFilter filter) {
    SDL_Texture* texture =
        SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, image.width, image.height);
    if (texture == nullptr) {
        log::error("SDL_CreateTexture({}x{}) failed: {}", image.width, image.height, SDL_GetError());
        return render::kNoTexture;
    }
    if (!SDL_UpdateTexture(texture, nullptr, image.pixels.data(), image.width * 4)) {
        log::error("SDL_UpdateTexture failed: {}", SDL_GetError());
        SDL_DestroyTexture(texture);
        return render::kNoTexture;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture,
                            filter == assets::TextureFilter::Nearest ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    return store(texture);
}

render::TextureHandle SdlRenderer2D::store(SDL_Texture* texture) {
    for (std::size_t i = 0; i < m_textures.size(); ++i) {
        if (m_textures[i] == nullptr) {
            m_textures[i] = texture;
            return static_cast<render::TextureHandle>(i + 1);
        }
    }
    m_textures.push_back(texture);
    return static_cast<render::TextureHandle>(m_textures.size());
}

render::TextureHandle SdlRenderer2D::createRenderTarget(Vec2i size) {
    SDL_Texture* texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
                                             std::max(1, size.x), std::max(1, size.y));
    if (texture == nullptr) {
        log::error("SDL_CreateTexture(render target {}x{}) failed: {}", size.x, size.y, SDL_GetError());
        return render::kNoTexture;
    }
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    return store(texture);
}

bool SdlRenderer2D::setRenderTarget(render::TextureHandle target) {
    SDL_Texture* texture = target == render::kNoTexture ? nullptr : lookup(target);
    if (target != render::kNoTexture && texture == nullptr) return false;
    return SDL_SetRenderTarget(m_renderer, texture);
}

SDL_Texture* SdlRenderer2D::lookup(render::TextureHandle texture) const {
    if (texture == render::kNoTexture || texture > m_textures.size()) return nullptr;
    return m_textures[texture - 1];
}

void SdlRenderer2D::destroyTexture(render::TextureHandle texture) {
    if (SDL_Texture* t = lookup(texture)) {
        SDL_DestroyTexture(t);
        m_textures[texture - 1] = nullptr;
    }
}

Vec2i SdlRenderer2D::textureSize(render::TextureHandle texture) const {
    float w = 0.0f;
    float h = 0.0f;
    if (SDL_Texture* t = lookup(texture)) SDL_GetTextureSize(t, &w, &h);
    return {static_cast<int>(w), static_cast<int>(h)};
}

void SdlRenderer2D::beginFrame(Color c) {
    SDL_SetRenderDrawColor(m_renderer, c.r, c.g, c.b, c.a);
    SDL_RenderClear(m_renderer);
}

void SdlRenderer2D::drawSprite(const render::SpriteDraw& draw) {
    SDL_Texture* texture = lookup(draw.texture);
    if (texture == nullptr) return;
    SDL_SetTextureColorMod(texture, draw.tint.r, draw.tint.g, draw.tint.b);
    SDL_SetTextureAlphaMod(texture, draw.tint.a);

    const SDL_FRect dst{draw.dest.x, draw.dest.y, draw.dest.w, draw.dest.h};
    const SDL_FRect src{draw.source.x, draw.source.y, draw.source.w, draw.source.h};
    const SDL_FRect* srcPtr = draw.source.empty() ? nullptr : &src;
    int flip = SDL_FLIP_NONE;
    if (draw.flipX) flip |= SDL_FLIP_HORIZONTAL;
    if (draw.flipY) flip |= SDL_FLIP_VERTICAL;
    if (draw.rotation == 0.0f && flip == SDL_FLIP_NONE) {
        SDL_RenderTexture(m_renderer, texture, srcPtr, &dst);
    } else {
        const SDL_FPoint center{draw.dest.w * draw.pivot.x, draw.dest.h * draw.pivot.y};
        SDL_RenderTextureRotated(m_renderer, texture, srcPtr, &dst, draw.rotation, &center,
                                 static_cast<SDL_FlipMode>(flip));
    }
}

void SdlRenderer2D::fillRect(const Rect& rect, Color c) {
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(m_renderer, c.r, c.g, c.b, c.a);
    const SDL_FRect r{rect.x, rect.y, rect.w, rect.h};
    SDL_RenderFillRect(m_renderer, &r);
}

void SdlRenderer2D::drawRect(const Rect& rect, Color c) {
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(m_renderer, c.r, c.g, c.b, c.a);
    const SDL_FRect r{rect.x, rect.y, rect.w, rect.h};
    SDL_RenderRect(m_renderer, &r);
}

void SdlRenderer2D::drawDebugText(Vec2 position, std::string_view text, Color c) {
    SDL_SetRenderDrawColor(m_renderer, c.r, c.g, c.b, c.a);
    const std::string copy(text); // SDL needs a NUL-terminated string
    SDL_RenderDebugText(m_renderer, position.x, position.y, copy.c_str());
}

void SdlRenderer2D::endFrame() { SDL_RenderPresent(m_renderer); }

} // namespace px::platform
