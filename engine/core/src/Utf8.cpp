#include "px/core/Utf8.hpp"

namespace px::utf8 {

char32_t next(std::string_view text, std::size_t& pos) {
    if (pos >= text.size()) return kReplacement;
    const auto b0 = static_cast<unsigned char>(text[pos]);
    if (b0 < 0x80) {
        ++pos;
        return b0;
    }
    int length = 0;
    char32_t c = 0;
    char32_t min = 0;
    if ((b0 & 0xE0) == 0xC0) {
        length = 2;
        c = b0 & 0x1F;
        min = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        length = 3;
        c = b0 & 0x0F;
        min = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        length = 4;
        c = b0 & 0x07;
        min = 0x10000;
    } else {
        ++pos;
        return kReplacement;
    }
    if (pos + static_cast<std::size_t>(length) > text.size()) {
        ++pos;
        return kReplacement;
    }
    for (int i = 1; i < length; ++i) {
        const auto b = static_cast<unsigned char>(text[pos + static_cast<std::size_t>(i)]);
        if ((b & 0xC0) != 0x80) {
            ++pos;
            return kReplacement;
        }
        c = (c << 6) | (b & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        ++pos;
        return kReplacement;
    }
    pos += static_cast<std::size_t>(length);
    return c;
}

void append(std::string& out, char32_t c) {
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = kReplacement;
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

std::string_view truncate(std::string_view text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) return text;
    std::size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end);
}

} // namespace px::utf8
