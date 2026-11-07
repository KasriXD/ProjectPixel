#pragma once

#include <filesystem>
#include <string>

// Settings that belong to a device, not a game (docs/input-devices.md#settings):
// remembered between runs in the user's preferences folder.
namespace px::player {

enum class Scaling { GameDefault, PixelPerfect, Fit };
enum class TouchMode { Auto, On, Off }; // Auto: shown after the screen is first touched

struct PlayerSettings {
    bool vsync = true;
    int frameLimit = 0; // frames per second; 0: no limit (vsync still applies)
    Scaling scaling = Scaling::GameDefault;
    bool showFps = false;
    TouchMode touch = TouchMode::Auto;
    // Sound, in percent (steps of 10).
    int volume = 80;
    int musicVolume = 100;
    int effectsVolume = 100;

    // "key=value" lines; unknown keys and bad values are ignored.
    [[nodiscard]] static PlayerSettings load(const std::filesystem::path& file);
    bool save(const std::filesystem::path& file) const;
};

} // namespace px::player
