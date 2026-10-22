#pragma once

#include "px/core/Guid.hpp"

#include <compare>
#include <optional>
#include <string>
#include <string_view>

namespace px::assets {

// Stable identifier of an asset. Files reference assets by AssetId, never by
// path, so assets can be renamed or moved without breaking references.
struct AssetId {
    Guid guid;

    [[nodiscard]] constexpr bool isNil() const { return guid.isNil(); }
    [[nodiscard]] std::string toString() const { return guid.toString(); }
    [[nodiscard]] static std::optional<AssetId> parse(std::string_view text) {
        if (auto g = Guid::parse(text)) return AssetId{*g};
        return std::nullopt;
    }

    friend constexpr auto operator<=>(const AssetId&, const AssetId&) = default;
};

} // namespace px::assets

template <>
struct std::hash<px::assets::AssetId> {
    std::size_t operator()(const px::assets::AssetId& id) const noexcept { return std::hash<px::Guid>{}(id.guid); }
};
