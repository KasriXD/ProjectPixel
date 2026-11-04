#include "px/platform_sdl/SdlPlatform.hpp"

#include "px/platform_sdl/SdlKeys.hpp"

#include "px/core/Log.hpp"
#include "px/core/Version.hpp"

#include <SDL3/SDL.h>

#include <format>

namespace px::platform {

Result<std::unique_ptr<SdlPlatform>> SdlPlatform::create(const WindowSettings& settings) {
    SDL_SetAppMetadata(version::productName, version::string, "dev.px.player");
    // Android's back button reaches the game (it opens the menu) instead of
    // closing the app.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return makeError(ErrorCode::Unsupported, "Could not initialise SDL video: {}", SDL_GetError());
    }
    // After video init: set earlier, SDL registers its mouse touch device
    // before the touch subsystem starts, which then forgets it.
    if (settings.mouseAsTouch) SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");
    // Gamepads are optional: no pad support on a platform is not an error.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) log::debug("No gamepad support: {}", SDL_GetError());

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
    platform->m_virtual = settings.virtualResolution;
    log::info("Window {}x{} ({})", width, height, platform->description());
    return platform;
}

SdlPlatform::~SdlPlatform() {
    for (SDL_Gamepad* pad : m_pads) SDL_CloseGamepad(pad);
    if (SDL_WasInit(SDL_INIT_GAMEPAD) != 0) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
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
            m_fingers.clear();
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (SDL_Gamepad* pad = SDL_OpenGamepad(event.gdevice.which)) {
                m_pads.push_back(pad);
                log::info("Gamepad connected: {}", SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "?");
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            for (auto it = m_pads.begin(); it != m_pads.end(); ++it) {
                if (SDL_GetGamepadID(*it) == event.gdevice.which) {
                    SDL_CloseGamepad(*it);
                    m_pads.erase(it);
                    break;
                }
            }
            updatePad();
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            updatePad();
            break;
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: {
            // Into the game's virtual pixels (SDL undoes the window scaling).
            SDL_Event e = event;
            SDL_ConvertEventToRenderCoordinates(m_renderer, &e);
            const Vec2 p{e.tfinger.x, e.tfinger.y};
            if (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED) {
                m_fingers.erase(event.tfinger.fingerID);
            } else {
                m_fingers[event.tfinger.fingerID] = p;
                if (event.type == SDL_EVENT_FINGER_DOWN) {
                    m_taps.push_back(p);
                    m_touchUsed = true;
                }
            }
            break;
        }
        default:
            break;
    }
    return false;
}

void SdlPlatform::updatePad() {
    PadStates now{};
    const auto press = [&now](PadButton b) { now[static_cast<std::size_t>(b)] = true; };
    for (SDL_Gamepad* pad : m_pads) {
        const std::pair<SDL_GamepadButton, PadButton> buttons[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, PadButton::South},
            {SDL_GAMEPAD_BUTTON_EAST, PadButton::East},
            {SDL_GAMEPAD_BUTTON_WEST, PadButton::West},
            {SDL_GAMEPAD_BUTTON_NORTH, PadButton::North},
            {SDL_GAMEPAD_BUTTON_BACK, PadButton::Back},
            {SDL_GAMEPAD_BUTTON_START, PadButton::Start},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PadButton::LeftShoulder},
            {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PadButton::RightShoulder},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, PadButton::DPadUp},
            {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PadButton::DPadDown},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PadButton::DPadLeft},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PadButton::DPadRight},
        };
        for (const auto& [sdl, ours] : buttons) {
            if (SDL_GetGamepadButton(pad, sdl)) press(ours);
        }
        // The left stick counts as the D-pad past 43 % of its travel, in
        // eight 45-degree sectors: running right with the stick a little
        // high does not also press up (which jumps in many games).
        constexpr float kThreshold = 14000.0f;
        constexpr float kTan22 = 0.4142f;
        const float x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        const float y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (x * x + y * y > kThreshold * kThreshold) {
            if (std::fabs(x) > kTan22 * std::fabs(y)) press(x < 0 ? PadButton::DPadLeft : PadButton::DPadRight);
            if (std::fabs(y) > kTan22 * std::fabs(x)) press(y < 0 ? PadButton::DPadUp : PadButton::DPadDown);
        }
    }
    for (std::size_t i = 0; i < kPadButtonCount; ++i) {
        if (now[i] && !m_padDown[i]) m_padPressed[i] = true;
    }
    m_padDown = now;
}

std::vector<Vec2> SdlPlatform::touches() const {
    std::vector<Vec2> out;
    out.reserve(m_fingers.size());
    for (const auto& [id, p] : m_fingers) out.push_back(p);
    return out;
}

void SdlPlatform::setVsync(bool on) {
    if (!SDL_SetRenderVSync(m_renderer, on ? 1 : 0)) log::debug("VSync: {}", SDL_GetError());
}

void SdlPlatform::setPresentation(Presentation presentation) {
    const SDL_RendererLogicalPresentation mode = presentation == Presentation::IntegerScale
                                                     ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE
                                                     : SDL_LOGICAL_PRESENTATION_LETTERBOX;
    SDL_SetRenderLogicalPresentation(m_renderer, m_virtual.x, m_virtual.y, mode);
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
