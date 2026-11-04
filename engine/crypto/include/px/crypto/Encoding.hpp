#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Text encodings for binary data (tokens, hashes, keys).
namespace px::crypto {

[[nodiscard]] std::string toHex(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> fromHex(std::string_view text);

// RFC 4648 base64url without padding: safe in URLs, headers and file names.
[[nodiscard]] std::string base64UrlEncode(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> base64UrlDecode(std::string_view text);

[[nodiscard]] inline std::span<const std::uint8_t> bytesOf(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

} // namespace px::crypto
