#include "px/project/Project.hpp"

#include "JsonUtil.hpp"

#include "px/core/Version.hpp"

#include <array>
#include <cmath>
#include <format>

namespace px::project {

using detail::fail;
using detail::Json;
using detail::ObjectReader;

namespace {

// Version 2 added settings.gameType. Every project before it was a platformer.
void migrateProject1To2(Json& root, std::vector<std::string>&) {
    if (!root.contains("settings") || !root["settings"].is_object()) root["settings"] = Json::object();
    root["settings"]["gameType"] = "platformer";
}

// Project format migrations: kProjectMigrations[i] upgrades version i+1 to i+2.
constexpr std::array<detail::Migration, 1> kProjectMigrations{&migrateProject1To2};

bool isValidActionName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (const char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

ProjectSettings readSettings(const Json& j, const std::string& path) {
    ProjectSettings settings;
    ObjectReader r(j, path);
    std::string gameType = "platformer";
    r.optional("gameType", gameType);
    if (const auto parsed = parseGameType(gameType)) {
        settings.gameType = *parsed;
    } else {
        fail(detail::childPath(path, "gameType"),
             std::format("unknown game type '{}'; use \"platformer\" or \"topdown\"", gameType));
    }
    r.optional("tickRate", settings.tickRate);
    if (settings.tickRate < 1 || settings.tickRate > 240) {
        fail(detail::childPath(path, "tickRate"), "must be between 1 and 240");
    }
    if (const Json* display = r.find("display")) {
        const std::string displayPath = detail::childPath(path, "display");
        ObjectReader d(*display, displayPath);
        d.optional("virtualResolution", settings.display.virtualResolution);
        const auto& res = settings.display.virtualResolution;
        if (res.x < 16 || res.y < 16 || res.x > 7680 || res.y > 4320) {
            fail(detail::childPath(displayPath, "virtualResolution"), "must be between [16, 16] and [7680, 4320]");
        }
        std::string scaling = "integer";
        d.optional("scaling", scaling);
        if (scaling == "integer") {
            settings.display.scaling = ScalingMode::Integer;
        } else if (scaling == "letterbox") {
            settings.display.scaling = ScalingMode::Letterbox;
        } else {
            fail(detail::childPath(displayPath, "scaling"),
                 std::format("unknown scaling mode '{}'; use \"integer\" or \"letterbox\"", scaling));
        }
        d.optional("windowScale", settings.display.windowScale);
        if (settings.display.windowScale < 1 || settings.display.windowScale > 16) {
            fail(detail::childPath(displayPath, "windowScale"), "must be between 1 and 16");
        }
        d.finish();
    }
    if (const Json* scripting = r.find("scripting")) {
        const std::string scriptingPath = detail::childPath(path, "scripting");
        ObjectReader sr(*scripting, scriptingPath);
        sr.optional("apiVersion", settings.scripting.apiVersion);
        const int v = settings.scripting.apiVersion;
        if (v < 1) fail(detail::childPath(scriptingPath, "apiVersion"), "must be 1 or higher");
        if (v > kScriptApiVersion) {
            fail(detail::childPath(scriptingPath, "apiVersion"),
                 std::format("this project needs scripting API version {}, but this engine provides version {}; "
                             "update the engine",
                             v, kScriptApiVersion));
        }
        sr.finish();
    }
    r.finish();
    return settings;
}

InputConfig readInput(const Json& j, const std::string& path) {
    InputConfig input;
    ObjectReader r(j, path);
    if (const Json* actions = r.find("actions")) {
        const std::string actionsPath = detail::childPath(path, "actions");
        if (!actions->is_object()) fail(actionsPath, "expected an object mapping action names to key lists");
        if (actions->size() > kMaxInputActions) {
            fail(actionsPath, std::format("too many actions ({}); the limit is {}", actions->size(), kMaxInputActions));
        }
        for (const auto& [name, keys] : actions->items()) {
            const std::string actionPath = detail::childPath(actionsPath, name);
            if (!isValidActionName(name)) {
                fail(actionPath, "action names may only contain a-z, 0-9 and '_'");
            }
            if (!keys.is_array()) fail(actionPath, "expected an array of key names");
            InputAction action;
            action.name = name;
            for (std::size_t i = 0; i < keys.size(); ++i) {
                std::string keyNameText;
                detail::readValue(keys[i], detail::indexPath(actionPath, i), keyNameText);
                const Key key = keyFromName(keyNameText);
                if (key == Key::Unknown) {
                    fail(detail::indexPath(actionPath, i), std::format("unknown key '{}'", keyNameText));
                }
                action.keys.push_back(key);
            }
            input.actions.push_back(std::move(action));
        }
    }
    r.finish();
    return input;
}

void readAssets(const Json& j, const std::string& path, assets::AssetRegistry& registry) {
    if (!j.is_array()) fail(path, "expected an array of assets");
    if (j.size() > kMaxAssets) fail(path, std::format("too many assets; the limit is {}", kMaxAssets));
    for (std::size_t i = 0; i < j.size(); ++i) {
        const std::string itemPath = detail::indexPath(path, i);
        ObjectReader r(j[i], itemPath);
        assets::AssetEntry entry;
        r.required("id", entry.id);
        std::string typeName;
        r.required("type", typeName);
        const auto type = assets::assetTypeFromName(typeName);
        if (!type) fail(detail::childPath(itemPath, "type"), std::format("unknown asset type '{}'", typeName));
        entry.type = *type;
        r.required("path", entry.path);
        entry.name = entry.path;
        r.optional("name", entry.name);
        if (entry.type == assets::AssetType::Texture) {
            std::string filter = "nearest";
            r.optional("filter", filter);
            const auto parsed = assets::textureFilterFromName(filter);
            if (!parsed) {
                fail(detail::childPath(itemPath, "filter"),
                     std::format("unknown filter '{}'; use \"nearest\" or \"linear\"", filter));
            }
            entry.filter = *parsed;
        }
        r.finish();
        if (auto status = registry.add(std::move(entry)); !status) fail(itemPath, status.error().message);
    }
}

std::vector<LeaderboardDef> readLeaderboards(const Json& j, const std::string& path) {
    if (!j.is_array()) fail(path, "expected an array of leaderboards");
    if (j.size() > kMaxLeaderboards) {
        fail(path, std::format("too many leaderboards ({}); the limit is {}", j.size(), kMaxLeaderboards));
    }
    std::vector<LeaderboardDef> out;
    for (std::size_t i = 0; i < j.size(); ++i) {
        const std::string itemPath = detail::indexPath(path, i);
        ObjectReader r(j[i], itemPath);
        LeaderboardDef b;
        r.required("id", b.id);
        if (!isValidLeaderboardId(b.id)) {
            fail(detail::childPath(itemPath, "id"), "leaderboard ids are 1-32 characters: a-z, 0-9 and '_'");
        }
        for (const auto& other : out) {
            if (other.id == b.id) fail(detail::childPath(itemPath, "id"), std::format("'{}' is used twice", b.id));
        }
        b.title = b.id;
        r.optional("title", b.title);
        if (b.title.empty() || b.title.size() > kMaxLeaderboardTitle) {
            fail(detail::childPath(itemPath, "title"), std::format("must be 1 to {} characters", kMaxLeaderboardTitle));
        }
        for (const char c : b.title) {
            if (static_cast<unsigned char>(c) < 0x20) fail(detail::childPath(itemPath, "title"), "must be one line");
        }
        std::string order = "highest";
        r.optional("order", order);
        const auto parsedOrder = parseLeaderboardOrder(order);
        if (!parsedOrder) fail(detail::childPath(itemPath, "order"), "use \"highest\" or \"lowest\" (which score is best)");
        b.order = *parsedOrder;
        std::string format = "number";
        r.optional("format", format);
        const auto parsedFormat = parseScoreFormat(format);
        if (!parsedFormat) fail(detail::childPath(itemPath, "format"), "use \"number\" or \"time\"");
        b.format = *parsedFormat;
        r.finish();
        out.push_back(std::move(b));
    }
    return out;
}

ProjectManifest readManifest(Json& root) {
    std::vector<std::string> warnings; // project migrations do not warn
    detail::checkAndMigrate(root, "px.project", kProjectFormatVersion, kProjectMigrations, "project", warnings);
    ProjectManifest m;
    ObjectReader r(root, "");
    (void)r.find("format");
    r.required("formatVersion", m.formatVersion);
    r.required("id", m.id);
    r.required("name", m.name);
    if (m.name.empty()) fail("name", "must not be empty");
    r.optional("engineVersion", m.engineVersion);
    if (const Json* settings = r.find("settings")) m.settings = readSettings(*settings, "settings");
    if (const Json* input = r.find("input")) m.input = readInput(*input, "input");
    if (const Json* assetList = r.find("assets")) readAssets(*assetList, "assets", m.assets);
    if (const Json* boards = r.find("leaderboards")) m.leaderboards = readLeaderboards(*boards, "leaderboards");
    r.required("startScene", m.startScene);
    const auto* start = m.assets.find(m.startScene);
    if (start == nullptr) {
        fail("startScene", std::format("refers to asset {}, which is not listed in \"assets\"", m.startScene.toString()));
    }
    if (start->type != assets::AssetType::Scene) {
        fail("startScene", std::format("asset '{}' is a {}, not a scene", start->name, assets::assetTypeName(start->type)));
    }
    r.finish();
    return m;
}

} // namespace

std::string_view leaderboardOrderName(LeaderboardOrder order) noexcept {
    return order == LeaderboardOrder::Lowest ? "lowest" : "highest";
}
std::string_view scoreFormatName(ScoreFormat format) noexcept { return format == ScoreFormat::Time ? "time" : "number"; }
std::optional<LeaderboardOrder> parseLeaderboardOrder(std::string_view name) noexcept {
    if (name == "highest") return LeaderboardOrder::Highest;
    if (name == "lowest") return LeaderboardOrder::Lowest;
    return std::nullopt;
}
std::optional<ScoreFormat> parseScoreFormat(std::string_view name) noexcept {
    if (name == "number") return ScoreFormat::Number;
    if (name == "time") return ScoreFormat::Time;
    return std::nullopt;
}
bool isValidLeaderboardId(std::string_view id) noexcept {
    if (id.empty() || id.size() > 32) return false;
    for (const char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}
std::string formatScore(double value, ScoreFormat format) {
    if (format == ScoreFormat::Time) {
        // Hundredths, rounded once, so 59.999 becomes 1:00.00, not 0:60.00.
        const auto hundredths = static_cast<long long>(std::llround(std::max(0.0, std::min(value, 1e12)) * 100.0));
        return std::format("{}:{:02}.{:02}", hundredths / 6000, (hundredths / 100) % 60, hundredths % 100);
    }
    if (value == std::floor(value) && std::abs(value) < 1e15) return std::format("{}", static_cast<long long>(value));
    return std::format("{:.2f}", value);
}

std::string_view gameTypeName(GameType type) noexcept {
    switch (type) {
        case GameType::Platformer: return "platformer";
        case GameType::TopDown: return "topdown";
    }
    return "platformer";
}

std::optional<GameType> parseGameType(std::string_view name) noexcept {
    if (name == "platformer") return GameType::Platformer;
    if (name == "topdown") return GameType::TopDown;
    return std::nullopt;
}

Result<ProjectManifest> parseProjectManifest(std::string_view json, std::string_view sourceName) {
    try {
        Json root = detail::parseJsonText(json);
        return readManifest(root);
    } catch (const detail::FormatError& e) {
        return makeError(ErrorCode::InvalidData, "{}: {}", sourceName, e.what());
    }
}

Result<ProjectManifest> loadProjectManifest(const IFileSystem& fs) {
    auto text = fs.readText(kProjectFileName);
    if (!text) return text.error();
    return parseProjectManifest(text.value(), fs.describe(kProjectFileName));
}

std::string serializeProjectManifest(const ProjectManifest& m) {
    Json root;
    root["format"] = "px.project";
    root["formatVersion"] = kProjectFormatVersion;
    root["id"] = detail::toJson(m.id);
    root["name"] = m.name;
    root["engineVersion"] = version::string;
    root["startScene"] = detail::toJson(m.startScene);

    Json display;
    display["virtualResolution"] = detail::toJson(m.settings.display.virtualResolution);
    display["scaling"] = m.settings.display.scaling == ScalingMode::Integer ? "integer" : "letterbox";
    display["windowScale"] = m.settings.display.windowScale;
    root["settings"] = Json{{"gameType", std::string(gameTypeName(m.settings.gameType))},
                            {"tickRate", m.settings.tickRate},
                            {"display", display},
                            {"scripting", Json{{"apiVersion", m.settings.scripting.apiVersion}}}};

    Json actions = Json::object();
    for (const auto& action : m.input.actions) {
        Json keys = Json::array();
        for (const Key k : action.keys) keys.push_back(std::string(keyName(k)));
        actions[action.name] = std::move(keys);
    }
    root["input"] = Json{{"actions", actions}};
    if (!m.leaderboards.empty()) {
        Json boards = Json::array();
        for (const auto& b : m.leaderboards) {
            boards.push_back(Json{{"id", b.id},
                                  {"title", b.title},
                                  {"order", std::string(leaderboardOrderName(b.order))},
                                  {"format", std::string(scoreFormatName(b.format))}});
        }
        root["leaderboards"] = std::move(boards);
    }

    Json assetList = Json::array();
    for (const auto& entry : m.assets.entries()) {
        Json a;
        a["id"] = detail::toJson(entry.id);
        a["type"] = std::string(assets::assetTypeName(entry.type));
        a["name"] = entry.name;
        a["path"] = entry.path;
        if (entry.type == assets::AssetType::Texture) a["filter"] = std::string(assets::textureFilterName(entry.filter));
        assetList.push_back(std::move(a));
    }
    root["assets"] = std::move(assetList);
    return detail::dumpPretty(root);
}

} // namespace px::project
