#include "JsonUtil.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>

namespace px::project::detail {

void fail(const std::string& path, std::string_view message) {
    if (path.empty()) throw FormatError(std::string(message));
    throw FormatError(std::format("{}: {}", path, message));
}

std::string childPath(const std::string& base, std::string_view key) {
    if (base.empty()) return std::string(key);
    return std::format("{}.{}", base, key);
}

std::string indexPath(const std::string& base, std::size_t index) {
    return std::format("{}[{}]", base, index);
}

namespace {

std::size_t editDistance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> prev(b.size() + 1);
    std::vector<std::size_t> cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
            const std::size_t cost = lower(a[i - 1]) == lower(b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

const char* typeName(const Json& j) {
    if (j.is_null()) return "null";
    if (j.is_boolean()) return "a boolean";
    if (j.is_number()) return "a number";
    if (j.is_string()) return "a string";
    if (j.is_array()) return "an array";
    if (j.is_object()) return "an object";
    return "an unknown value";
}

float numberAt(const Json& j, const std::string& path) {
    if (!j.is_number()) fail(path, std::format("expected a number, found {}", typeName(j)));
    const double d = j.get<double>();
    if (!std::isfinite(d) || std::abs(d) > static_cast<double>(std::numeric_limits<float>::max())) {
        fail(path, "number is out of range");
    }
    return static_cast<float>(d);
}

void expectArray(const Json& j, const std::string& path, std::size_t count, const char* what) {
    if (!j.is_array() || j.size() != count) {
        fail(path, std::format("expected {} (an array of {} numbers), found {}", what, count,
                               j.is_array() ? std::format("an array of {} items", j.size()) : typeName(j)));
    }
}

} // namespace

std::string spellingSuggestion(std::string_view key, const std::vector<std::string>& candidates) {
    std::string best;
    std::size_t bestDistance = 3; // suggest only close matches
    for (const auto& c : candidates) {
        const std::size_t d = editDistance(key, c);
        if (d < bestDistance) {
            bestDistance = d;
            best = c;
        }
    }
    return best.empty() ? std::string() : std::format(" (did you mean '{}'?)", best);
}

void readValue(const Json& j, const std::string& path, float& out) { out = numberAt(j, path); }

void readValue(const Json& j, const std::string& path, int& out) {
    if (!j.is_number_integer()) fail(path, std::format("expected a whole number, found {}", typeName(j)));
    const auto v = j.get<long long>();
    if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) {
        fail(path, "number is out of range");
    }
    out = static_cast<int>(v);
}

void readValue(const Json& j, const std::string& path, bool& out) {
    if (!j.is_boolean()) fail(path, std::format("expected true or false, found {}", typeName(j)));
    out = j.get<bool>();
}

void readValue(const Json& j, const std::string& path, std::string& out) {
    if (!j.is_string()) fail(path, std::format("expected a string, found {}", typeName(j)));
    out = j.get<std::string>();
}

void readValue(const Json& j, const std::string& path, Vec2& out) {
    expectArray(j, path, 2, "[x, y]");
    out = {numberAt(j[0], indexPath(path, 0)), numberAt(j[1], indexPath(path, 1))};
}

void readValue(const Json& j, const std::string& path, Vec2i& out) {
    expectArray(j, path, 2, "[x, y]");
    readValue(j[0], indexPath(path, 0), out.x);
    readValue(j[1], indexPath(path, 1), out.y);
}

void readValue(const Json& j, const std::string& path, Rect& out) {
    expectArray(j, path, 4, "[x, y, width, height]");
    out = {numberAt(j[0], indexPath(path, 0)), numberAt(j[1], indexPath(path, 1)), numberAt(j[2], indexPath(path, 2)),
           numberAt(j[3], indexPath(path, 3))};
    if (out.w < 0.0f || out.h < 0.0f) fail(path, "width and height may not be negative");
}

void readValue(const Json& j, const std::string& path, Color& out) {
    if (!j.is_string()) fail(path, std::format("expected a color string like \"#ff8800\", found {}", typeName(j)));
    const auto parsed = Color::parseHex(j.get<std::string>());
    if (!parsed) fail(path, std::format("'{}' is not a color; use \"#rrggbb\" or \"#rrggbbaa\"", j.get<std::string>()));
    out = *parsed;
}

void readValue(const Json& j, const std::string& path, Guid& out) {
    if (!j.is_string()) fail(path, std::format("expected an id string, found {}", typeName(j)));
    const auto parsed = Guid::parse(j.get<std::string>());
    if (!parsed) {
        fail(path, std::format("'{}' is not a valid id (expected xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)",
                               j.get<std::string>()));
    }
    out = *parsed;
}

void readValue(const Json& j, const std::string& path, assets::AssetId& out) { readValue(j, path, out.guid); }

Json toJson(Vec2 v) { return Json::array({toJson(v.x), toJson(v.y)}); }
Json toJson(Vec2i v) { return Json::array({v.x, v.y}); }
Json toJson(const Rect& r) { return Json::array({toJson(r.x), toJson(r.y), toJson(r.w), toJson(r.h)}); }
Json toJson(const Color& c) { return c.toHex(); }
Json toJson(const Guid& g) { return g.toString(); }
Json toJson(const assets::AssetId& id) { return id.toString(); }

Json toJson(float f) {
    // Integral values are written as integers ("16" rather than "16.0").
    if (std::floor(f) == f && std::abs(f) < 1.0e9f) return static_cast<long long>(f);
    // Shortest decimal that round-trips as a float, so 0.45f is written as
    // 0.45 rather than 0.44999998807907104.
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), f);
    return std::stod(std::string(buffer, result.ptr));
}

namespace {

bool isInlineArray(const Json& j) {
    if (!j.is_array() || j.empty()) return j.is_array();
    return std::all_of(j.begin(), j.end(), [](const Json& e) { return e.is_number() || e.is_boolean() || e.is_null(); });
}

void dumpPrettyImpl(const Json& j, int indent, std::string& out) {
    const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
    const std::string innerPad(static_cast<std::size_t>(indent + 1) * 2, ' ');
    if (j.is_object()) {
        if (j.empty()) {
            out += "{}";
            return;
        }
        out += "{\n";
        bool first = true;
        for (const auto& [key, value] : j.items()) {
            if (!first) out += ",\n";
            first = false;
            out += innerPad;
            out += Json(key).dump();
            out += ": ";
            dumpPrettyImpl(value, indent + 1, out);
        }
        out += "\n" + pad + "}";
    } else if (j.is_array()) {
        if (isInlineArray(j)) {
            out += "[";
            for (std::size_t i = 0; i < j.size(); ++i) {
                if (i > 0) out += ", ";
                out += j[i].dump();
            }
            out += "]";
            return;
        }
        out += "[\n";
        for (std::size_t i = 0; i < j.size(); ++i) {
            if (i > 0) out += ",\n";
            out += innerPad;
            dumpPrettyImpl(j[i], indent + 1, out);
        }
        out += "\n" + pad + "]";
    } else {
        out += j.dump();
    }
}

} // namespace

std::string dumpPretty(const Json& json) {
    std::string out;
    dumpPrettyImpl(json, 0, out);
    out += '\n';
    return out;
}

ObjectReader::ObjectReader(const Json& json, std::string path) : m_json(json), m_path(std::move(path)) {
    if (!m_json.is_object()) fail(m_path, std::format("expected an object, found {}", typeName(m_json)));
}

const Json* ObjectReader::find(std::string_view key) {
    m_known.emplace_back(key);
    const auto it = m_json.find(key);
    return it == m_json.end() ? nullptr : &*it;
}

const Json& ObjectReader::require(std::string_view key) {
    const Json* v = find(key);
    if (v == nullptr) fail(m_path, std::format("missing required field '{}'", key));
    return *v;
}

void ObjectReader::finish() const {
    for (const auto& [key, value] : m_json.items()) {
        if (std::find(m_known.begin(), m_known.end(), key) == m_known.end()) {
            fail(m_path, std::format("unknown field '{}'{}", key, spellingSuggestion(key, m_known)));
        }
    }
}

Json parseJsonText(std::string_view text) {
    try {
        return Json::parse(text.begin(), text.end());
    } catch (const nlohmann::json::parse_error& e) {
        // e.what() looks like "[json.exception.parse_error.101] parse error at line 3, column 5: ..."
        std::string message = e.what();
        if (const auto pos = message.find("] "); pos != std::string::npos) message = message.substr(pos + 2);
        throw FormatError(std::format("invalid JSON: {}", message));
    }
}

void checkAndMigrate(Json& root, std::string_view expectedFormat, int currentVersion,
                     std::span<const Migration> migrations, std::string_view fileKind) {
    if (!root.is_object()) fail("", std::format("a {} file must contain a JSON object", fileKind));
    const auto formatIt = root.find("format");
    if (formatIt == root.end() || !formatIt->is_string() || formatIt->get<std::string>() != expectedFormat) {
        fail("format", std::format("expected \"format\": \"{}\" (is this a {} file?)", expectedFormat, fileKind));
    }
    const auto versionIt = root.find("formatVersion");
    if (versionIt == root.end() || !versionIt->is_number_integer()) {
        fail("formatVersion", "missing or not a whole number");
    }
    const int version = versionIt->get<int>();
    if (version < 1) fail("formatVersion", std::format("{} is not a valid version", version));
    if (version > currentVersion) {
        throw FormatError(std::format(
            "this {} uses format version {}, but this engine supports up to version {}; update the engine",
            fileKind, version, currentVersion));
    }
    for (int v = version; v < currentVersion; ++v) {
        migrations[static_cast<std::size_t>(v - 1)](root);
        root["formatVersion"] = v + 1;
    }
}

} // namespace px::project::detail
