#include "px/platform_sdl/SdlPlatform.hpp"

#include "px/platform_sdl/SdlKeys.hpp"

#include "px/core/Log.hpp"
#include "px/core/Version.hpp"

#include <SDL3/SDL.h>

#include <format>

namespace px::platform {

Result<std::unique_ptr<SdlPlatform>> SdlPlatform::create(const WindowSettings& settings) {
    SDL_SetAppMetadata(version::productName, version::string, "dev.px.player");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return makeError(ErrorCode::Unsupported, "Could not initialise SDL video: {}", SDL_GetError());
    }

    std::unique_ptr<SdlPlatform> platform(new SdlPlatform());
    const int width = settings.virtualResolution.x * settings.windowScale;
    const int height = settings.virtualResolution.y * settings.windowScale;
    if (!SDL_CreateWindowAndRenderer(settings.title.c_str(), width, height,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &platform->m_window,
                                     &platform->m_renderer)) {
        return makeError(ErrorCode::Unsupported, "Could not create a window and renderer: {}", SDL_GetError());
    }
    if (settings.position) SDL_SetWindowPosition(platform->m_window, settings.position->x, settings.position->y);
    // The game draws at the virtual resolution; SDL scales it to the window.
    const SDL_RendererLogicalPresentation mode = settings.presentation == Presentation::IntegerScale
                                                     ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE
                                                     : SDL_LOGICAL_PRESENTATION_LETTERBOX;
    if (!SDL_SetRenderLogicalPresentation(platform->m_renderer, settings.virtualResolution.x,
                                          settings.virtualResolution.y, mode)) {
        log::warn("SDL_SetRenderLogicalPresentation failed: {}", SDL_GetError());
    }
    if (!SDL_SetRenderVSync(platform->m_renderer, settings.vsync ? 1 : 0)) {
        log::debug("VSync setting not supported by this renderer: {}", SDL_GetError());
    }
    platform->m_renderer2d = std::make_unique<SdlRenderer2D>(platform->m_renderer);
    log::info("Window {}x{} ({})", width, height, platform->description());
    return platform;
}

SdlPlatform::~SdlPlatform() {
    m_renderer2d.reset(); // textures must be destroyed before the renderer
    if (m_renderer != nullptr) SDL_DestroyRenderer(m_renderer);
    if (m_window != nullptr) SDL_DestroyWindow(m_window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

bool SdlPlatform::processEvent(const SDL_Event& event) {
    switch (event.type) {
        case SDL_EVENT_QUIT:
            return true;
        case SDL_EVENT_KEY_DOWN: {
            const Key key = keyFromScancode(event.key.scancode);
            if (key != Key::Unknown) {
                const auto index = static_cast<std::size_t>(key);
                if (!event.key.repeat) m_pressed[index] = true;
                m_down[index] = true;
            }
            break;
        }
        case SDL_EVENT_KEY_UP: {
            const Key key = keyFromScancode(event.key.scancode);
            if (key != Key::Unknown) m_down[static_cast<std::size_t>(key)] = false;
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            // Key-up events are not delivered while unfocused; avoid stuck keys.
            m_down = {};
            break;
        default:
            break;
    }
    return false;
}

Status SdlPlatform::saveScreenshot(const std::filesystem::path& path) {
    SDL_Surface* surface = SDL_RenderReadPixels(m_renderer, nullptr);
    if (surface == nullptr) {
        return makeError(ErrorCode::Unsupported, "Could not read back the frame: {}", SDL_GetError());
    }
    const bool saved = SDL_SavePNG(surface, path.string().c_str());
    SDL_DestroySurface(surface);
    if (!saved) {
        return makeError(ErrorCode::IoError, "Could not save {}: {}", path.string(), SDL_GetError());
    }
    return {};
}

double SdlPlatform::secondsSinceStart() const { return static_cast<double>(SDL_GetTicksNS()) * 1.0e-9; }

std::string SdlPlatform::description() const {
    const char* driver = SDL_GetCurrentVideoDriver();
    const char* renderer = m_renderer ? SDL_GetRendererName(m_renderer) : nullptr;
    return std::format("video driver '{}', renderer '{}'", driver ? driver : "?", renderer ? renderer : "?");
}

} // namespace px::platform
