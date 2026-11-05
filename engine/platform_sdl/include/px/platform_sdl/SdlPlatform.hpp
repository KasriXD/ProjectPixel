#pragma once

#include "px/core/Keys.hpp"
#include "px/core/Math.hpp"
#include "px/core/Result.hpp"
#include "px/platform_sdl/SdlRenderer2D.hpp"

#include <filesystem>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <vector>

union SDL_Event;
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Gamepad;

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
    std::optional<Vec2i> position; // screen position of the window's top-left corner (default: centred)
    bool mouseAsTouch = false;     // the mouse acts as a finger (trying touch controls on a desktop)
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

    // Gamepads: every connected pad counts (one local player).
    [[nodiscard]] const PadStates& padDown() const { return m_padDown; }
    [[nodiscard]] const PadStates& padPressed() const { return m_padPressed; }
    void clearPressedPad() { m_padPressed = {}; }
    [[nodiscard]] int gamepadCount() const { return static_cast<int>(m_pads.size()); }

    // Fingers on a touch screen, in virtual-resolution pixels.
    [[nodiscard]] std::vector<Vec2> touches() const;
    // Points touched since the last clearTaps(): quick taps between frames count.
    [[nodiscard]] const std::vector<Vec2>& taps() const { return m_taps; }
    void clearTaps() { m_taps.clear(); }
    [[nodiscard]] bool touchUsed() const { return m_touchUsed; }

    // Per-device settings that can change while playing.
    void setVsync(bool on);
    void setPresentation(Presentation presentation);
    // The same window for another game (the launcher hands it to each game it
    // starts): title, virtual resolution, presentation, vsync. The window keeps its size.
    void reconfigure(const WindowSettings& settings);
    // Drawing in window pixels, without the game's virtual resolution (the launcher's UI).
    void useWindowPixels();
    // Forgets keys, buttons and taps (switching between the launcher and a game).
    void clearInput();

    [[nodiscard]] SDL_Window* sdlWindow() const { return m_window; }
    [[nodiscard]] SDL_Renderer* sdlRenderer() const { return m_renderer; }
    [[nodiscard]] SdlRenderer2D& sdlRenderer2D() { return *m_renderer2d; }

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
    void updatePad();

    KeyStates m_down{};
    KeyStates m_pressed{};
    std::vector<SDL_Gamepad*> m_pads;
    PadStates m_padDown{};
    PadStates m_padPressed{};
    std::map<std::uint64_t, Vec2> m_fingers; // by SDL finger id
    std::vector<Vec2> m_taps;
    bool m_touchUsed = false;
    Vec2i m_virtual{320, 180};
};

} // namespace px::platform
