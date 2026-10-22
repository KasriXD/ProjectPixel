#pragma once

// Internal helpers shared by the project and scene file formats.
// Parsing code throws FormatError internally; public functions catch it at
// the module boundary and return Result errors.

#include "px/assets/AssetId.hpp"
#include "px/core/Color.hpp"
#include "px/core/Guid.hpp"
#include "px/core/Math.hpp"

#include <nlohmann/json.hpp>

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace px::project::detail {

using Json = nlohmann::ordered_json;

class FormatError : public std::runtime_error {
public:
    explicit FormatError(std::string message) : std::runtime_error(std::move(message)) {}
};

[[noreturn]] void fail(const std::string& path, std::string_view message);

[[nodiscard]] std::string childPath(const std::string& base, std::string_view key);
[[nodiscard]] std::string indexPath(const std::string& base, std::size_t index);

// " (did you mean 'x'?)" if a candidate is a close spelling of `key`, else "".
[[nodiscard]] std::string spellingSuggestion(std::string_view key, const std::vector<std::string>& candidates);

void readValue(const Json& j, const std::string& path, float& out);
void readValue(const Json& j, const std::string& path, int& out);
void readValue(const Json& j, const std::string& path, bool& out);
void readValue(const Json& j, const std::string& path, std::string& out);
void readValue(const Json& j, const std::string& path, Vec2& out);
void readValue(const Json& j, const std::string& path, Vec2i& out);
void readValue(const Json& j, const std::string& path, Rect& out);
void readValue(const Json& j, const std::string& path, Color& out);
void readValue(const Json& j, const std::string& path, Guid& out);
void readValue(const Json& j, const std::string& path, assets::AssetId& out);

[[nodiscard]] Json toJson(Vec2 v);
[[nodiscard]] Json toJson(Vec2i v);
[[nodiscard]] Json toJson(const Rect& r);
[[nodiscard]] Json toJson(const Color& c);
[[nodiscard]] Json toJson(const Guid& g);
[[nodiscard]] Json toJson(const assets::AssetId& id);
// Floats are written with the shortest representation that round-trips.
[[nodiscard]] Json toJson(float f);

// Reads fields of a JSON object and reports unknown (e.g. misspelled) keys.
class ObjectReader {
public:
    ObjectReader(const Json& json, std::string path);

    [[nodiscard]] const Json* find(std::string_view key);
    [[nodiscard]] const Json& require(std::string_view key);

    template <class T>
    bool optional(std::string_view key, T& out) {
        if (const Json* v = find(key)) {
            readValue(*v, childPath(m_path, key), out);
            return true;
        }
        return false;
    }

    template <class T>
    void required(std::string_view key, T& out) {
        readValue(require(key), childPath(m_path, key), out);
    }

    // Fails if the object has keys that were never looked up.
    void finish() const;

    [[nodiscard]] const std::string& path() const { return m_path; }

private:
    const Json& m_json;
    std::string m_path;
    std::vector<std::string> m_known;
};

// Parses JSON text; throws FormatError with line/column on syntax errors.
[[nodiscard]] Json parseJsonText(std::string_view text);

// Pretty-prints with 2-space indentation, keeping arrays of numbers/bools on
// one line ("[16, 16]") so files stay compact and diff-friendly. Ends with '\n'.
[[nodiscard]] std::string dumpPretty(const Json& json);

// Upgrades a document by one format version. Changes that alter behaviour
// are described in `warnings`, which loaders pass on to the creator.
using Migration = void (*)(Json& root, std::vector<std::string>& warnings);

// Checks the "format" tag and "formatVersion", then applies migrations in
// order. migrations[i] upgrades version i+1 to i+2.
void checkAndMigrate(Json& root, std::string_view expectedFormat, int currentVersion,
                     std::span<const Migration> migrations, std::string_view fileKind,
                     std::vector<std::string>& warnings);

} // namespace px::project::detail
