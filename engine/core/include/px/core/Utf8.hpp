#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace px::utf8 {

inline constexpr char32_t kReplacement = 0xFFFD;

// The character starting at `pos`, moving `pos` past it. Broken sequences
// (stray continuation bytes, overlong forms, surrogates, past U+10FFFF, cut
// off) give kReplacement and skip one byte, so decoding always moves on.
[[nodiscard]] char32_t next(std::string_view text, std::size_t& pos);

// Appends `c` as UTF-8 (kReplacement for values that are not characters).
void append(std::string& out, char32_t c);

// Shortens `text` to at most `maxBytes` without cutting a character in two.
[[nodiscard]] std::string_view truncate(std::string_view text, std::size_t maxBytes);

} // namespace px::utf8
