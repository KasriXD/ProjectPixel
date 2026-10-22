#pragma once

#include "px/core/Keys.hpp"
#include "px/core/Math.hpp"
#include "px/core/Result.hpp"
#include "px/platform_sdl/SdlRenderer2D.hpp"

#include <filesystem>
#include <memory>
#include <string>

union SDL_Event;
struct SDL_Window;
struct SDL_Renderer;

namespace px::platform {

enum class Presentation {
    IntegerScale, // whole-number scaling with borders: crisp pixel art
    Letterbox,    // fractional scaling preserving aspect ratio
};

struct WindowSettings {
    std::string title;
    Vec2i virtualResolution{320, 180};
    Presentation presentation = Presentation::IntegerScale;
    int windowScale = 3;
    bool vsync = true;
};

// Owns the SDL window and renderer and tracks keyboard state.
// The application owns the main loop (SDL main callbacks) and forwards every
// SDL event to processEvent().
class SdlPlatform {
public:
    [[nodiscard]] static Result<std::unique_ptr<SdlPlatform>> create(const WindowSettings& settings);
    ~SdlPlatform();
    SdlPlatform(const SdlPlatform&) = delete;
    SdlPlatform& operator=(const SdlPlatform&) = delete;

    // Returns true when the event requests that the application quit.
    bool processEvent(const SDL_Event& event);

    [[nodiscard]] const KeyStates& keysDown() const { return m_down; }
    // Keys pressed since the last clearPressedKeys(); catches taps shorter than a tick.
    [[nodiscard]] const KeyStates& keysPressed() const { return m_pressed; }
    void clearPressedKeys() { m_pressed = {}; }

    [[nodiscard]] render::IRenderer2D& renderer() { return *m_renderer2d; }

    // Saves the current frame (call after drawing, before endFrame()) as PNG.
    [[nodiscard]] Status saveScreenshot(const std::filesystem::path& path);

    [[nodiscard]] double secondsSinceStart() const;
    [[nodiscard]] std::string description() const; // video driver and renderer, for logs

private:
    SdlPlatform() = default;

    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    std::unique_ptr<SdlRenderer2D> m_renderer2d;
    KeyStates m_down{};
    KeyStates m_pressed{};
};

} // namespace px::platform
