#include "PlayerSettings.hpp"

#include <fstream>
#include <string>

namespace px::player {

PlayerSettings PlayerSettings::load(const std::filesystem::path& file) {
    PlayerSettings s;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "vsync") {
            s.vsync = value != "off";
        } else if (key == "frameLimit") {
            if (value == "30" || value == "60" || value == "120") s.frameLimit = std::stoi(value);
        } else if (key == "scaling") {
            s.scaling = value == "pixel" ? Scaling::PixelPerfect : value == "fit" ? Scaling::Fit : Scaling::GameDefault;
        } else if (key == "showFps") {
            s.showFps = value == "on";
        } else if (key == "volume" || key == "musicVolume" || key == "effectsVolume") {
            int percent = -1;
            try {
                percent = std::stoi(value);
            } catch (...) {
            }
            if (percent >= 0 && percent <= 100) {
                (key == "volume" ? s.volume : key == "musicVolume" ? s.musicVolume : s.effectsVolume) = percent / 10 * 10;
            }
        } else if (key == "touch") {
            s.touch = value == "on" ? TouchMode::On : value == "off" ? TouchMode::Off : TouchMode::Auto;
        }
    }
    return s;
}

bool PlayerSettings::save(const std::filesystem::path& file) const {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    out << "vsync=" << (vsync ? "on" : "off") << "\n"
        << "frameLimit=" << frameLimit << "\n"
        << "scaling=" << (scaling == Scaling::PixelPerfect ? "pixel" : scaling == Scaling::Fit ? "fit" : "game") << "\n"
        << "showFps=" << (showFps ? "on" : "off") << "\n"
        << "touch=" << (touch == TouchMode::On ? "on" : touch == TouchMode::Off ? "off" : "auto") << "\n"
        << "volume=" << volume << "\n"
        << "musicVolume=" << musicVolume << "\n"
        << "effectsVolume=" << effectsVolume << "\n";
    return static_cast<bool>(out);
}

} // namespace px::player
