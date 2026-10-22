#pragma once

#include "px/core/Keys.hpp"

#include <SDL3/SDL_scancode.h>

namespace px::platform {

// Physical key position (scancode) to engine key. Scancodes keep WASD in the
// same place on AZERTY and other layouts.
[[nodiscard]] Key keyFromScancode(SDL_Scancode scancode) noexcept;

} // namespace px::platform
