#pragma once

#include "px/core/Result.hpp"
#include "px/scene/Components.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// Saved data of one player in one game (scripts: Data.get / Data.set;
// docs/platform.md#saved-player-data). On the platform it is loaded when the
// player joins a game server and saved while they play and when they leave;
// played locally it lives in memory.
namespace px::runtime {

inline constexpr std::size_t kMaxPlayerDataKeys = 200;
inline constexpr std::size_t kMaxPlayerDataKeyLength = 50;
inline constexpr std::size_t kMaxPlayerDataStringLength = 1000;
inline constexpr std::size_t kMaxPlayerDataBytes = 64 * 1024;

// 1-50 letters, digits, '_', '-' or '.'.
[[nodiscard]] bool isValidDataKey(std::string_view key);

struct PlayerData {
    std::map<std::string, scene::ScriptValue, std::less<>> values;

    [[nodiscard]] const scene::ScriptValue* get(std::string_view key) const;
    // Sets or (nullopt) removes a value. Errors are written for script authors.
    [[nodiscard]] Status set(std::string_view key, std::optional<scene::ScriptValue> value);
    // Roughly the stored size, for the per-player limit.
    [[nodiscard]] std::size_t approxBytes() const;
    bool operator==(const PlayerData&) const = default;
};

} // namespace px::runtime
