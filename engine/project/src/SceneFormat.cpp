#include "px/project/SceneFormat.hpp"

#include "JsonUtil.hpp"

#include "px/core/Log.hpp"
#include "px/scene/ComponentInfo.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <sstream>
#include <type_traits>
#include <unordered_map>

// Scene files are read and written generically from the component
// descriptors (px/scene/ComponentInfo.hpp): each field kind has one reader
// and one writer here. Value ranges, required components, asset types and
// entity references are checked by scene::validateEntity once the whole
// scene is read, so Studio can run the very same checks.
namespace px::project {

using detail::childPath;
using detail::fail;
using detail::Json;
using detail::ObjectReader;
using detail::toJson;
using namespace px::scene;

namespace {

// --- Migrations ---------------------------------------------------------------

// Version 1 -> 2: the built-in Collectible component was removed; collecting
// is now done by scripts. Each Collectible becomes a tag with its `kind`, so
// scripts can still find these entities, and the creator is warned that the
// behaviour itself must now come from a script.
void migrateScene1To2(Json& root, std::vector<std::string>& warnings) {
    auto entities = root.find("entities");
    if (entities == root.end() || !entities->is_array()) return;
    int converted = 0;
    for (auto& entity : *entities) {
        if (!entity.is_object()) continue;
        auto components = entity.find("components");
        if (components == entity.end() || !components->is_object()) continue;
        auto collectible = components->find("Collectible");
        if (collectible == components->end()) continue;
        std::string kind = "coin";
        if (collectible->is_object()) {
            if (auto k = collectible->find("kind"); k != collectible->end() && k->is_string()) kind = k->get<std::string>();
        }
        components->erase("Collectible");
        auto& tags = (*components)["Tags"];
        if (!tags.is_array()) tags = Json::array();
        if (std::find(tags.begin(), tags.end(), Json(kind)) == tags.end()) tags.push_back(kind);
        ++converted;
    }
    if (converted > 0) {
        warnings.push_back(std::format(
            "scene format 1 -> 2: {} Collectible component(s) were converted to tags. Collecting is now done by "
            "scripts; add a Script component (see examples/platformer/scripts/coin.luau)",
            converted));
    }
}

// Version 2 -> 3: entities may have several scripts. The single "Script"
// component becomes a one-element "Scripts" list. Behaviour is unchanged.
// (Version 3 also adds the optional entity "parent" field.)
void migrateScene2To3(Json& root, std::vector<std::string>&) {
    auto entities = root.find("entities");
    if (entities == root.end() || !entities->is_array()) return;
    for (auto& entity : *entities) {
        if (!entity.is_object()) continue;
        auto components = entity.find("components");
        if (components == entity.end() || !components->is_object()) continue;
        auto script = components->find("Script");
        if (script == components->end() || components->contains("Scripts")) continue;
        Json list = Json::array();
        list.push_back(*script);
        components->erase("Script");
        (*components)["Scripts"] = std::move(list);
    }
}

// Scene format migrations: kSceneMigrations[i] upgrades version i+1 to i+2.
constexpr std::array<detail::Migration, 2> kSceneMigrations{&migrateScene1To2, &migrateScene2To3};

// --- Tilemap rows -------------------------------------------------------------
// Each row is a string of whitespace-separated tokens: "." for empty or a
// tile index. All rows must have the same number of tokens.

std::vector<int> parseRow(const std::string& row, const std::string& path) {
    std::vector<int> out;
    std::istringstream in(row);
    std::string token;
    while (in >> token) {
        if (token == ".") {
            out.push_back(-1);
            continue;
        }
        int value = 0;
        for (const char c : token) {
            if (c < '0' || c > '9') fail(path, std::format("'{}' is not a tile index or '.'", token));
            value = value * 10 + (c - '0');
            if (value > 1000000) fail(path, std::format("tile index '{}' is too large", token));
        }
        out.push_back(value);
    }
    return out;
}

std::string formatRow(const Tilemap& map, int y) {
    std::string row;
    for (int x = 0; x < map.width; ++x) {
        if (x > 0) row += ' ';
        const int tile = map.tileAt(x, y);
        row += tile < 0 ? std::string(".") : std::to_string(tile);
    }
    return row;
}

void readTileGrid(Tilemap& m, const Json& rows, const std::string& rowsPath) {
    if (!rows.is_array()) fail(rowsPath, "expected an array of row strings");
    if (rows.size() > static_cast<std::size_t>(kMaxTilemapDimension)) {
        fail(rowsPath, std::format("too many rows; the limit is {}", kMaxTilemapDimension));
    }
    m.height = static_cast<int>(rows.size());
    m.width = 0;
    m.tiles.clear();
    for (std::size_t y = 0; y < rows.size(); ++y) {
        const std::string rowPath = detail::indexPath(rowsPath, y);
        std::string text;
        detail::readValue(rows[y], rowPath, text);
        auto row = parseRow(text, rowPath);
        if (y == 0) {
            m.width = static_cast<int>(row.size());
            if (m.width == 0) fail(rowPath, "row is empty");
            if (m.width > kMaxTilemapDimension) fail(rowPath, std::format("row is wider than {} tiles", kMaxTilemapDimension));
        } else if (static_cast<int>(row.size()) != m.width) {
            fail(rowPath, std::format("has {} tiles, but row 0 has {}; all rows must be the same width", row.size(), m.width));
        }
        m.tiles.insert(m.tiles.end(), row.begin(), row.end());
    }
}

// --- Script lists -------------------------------------------------------------

bool isIdentifier(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    if (!((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z') || name[0] == '_')) return false;
    for (const char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

ScriptValue readScriptValue(const Json& j, const std::string& path) {
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number()) {
        float checked = 0.0f;
        detail::readValue(j, path, checked); // rejects NaN, infinity and out-of-range values
        return j.get<double>();
    }
    if (j.is_string()) {
        std::string text = j.get<std::string>();
        if (text.size() > kMaxScriptStringLength) {
            fail(path, std::format("text is longer than {} characters", kMaxScriptStringLength));
        }
        return text;
    }
    if (j.is_array()) {
        Vec2 v;
        detail::readValue(j, path, v);
        return v;
    }
    fail(path, "script properties must be a number, true/false, a string, or a vector [x, y]");
}

Json scriptValueToJson(const ScriptValue& value) {
    return std::visit(
        [](const auto& v) -> Json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, double>) {
                if (std::floor(v) == v && std::abs(v) < 1.0e15) return static_cast<long long>(v);
                return v;
            } else if constexpr (std::is_same_v<T, Vec2>) {
                return toJson(v);
            } else {
                return v;
            }
        },
        value);
}

void readScriptList(Scripts& scripts, const Json& j, const std::string& path, const FieldInfo& f) {
    if (!j.is_array()) fail(path, "expected an array of scripts, e.g. [{\"script\": \"<script asset id>\"}]");
    if (j.size() > f.maxCount) fail(path, std::format("too many scripts; the limit is {}", f.maxCount));
    scripts.items.clear();
    for (std::size_t i = 0; i < j.size(); ++i) {
        const std::string itemPath = detail::indexPath(path, i);
        AttachedScript script;
        ObjectReader r(j[i], itemPath);
        r.required("script", script.script);
        if (const Json* props = r.find("properties")) {
            const std::string propsPath = childPath(itemPath, "properties");
            if (!props->is_object()) fail(propsPath, "expected an object mapping property names to values");
            if (props->size() > kMaxScriptProperties) {
                fail(propsPath, std::format("too many properties; the limit is {}", kMaxScriptProperties));
            }
            for (const auto& [name, value] : props->items()) {
                const std::string propPath = childPath(propsPath, name);
                if (!isIdentifier(name)) fail(propPath, "property names must be identifiers (letters, digits, '_')");
                script.properties.push_back({name, readScriptValue(value, propPath)});
            }
        }
        r.finish();
        scripts.items.push_back(std::move(script));
    }
}

Json writeScriptList(const Scripts& scripts) {
    Json list = Json::array();
    for (const AttachedScript& s : scripts.items) {
        Json props = Json::object();
        for (const auto& p : s.properties) props[p.name] = scriptValueToJson(p.value);
        list.push_back(Json{{"script", toJson(s.script)}, {"properties", props}});
    }
    return list;
}

// --- Generic fields -------------------------------------------------------------

void readField(const FieldInfo& f, void* data, const Json& j, const std::string& path) {
    switch (f.kind) {
        case FieldKind::Float: detail::readValue(j, path, f.ref<float>(data)); break;
        case FieldKind::Int: detail::readValue(j, path, f.ref<int>(data)); break;
        case FieldKind::Bool: detail::readValue(j, path, f.ref<bool>(data)); break;
        case FieldKind::String: {
            std::string& text = f.ref<std::string>(data);
            detail::readValue(j, path, text);
            if (text.size() > f.maxLength) fail(path, std::format("is longer than {} characters", f.maxLength));
            break;
        }
        case FieldKind::Vec2: detail::readValue(j, path, f.ref<Vec2>(data)); break;
        case FieldKind::Rect: detail::readValue(j, path, f.ref<Rect>(data)); break;
        case FieldKind::Color: detail::readValue(j, path, f.ref<Color>(data)); break;
        case FieldKind::Asset: detail::readValue(j, path, f.ref<assets::AssetId>(data)); break;
        case FieldKind::EntityRef: detail::readValue(j, path, f.ref<Guid>(data)); break;
        case FieldKind::StringList: {
            if (!j.is_array()) fail(path, "expected an array of strings, e.g. [\"coin\"]");
            if (j.size() > f.maxCount) fail(path, std::format("too many entries; the limit is {}", f.maxCount));
            auto& list = f.ref<std::vector<std::string>>(data);
            list.clear();
            for (std::size_t i = 0; i < j.size(); ++i) {
                std::string text;
                detail::readValue(j[i], detail::indexPath(path, i), text);
                if (text.size() < f.minLength || text.size() > f.maxLength) {
                    fail(detail::indexPath(path, i),
                         std::format("entries must be {} to {} characters", f.minLength, f.maxLength));
                }
                if (std::find(list.begin(), list.end(), text) == list.end()) list.push_back(std::move(text));
            }
            break;
        }
        case FieldKind::RectList: {
            if (!j.is_array() || j.empty()) fail(path, "expected a non-empty array of [x, y, w, h] rectangles");
            if (j.size() > f.maxCount) fail(path, std::format("too many entries; the limit is {}", f.maxCount));
            auto& list = f.ref<std::vector<Rect>>(data);
            list.clear();
            for (std::size_t i = 0; i < j.size(); ++i) {
                Rect r;
                detail::readValue(j[i], detail::indexPath(path, i), r);
                list.push_back(r);
            }
            break;
        }
        case FieldKind::TileGrid: readTileGrid(*static_cast<Tilemap*>(data), j, path); break;
        case FieldKind::ScriptList: readScriptList(*static_cast<Scripts*>(data), j, path, f); break;
    }
}

Json writeField(const FieldInfo& f, const void* data) {
    switch (f.kind) {
        case FieldKind::Float: return toJson(f.ref<float>(data));
        case FieldKind::Int: return f.ref<int>(data);
        case FieldKind::Bool: return f.ref<bool>(data);
        case FieldKind::String: return f.ref<std::string>(data);
        case FieldKind::Vec2: return toJson(f.ref<Vec2>(data));
        case FieldKind::Rect: return toJson(f.ref<Rect>(data));
        case FieldKind::Color: return toJson(f.ref<Color>(data));
        case FieldKind::Asset: return toJson(f.ref<assets::AssetId>(data));
        case FieldKind::EntityRef: return toJson(f.ref<Guid>(data));
        case FieldKind::StringList: {
            Json list = Json::array();
            for (const auto& s : f.ref<std::vector<std::string>>(data)) list.push_back(s);
            return list;
        }
        case FieldKind::RectList: {
            Json list = Json::array();
            for (const auto& r : f.ref<std::vector<Rect>>(data)) list.push_back(toJson(r));
            return list;
        }
        case FieldKind::TileGrid: {
            const auto& map = *static_cast<const Tilemap*>(data);
            Json rows = Json::array();
            for (int y = 0; y < map.height; ++y) rows.push_back(formatRow(map, y));
            return rows;
        }
        case FieldKind::ScriptList: return writeScriptList(*static_cast<const Scripts*>(data));
    }
    return nullptr;
}

void readComponent(Scene& scene, Entity e, const ComponentInfo& c, const Json& j, const std::string& path) {
    c.add(scene, e);
    void* data = c.get(scene, e);
    if ((c.flags & kComponentInlineField) != 0) {
        readField(c.fields.front(), data, j, path);
        return;
    }
    ObjectReader r(j, path);
    for (const FieldInfo& f : c.fields) {
        if (f.isRuntime()) continue;
        if (f.isRequired()) {
            readField(f, data, r.require(f.name), childPath(path, f.name));
        } else if (const Json* v = r.find(f.name)) {
            readField(f, data, *v, childPath(path, f.name));
        }
    }
    r.finish();
}

Json writeComponent(const ComponentInfo& c, const void* data) {
    if ((c.flags & kComponentInlineField) != 0) return writeField(c.fields.front(), data);
    Json out = Json::object();
    for (const FieldInfo& f : c.fields) {
        if (f.isRuntime()) continue;
        if ((f.flags & kFieldOptional) != 0 && f.kind == FieldKind::EntityRef && f.ref<Guid>(data).isNil()) continue;
        out[std::string(f.name)] = writeField(f, data);
    }
    return out;
}

// --- Entities -------------------------------------------------------------------

struct EntityRecord {
    Entity entity;
    Guid parentId;
    std::string path; // "entities[3] ('Player')"
};

std::string problemPath(const std::string& entityPath, const ComponentProblem& p) {
    std::string path = childPath(entityPath, "components");
    if (p.component.empty()) return entityPath;
    path = childPath(path, p.component);
    const ComponentInfo* c = findComponentType(p.component);
    if (!p.field.empty() && c != nullptr && (c->flags & kComponentInlineField) == 0) path = childPath(path, p.field);
    return path;
}

// Reads an "entities" array into `scene` (all as roots first, then the
// hierarchy is applied) and validates every entity.
void readEntities(Scene& scene, const Json& entities, const assets::AssetRegistry* assets) {
    if (!entities.is_array()) fail("entities", "expected an array of entities");
    if (entities.size() > kMaxEntitiesPerScene) {
        fail("entities", std::format("too many entities ({}); the limit is {}", entities.size(), kMaxEntitiesPerScene));
    }
    std::vector<EntityRecord> records;
    records.reserve(entities.size());
    for (std::size_t i = 0; i < entities.size(); ++i) {
        std::string path = detail::indexPath("entities", i);
        ObjectReader er(entities[i], path);
        Guid id;
        er.required("id", id);
        std::string entityName;
        er.optional("name", entityName);
        if (!entityName.empty()) path += std::format(" ('{}')", entityName);
        Guid parentId;
        er.optional("parent", parentId);

        auto created = scene.createEntityWithId(id, entityName);
        if (!created) fail(path, created.error().message);
        const Entity entity = created.value();

        if (const Json* components = er.find("components")) {
            const std::string componentsPath = childPath(path, "components");
            if (!components->is_object()) fail(componentsPath, "expected an object of components");
            for (const auto& [componentName, data] : components->items()) {
                const ComponentInfo* info = findComponentType(componentName);
                if (info == nullptr) {
                    fail(componentsPath, std::format("unknown component '{}'{}", componentName,
                                                     detail::spellingSuggestion(componentName, componentTypeNames())));
                }
                readComponent(scene, entity, *info, data, childPath(componentsPath, componentName));
            }
        }
        er.finish();
        records.push_back({entity, parentId, std::move(path)});
    }

    // Hierarchy, once every entity exists (parents may be listed after children).
    std::vector<std::pair<Entity, Entity>> links;
    for (const EntityRecord& rec : records) {
        if (rec.parentId.isNil()) continue;
        const Entity parent = scene.findById(rec.parentId);
        if (parent.isNull()) {
            fail(childPath(rec.path, "parent"),
                 std::format("no entity with id {} exists in this scene", rec.parentId.toString()));
        }
        links.emplace_back(rec.entity, parent);
    }
    if (!links.empty()) {
        if (auto status = scene.setParents(links); !status) fail("entities", status.error().message);
    }

    // Values, required components, asset references and entity references.
    for (const EntityRecord& rec : records) {
        const auto problems = validateEntity(scene, rec.entity, assets);
        if (!problems.empty()) fail(problemPath(rec.path, problems.front()), problems.front().message);
    }
}

Json writeEntity(const Scene& scene, Entity e, bool writeParent) {
    const auto* info = scene.tryGet<EntityInfo>(e);
    Json entity;
    entity["id"] = toJson(info->id);
    entity["name"] = info->name;
    if (writeParent) {
        const Entity parent = scene.parent(e);
        if (!parent.isNull()) entity["parent"] = toJson(scene.tryGet<EntityInfo>(parent)->id);
    }
    Json components = Json::object();
    forEachComponent(scene, e, [&](const ComponentInfo& c, const void* data) {
        components[std::string(c.name)] = writeComponent(c, data);
    });
    entity["components"] = std::move(components);
    return entity;
}

// --- Scene --------------------------------------------------------------------

Scene readScene(Json& root, const assets::AssetRegistry* assets, std::vector<std::string>& warnings) {
    detail::checkAndMigrate(root, "px.scene", kSceneFormatVersion, kSceneMigrations, "scene", warnings);
    Scene scene;
    ObjectReader r(root, "");
    (void)r.find("format");
    (void)r.find("formatVersion");
    std::string name;
    r.optional("name", name);
    scene.setName(name);

    if (const Json* settings = r.find("settings")) {
        ObjectReader sr(*settings, "settings");
        sr.optional("backgroundColor", scene.settings().backgroundColor);
        float killPlane = 0.0f;
        if (sr.optional("killPlaneY", killPlane)) scene.settings().killPlaneY = killPlane;
        sr.finish();
    }
    readEntities(scene, r.require("entities"), assets);
    r.finish();
    return scene;
}

constexpr int kEntitiesFormatVersion = 1;

} // namespace

Result<Scene> parseScene(std::string_view json, std::string_view sourceName, const assets::AssetRegistry* assets,
                         std::vector<std::string>* warnings) {
    try {
        Json root = detail::parseJsonText(json);
        std::vector<std::string> migrationWarnings;
        Scene scene = readScene(root, assets, migrationWarnings);
        for (const auto& w : migrationWarnings) {
            if (warnings != nullptr) {
                warnings->push_back(std::format("{}: {}", sourceName, w));
            } else {
                log::warn("{}: {}", sourceName, w);
            }
        }
        return scene;
    } catch (const detail::FormatError& e) {
        return makeError(ErrorCode::InvalidData, "{}: {}", sourceName, e.what());
    }
}

Result<Scene> loadScene(const IFileSystem& fs, const ProjectManifest& project, assets::AssetId sceneId) {
    const auto* entry = project.assets.find(sceneId);
    if (entry == nullptr) {
        return makeError(ErrorCode::NotFound, "Scene {} is not listed in the project's assets", sceneId.toString());
    }
    if (entry->type != assets::AssetType::Scene) {
        return makeError(ErrorCode::InvalidArgument, "Asset '{}' is not a scene", entry->name);
    }
    auto text = fs.readText(entry->path);
    if (!text) return text.error();
    return parseScene(text.value(), fs.describe(entry->path), &project.assets);
}

std::string serializeScene(const Scene& scene) {
    Json root;
    root["format"] = "px.scene";
    root["formatVersion"] = kSceneFormatVersion;
    root["name"] = scene.name();
    Json settings;
    settings["backgroundColor"] = toJson(scene.settings().backgroundColor);
    if (scene.settings().killPlaneY) settings["killPlaneY"] = toJson(*scene.settings().killPlaneY);
    root["settings"] = std::move(settings);

    Json entities = Json::array();
    for (const Entity e : scene.entities()) entities.push_back(writeEntity(scene, e, true));
    root["entities"] = std::move(entities);
    return detail::dumpPretty(root);
}

std::string serializeEntities(const Scene& scene, std::span<const Entity> roots) {
    Json root;
    root["format"] = "px.entities";
    root["formatVersion"] = kEntitiesFormatVersion;
    root["sceneFormatVersion"] = kSceneFormatVersion;
    Json entities = Json::array();
    std::vector<Entity> written;
    for (const Entity r : roots) {
        if (!scene.isAlive(r)) continue;
        // Skip roots that are inside another listed root's subtree.
        bool nested = false;
        for (const Entity other : roots) {
            if (other != r && scene.isAncestor(other, r)) nested = true;
        }
        if (nested) continue;
        for (const Entity e : scene.subtree(r)) {
            Json entity = writeEntity(scene, e, e != r);
            entities.push_back(std::move(entity));
        }
    }
    root["entities"] = std::move(entities);
    return detail::dumpPretty(root);
}

Result<std::vector<Entity>> pasteEntities(Scene& scene, std::string_view json, const assets::AssetRegistry* assets,
                                          Entity parent) {
    if (!parent.isNull() && !scene.isAlive(parent)) {
        return makeError(ErrorCode::NotFound, "Cannot paste: the target entity does not exist");
    }
    Scene clipboard;
    try {
        Json root = detail::parseJsonText(json);
        ObjectReader r(root, "");
        std::string format;
        r.required("format", format);
        if (format != "px.entities") fail("format", "expected \"px.entities\" (copied entities)");
        int version = 0;
        r.required("formatVersion", version);
        if (version != kEntitiesFormatVersion) fail("formatVersion", "unsupported version");
        int sceneVersion = 0;
        r.required("sceneFormatVersion", sceneVersion);
        if (sceneVersion != kSceneFormatVersion) {
            fail("sceneFormatVersion", "the entities were copied from a different engine version");
        }
        Json entities = r.require("entities");
        r.finish();
        if (!entities.is_array()) fail("entities", "expected an array of entities");

        // References to entities outside the copied set cannot be validated
        // inside the clipboard; take them out, read, then put back those that
        // exist in the target scene.
        std::vector<Guid> ids;
        for (const auto& e : entities) {
            if (e.is_object() && e.contains("id") && e["id"].is_string()) {
                if (auto g = Guid::parse(e["id"].get<std::string>())) ids.push_back(*g);
            }
        }
        struct External {
            Guid entity;
            const ComponentInfo* component;
            const FieldInfo* field;
            Guid target;
        };
        std::vector<External> external;
        for (auto& e : entities) {
            if (!e.is_object() || !e.contains("components") || !e["components"].is_object()) continue;
            if (!e.contains("id") || !e["id"].is_string()) continue;
            const auto entityId = Guid::parse(e["id"].get<std::string>());
            if (!entityId) continue;
            for (auto& [componentName, data] : e["components"].items()) {
                const ComponentInfo* info = findComponentType(componentName);
                if (info == nullptr || !data.is_object()) continue;
                for (const FieldInfo& f : info->fields) {
                    const std::string key(f.name);
                    if (f.kind != FieldKind::EntityRef || !data.contains(key) || !data[key].is_string()) continue;
                    const auto target = Guid::parse(data[key].get<std::string>());
                    if (!target || std::find(ids.begin(), ids.end(), *target) != ids.end()) continue;
                    external.push_back({*entityId, info, &f, *target});
                    data.erase(key);
                }
            }
        }
        readEntities(clipboard, entities, assets);
        for (const External& x : external) {
            if (scene.findById(x.target).isNull()) continue;
            if (void* data = x.component->get(clipboard, clipboard.findById(x.entity))) {
                x.field->ref<Guid>(data) = x.target;
            }
        }
    } catch (const detail::FormatError& e) {
        return makeError(ErrorCode::InvalidData, "Cannot paste: {}", e.what());
    }

    std::vector<Entity> pasted;
    std::vector<std::pair<Guid, Guid>> remap;
    for (const Entity original : clipboard.children(kNullEntity)) {
        const Entity copy = scene.copySubtree(clipboard, original, parent);
        if (copy.isNull()) return makeError(ErrorCode::LimitExceeded, "Cannot paste: the hierarchy would be too deep");
        const auto source = clipboard.subtree(original);
        const auto target = scene.subtree(copy);
        for (std::size_t i = 0; i < source.size() && i < target.size(); ++i) {
            remap.emplace_back(clipboard.tryGet<EntityInfo>(source[i])->id, scene.tryGet<EntityInfo>(target[i])->id);
        }
        pasted.push_back(copy);
    }
    std::vector<Entity> all;
    for (const Entity p : pasted) {
        const auto sub = scene.subtree(p);
        all.insert(all.end(), sub.begin(), sub.end());
    }
    remapEntityReferences(scene, all, remap);
    return pasted;
}

} // namespace px::project
