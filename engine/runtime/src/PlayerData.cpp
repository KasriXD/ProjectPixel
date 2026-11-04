#include "px/runtime/PlayerData.hpp"

#include <cctype>
#include <cmath>
#include <variant>

namespace px::runtime {

bool isValidDataKey(std::string_view key) {
    if (key.empty() || key.size() > kMaxPlayerDataKeyLength) return false;
    for (const char c : key) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

namespace {
std::size_t valueBytes(const scene::ScriptValue& v) {
    if (const auto* s = std::get_if<std::string>(&v)) return s->size() + 2;
    if (std::holds_alternative<Vec2>(v)) return 32;
    return 16;
}
} // namespace

const scene::ScriptValue* PlayerData::get(std::string_view key) const {
    const auto it = values.find(key);
    return it == values.end() ? nullptr : &it->second;
}

std::size_t PlayerData::approxBytes() const {
    std::size_t n = 2;
    for (const auto& [k, v] : values) n += k.size() + 4 + valueBytes(v);
    return n;
}

Status PlayerData::set(std::string_view key, std::optional<scene::ScriptValue> value) {
    if (!isValidDataKey(key)) {
        return makeError(ErrorCode::InvalidArgument,
                         "'{}' cannot be a data key: use 1 to {} letters, digits, '_', '-' or '.'", key,
                         kMaxPlayerDataKeyLength);
    }
    if (!value) {
        if (const auto it = values.find(key); it != values.end()) values.erase(it);
        return {};
    }
    if (const auto* s = std::get_if<std::string>(&*value); s != nullptr && s->size() > kMaxPlayerDataStringLength) {
        return makeError(ErrorCode::LimitExceeded, "text saved in '{}' is longer than {} characters", key,
                         kMaxPlayerDataStringLength);
    }
    if (const auto* d = std::get_if<double>(&*value); d != nullptr && !std::isfinite(*d)) {
        return makeError(ErrorCode::InvalidArgument, "'{}': only finite numbers can be saved", key);
    }
    if (const auto* v = std::get_if<Vec2>(&*value); v != nullptr && !(std::isfinite(v->x) && std::isfinite(v->y))) {
        return makeError(ErrorCode::InvalidArgument, "'{}': only finite vectors can be saved", key);
    }
    PlayerData next = *this;
    next.values.insert_or_assign(std::string(key), std::move(*value));
    if (next.values.size() > kMaxPlayerDataKeys) {
        return makeError(ErrorCode::LimitExceeded, "a player can have at most {} saved values", kMaxPlayerDataKeys);
    }
    if (next.approxBytes() > kMaxPlayerDataBytes) {
        return makeError(ErrorCode::LimitExceeded, "a player's saved data is limited to {} KiB",
                         kMaxPlayerDataBytes / 1024);
    }
    *this = std::move(next);
    return {};
}

} // namespace px::runtime
