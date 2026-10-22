#include "px/project/SceneFormat.hpp"

#include "JsonUtil.hpp"

#include <array>
#include <format>
#include <sstream>

namespace px::project {

using detail::childPath;
using detail::fail;
using detail::Json;
using detail::ObjectReader;
using detail::toJson;
using namespace px::scene;

namespace {

// Scene format migrations: kSceneMigrations[i] upgrades version i+1 to i+2.
constexpr std::array<detail::Migration, 0> kSceneMigrations{};

struct ReadContext {
    const assets::AssetRegistry* assets = nullptr;
};

void checkTexture(const ReadContext& ctx, const assets::AssetId& id, const std::string& path) {
    if (id.isNil()) fail(path, "a texture is required");
    if (ctx.assets == nullptr) return;
    const auto* entry = ctx.assets->find(id);
    if (entry == nullptr) fail(path, std::format("refers to asset {}, which is not in the project", id.toString()));
    if (entry->type != assets::AssetType::Texture) {
        fail(path, std::format("asset '{}' is a {}, not a texture", entry->name, assets::assetTypeName(entry->type)));
    }
}

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

// --- Component codecs ---------------------------------------------------------

using ReadFn = void (*)(Scene&, Entity, const Json&, const std::string& path, const ReadContext&);
using WriteFn = bool (*)(const Scene&, Entity, Json& out);

struct ComponentCodec {
    std::string_view name;
    ReadFn read;
    WriteFn write;
};

void readTransform(Scene& s, Entity e, const Json& j, const std::string& path, const ReadContext&) {
    Transform t;
    ObjectReader r(j, path);
    r.optional("position", t.position);
    r.optional("rotation", t.rotation);
    r.optional("scale", t.scale);
    r.finish();
    s.set(e, t);
}
bool writeTransform(const Scene& s, Entity e, Json& out) {
    const auto* t = s.tryGet<Transform>(e);
    if (!t) return false;
    out = Json{{"position", toJson(t->position)}, {"rotation", toJson(t->rotation)}, {"scale", toJson(t->scale)}};
    return true;
}

void readSprite(Scene& s, Entity e, const Json& j, const std::string& path, const ReadContext& ctx) {
    Sprite sp;
    ObjectReader r(j, path);
    r.required("texture", sp.texture);
    checkTexture(ctx, sp.texture, childPath(path, "texture"));
    r.optional("source", sp.source);
    r.optional("size", sp.size);
    r.optional("pivot", sp.pivot);
    r.optional("tint", sp.tint);
    r.optional("layer", sp.layer);
    r.optional("flipX", sp.flipX);
    r.optional("flipY", sp.flipY);
    r.optional("visible", sp.visible);
    r.finish();
    s.set(e, sp);
}
bool writeSprite(const Scene& s, Entity e, Json& out) {
    const auto* sp = s.tryGet<Sprite>(e);
    if (!sp) return false;
    out = Json{{"texture", toJson(sp->texture)}, {"source", toJson(sp->source)}, {"size", toJson(sp->size)},
               {"pivot", toJson(sp->pivot)},     {"tint", toJson(sp->tint)},     {"layer", sp->layer},
               {"flipX", sp->flipX},             {"flipY", sp->flipY},           {"visible", sp->visible}};
    return true;
}

void readSpriteAnimation(Scene& s, Entity e, const Json& j, const std::string& path, const ReadContext&) {
    SpriteAnimation a;
    ObjectReader r(j, path);
    const Json& frames = r.require("frames");
    const std::string framesPath = childPath(path, "frames");
    if (!frames.is_array() || frames.empty()) fail(framesPath, "expected a non-empty array of [x, y, w, h] rectangles");
    if (frames.size() > kMaxAnimationFrames) fail(framesPath, std::format("too many frames; the limit is {}", kMaxAnimationFrames));
    for (std::size_t i = 0; i < frames.size(); ++i) {
        Rect frame;
        detail::readValue(frames[i], detail::indexPath(framesPath, i), frame);
        a.frames.push_back(frame);
    }
    r.optional("framesPerSecond", a.framesPerSecond);
    if (a.framesPerSecond <= 0.0f || a.framesPerSecond > 240.0f) {
        fail(childPath(path, "framesPerSecond"), "must be greater than 0 and at most 240");
    }
    r.optional("loop", a.loop);
    r.optional("playing", a.playing);
    r.finish();
    s.set(e, a);
}
bool writeSpriteAnimation(const Scene& s, Entity e, Json& out) {
    const auto* a = s.tryGet<SpriteAnimation>(e);
    if (!a) return false;
    Json frames = Json::array();
    for (const auto& f : a->frames) frames.push_back(toJson(f));
    out = Json{{"frames", frames},
               {"framesPerSecond", toJson(a->framesPerSecond)},
               {"loop", a->loop},
               {"playing", a->playing}};
    return true;
}

void readTilemap(Scene& s, Entity e, const Json& j, const std::string& path, const ReadContext& ctx) {
    Tilemap m;
    ObjectReader r(j, path);
    r.required("tileset", m.tileset);
    checkTexture(ctx, m.tileset, childPath(path, "tileset"));
    r.optional("tileSize", m.tileSize);
    if (m.tileSize.x <= 0.0f || m.tileSize.y <= 0.0f) fail(childPath(path, "tileSize"), "must be positive");
    r.optional("tilesetColumns", m.tilesetColumns);
    if (m.tilesetColumns < 1) fail(childPath(path, "tilesetColumns"), "must be at least 1");
    r.optional("solid", m.solid);
    r.optional("layer", m.layer);
    r.optional("tint", m.tint);

    const Json& rows = r.require("rows");
    const std::string rowsPath = childPath(path, "rows");
    if (!rows.is_array()) fail(rowsPath, "expected an array of row strings");
    if (rows.size() > static_cast<std::size_t>(kMaxTilemapDimension)) {
        fail(rowsPath, std::format("too many rows; the limit is {}", kMaxTilemapDimension));
    }
    m.height = static_cast<int>(rows.size());
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
    r.finish();
    s.set(e, std::move(m));
}
bool writeTilemap(const Scene& s, Entity e, Json& out) {
    const auto* m = s.tryGet<Tilemap>(e);
    if (!m) return false;
    Json rows = Json::array();
    for (int y = 0; y < m->height; ++y) rows.push_back(formatRow(*m, y));
    out = Json{{"tileset", toJson(m->tileset)}, {"tileSize", toJson(m->tileSize)},
               {"tilesetColumns", m->tilesetColumns}, {"solid", m->solid},
               {"layer", m->layer}, {"tint", toJson(m->tint)}, {"rows", rows}};
    return true;
}

void readBoxCollider(Scene& s, Entity e, const Json& j, const std::string& path, const ReadContext&) {
    BoxCollider c;
    ObjectReader r(j, path);
    r.optional("size", c.size);
    if (c.size.x <= 0.0f || c.size.y <= 0.0f) fail(childPath(path, "size"), "must be positive");
    r.optional("offset", c.offset);
    r.optional("isTrigger", c.isTrigger);
    r.finish();
    s.set(e, c);
}
bool writeBoxCollider(const Scene& s, Entity e, Json& out) {
    const auto* c = s.tryGet<BoxCollider>(e);
    if (!c) return false;
    out = Json{{"size", toJson(c->size)}, {"offset", toJson(c->offset)}, {"isTrigger", c->isTrigger}};
    return true;
}

void readKinematicBody(Scene& s, Entity e, const Json& j, const std::string& path, const ReadContext&) {
    ObjectReader r(j, path);
    r.finish();
    s.set(e, KinematicBody{});
}
bool writeKinematicBody(const Scene& s, Entity e, Json& out) {
    if (!s.tryGet<KinematicBody>(e)) return false;
    out = Json::object();
    return true;
}

void readPlatformerController(Scene& s, Entity e, const Json& j, const std::string& path, const ReadContext&) {
    PlatformerController c;
    ObjectReader r(j, path);
    r.optional("maxSpeed", c.maxSpeed);
    r.optional("acceleration", c.acceleration);
    r.optional("deceleration", c.deceleration);
    r.optional("airAcceleration", c.airAcceleration);
    r.optional("jumpSpeed", c.jumpSpeed);
    r.optional("gravity", c.gravity);
    r.optional("maxFallSpeed", c.maxFallSpeed);
    r.optional("jumpCutMultiplier", c.jumpCutMultiplier);
    r.optional("coyoteTime", c.coyoteTime);
    r.optional("jumpBufferTime", c.jumpBufferTime);
    r.finish();
    for (const auto& [name, value] : {std::pair{"maxSpeed", c.maxSpeed}, std::pair{"acceleration", c.acceleration},
                                     std::pair{"deceleration", c.deceleration},
                                     std::pair{"airAcceleration", c.airAcceleration},
                                     std::pair{"jumpSpeed", c.jumpSpeed}, std::pair{"gravity", c.gravity},
                                     std::pair{"maxFallSpeed", c.maxFallSpeed},
                                     std::pair{"coyoteTime", c.coyoteTime},
                                     std::pair{"jumpBufferTime", c.jumpBufferTime}}) {
        if (value < 0.0f) fail(childPath(path, name), "may not be negative");
    }
    if (c.jumpCutMultiplier < 0.0f || c.jumpCutMultiplier > 1.0f) {
        fail(childPath(path, "jumpCutMultiplier"), "must be between 0 and 1");
    }
    s.set(e, c);
}
bool writePlatformerController(const Scene& s, Entity e, Json& out) {
    const auto* c = s.tryGet<PlatformerController>(e);
    if (!c) return false;
    out = Json{{"maxSpeed", toJson(c->maxSpeed)},
               {"acceleration", toJson(c->acceleration)},
               {"deceleration", toJson(c->deceleration)},
               {"airAcceleration", toJson(c->airAcceleration)},
               {"jumpSpeed", toJson(c->jumpSpeed)},
               {"gravity", toJson(c->gravity)},
               {"maxFallSpeed", toJson(c->maxFallSpeed)},
               {"jumpCutMultiplier", toJson(c->jumpCutMultiplier)},
               {"coyoteTime", toJson(c->coyoteTime)},
               {"jumpBufferTime", toJson(c->jumpBufferTime)}};
    return true;
}

void readCollectible(Scene& s, Entity e, const Json& j, const std::string& path, const ReadContext&) {
    Collectible c;
    ObjectReader r(j, path);
    r.optional("kind", c.kind);
    r.optional("value", c.value);
    r.finish();
    s.set(e, c);
}
bool writeCollectible(const Scene& s, Entity e, Json& out) {
    const auto* c = s.tryGet<Collectible>(e);
    if (!c) return false;
    out = Json{{"kind", c->kind}, {"value", c->value}};
    return true;
}

void readPlayer(Scene& s, Entity e, const Json& j, const std::string& path, const ReadContext&) {
    ObjectReader r(j, path);
    r.finish();
    s.set(e, Player{});
}
bool writePlayer(const Scene& s, Entity e, Json& out) {
    if (!s.tryGet<Player>(e)) return false;
    out = Json::object();
    return true;
}

void readCamera(Scene& s, Entity e, const Json& j, const std::string& path, const ReadContext&) {
    Camera c;
    ObjectReader r(j, path);
    r.optional("follow", c.follow);
    r.optional("zoom", c.zoom);
    if (c.zoom <= 0.0f || c.zoom > 64.0f) fail(childPath(path, "zoom"), "must be greater than 0 and at most 64");
    r.optional("clampToBounds", c.clampToBounds);
    r.finish();
    s.set(e, c);
}
bool writeCamera(const Scene& s, Entity e, Json& out) {
    const auto* c = s.tryGet<Camera>(e);
    if (!c) return false;
    out = Json::object();
    if (!c->follow.isNil()) out["follow"] = toJson(c->follow);
    out["zoom"] = toJson(c->zoom);
    out["clampToBounds"] = c->clampToBounds;
    return true;
}

// Order here is the order components are written in files.
constexpr std::array<ComponentCodec, 10> kCodecs{{
    {"Transform", readTransform, writeTransform},
    {"Sprite", readSprite, writeSprite},
    {"SpriteAnimation", readSpriteAnimation, writeSpriteAnimation},
    {"Tilemap", readTilemap, writeTilemap},
    {"BoxCollider", readBoxCollider, writeBoxCollider},
    {"KinematicBody", readKinematicBody, writeKinematicBody},
    {"PlatformerController", readPlatformerController, writePlatformerController},
    {"Collectible", readCollectible, writeCollectible},
    {"Player", readPlayer, writePlayer},
    {"Camera", readCamera, writeCamera},
}};

const ComponentCodec* findCodec(std::string_view name) {
    for (const auto& codec : kCodecs) {
        if (codec.name == name) return &codec;
    }
    return nullptr;
}

std::vector<std::string> codecNames() {
    std::vector<std::string> names;
    for (const auto& codec : kCodecs) names.emplace_back(codec.name);
    return names;
}

// --- Scene --------------------------------------------------------------------

Scene readScene(Json& root, const ReadContext& ctx) {
    detail::checkAndMigrate(root, "px.scene", kSceneFormatVersion, kSceneMigrations, "scene");
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

    const Json& entities = r.require("entities");
    if (!entities.is_array()) fail("entities", "expected an array of entities");
    if (entities.size() > kMaxEntitiesPerScene) {
        fail("entities", std::format("too many entities ({}); the limit is {}", entities.size(), kMaxEntitiesPerScene));
    }
    for (std::size_t i = 0; i < entities.size(); ++i) {
        std::string path = detail::indexPath("entities", i);
        ObjectReader er(entities[i], path);
        Guid id;
        er.required("id", id);
        std::string entityName;
        er.optional("name", entityName);
        if (!entityName.empty()) path += std::format(" ('{}')", entityName);

        auto created = scene.createEntityWithId(id, entityName);
        if (!created) fail(path, created.error().message);
        const Entity entity = created.value();

        if (const Json* components = er.find("components")) {
            const std::string componentsPath = childPath(path, "components");
            if (!components->is_object()) fail(componentsPath, "expected an object of components");
            for (const auto& [componentName, data] : components->items()) {
                const ComponentCodec* codec = findCodec(componentName);
                if (codec == nullptr) {
                    fail(componentsPath, std::format("unknown component '{}'{}", componentName,
                                                     detail::spellingSuggestion(componentName, codecNames())));
                }
                codec->read(scene, entity, data, childPath(componentsPath, componentName), ctx);
            }
        }
        er.finish();
    }
    r.finish();

    // Cross-entity references are checked once every entity exists.
    for (std::size_t i = 0; i < scene.entities().size(); ++i) {
        const Entity e = scene.entities()[i];
        if (const auto* cam = scene.tryGet<Camera>(e); cam && !cam->follow.isNil()) {
            if (scene.findById(cam->follow).isNull()) {
                fail(std::format("entities[{}].components.Camera.follow", i),
                     std::format("no entity with id {} exists in this scene", cam->follow.toString()));
            }
        }
        if (scene.tryGet<PlatformerController>(e) &&
            (!scene.tryGet<Transform>(e) || !scene.tryGet<BoxCollider>(e) || !scene.tryGet<KinematicBody>(e))) {
            fail(std::format("entities[{}].components.PlatformerController", i),
                 "requires Transform, BoxCollider and KinematicBody on the same entity");
        }
    }
    return scene;
}

} // namespace

Result<Scene> parseScene(std::string_view json, std::string_view sourceName, const assets::AssetRegistry* assets) {
    try {
        Json root = detail::parseJsonText(json);
        return readScene(root, ReadContext{assets});
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
    for (const Entity e : scene.entities()) {
        const auto* info = scene.tryGet<EntityInfo>(e);
        Json entity;
        entity["id"] = toJson(info->id);
        entity["name"] = info->name;
        Json components = Json::object();
        for (const auto& codec : kCodecs) {
            Json data;
            if (codec.write(scene, e, data)) components[std::string(codec.name)] = std::move(data);
        }
        entity["components"] = std::move(components);
        entities.push_back(std::move(entity));
    }
    root["entities"] = std::move(entities);
    return detail::dumpPretty(root);
}

} // namespace px::project
