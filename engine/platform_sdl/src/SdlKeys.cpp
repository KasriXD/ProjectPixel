#include "px/platform_sdl/SdlKeys.hpp"

namespace px::platform {

Key keyFromScancode(SDL_Scancode sc) noexcept {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        return static_cast<Key>(static_cast<int>(Key::A) + (sc - SDL_SCANCODE_A));
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) {
        return static_cast<Key>(static_cast<int>(Key::Num1) + (sc - SDL_SCANCODE_1));
    }
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) {
        return static_cast<Key>(static_cast<int>(Key::F1) + (sc - SDL_SCANCODE_F1));
    }
    switch (sc) {
        case SDL_SCANCODE_0: return Key::Num0;
        case SDL_SCANCODE_SPACE: return Key::Space;
        case SDL_SCANCODE_RETURN: return Key::Enter;
        case SDL_SCANCODE_ESCAPE: return Key::Escape;
        case SDL_SCANCODE_BACKSPACE: return Key::Backspace;
        case SDL_SCANCODE_TAB: return Key::Tab;
        case SDL_SCANCODE_LEFT: return Key::Left;
        case SDL_SCANCODE_RIGHT: return Key::Right;
        case SDL_SCANCODE_UP: return Key::Up;
        case SDL_SCANCODE_DOWN: return Key::Down;
        case SDL_SCANCODE_LSHIFT: return Key::LeftShift;
        case SDL_SCANCODE_RSHIFT: return Key::RightShift;
        case SDL_SCANCODE_LCTRL: return Key::LeftCtrl;
        case SDL_SCANCODE_RCTRL: return Key::RightCtrl;
        case SDL_SCANCODE_LALT: return Key::LeftAlt;
        case SDL_SCANCODE_RALT: return Key::RightAlt;
        default: return Key::Unknown;
    }
}

} // namespace px::platform
