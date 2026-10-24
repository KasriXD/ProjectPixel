#include "px/scene/ComponentInfo.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <utility>

namespace px::scene {

namespace {

// --- Descriptor builders -----------------------------------------------------------------------

template <class T>
struct MemberTraits;
template <class C, class M>
struct MemberTraits<M C::*> {
    using Class = C;
    using Member = M;
};

template <class M>
constexpr FieldKind kindOf() {
    if constexpr (std::is_same_v<M, float>) return FieldKind::Float;
    else if constexpr (std::is_same_v<M, int>) return FieldKind::Int;
    else if constexpr (std::is_same_v<M, bool>) return FieldKind::Bool;
    else if constexpr (std::is_same_v<M, std::string>) return FieldKind::String;
    else if constexpr (std::is_same_v<M, Vec2>) return FieldKind::Vec2;
    else if constexpr (std::is_same_v<M, Rect>) return FieldKind::Rect;
    else if constexpr (std::is_same_v<M, Color>) return FieldKind::Color;
    else if constexpr (std::is_same_v<M, assets::AssetId>) return FieldKind::Asset;
    else if constexpr (std::is_same_v<M, Guid>) return FieldKind::EntityRef;
    else if constexpr (std::is_same_v<M, std::vector<std::string>>) return FieldKind::StringList;
    else if constexpr (std::is_same_v<M, std::vector<Rect>>) return FieldKind::RectList;
    else static_assert(sizeof(M) == 0, "no FieldKind for this member type");
}

template <auto Member>
void* accessMember(void* component) {
    using C = typename MemberTraits<decltype(Member)>::Class;
    return &(static_cast<C*>(component)->*Member);
}

void* accessWhole(void* component) { return component; }

template <auto Member>
FieldInfo field(std::string_view name, std::string_view description) {
    using M = typename MemberTraits<decltype(Member)>::Member;
    FieldInfo f;
    f.name = name;
    f.description = description;
    f.kind = kindOf<M>();
    f.access = &accessMember<Member>;
    if constexpr (std::is_same_v<M, int>) f.step = 1.0f;
    return f;
}

// A field whose value is the whole component (structured kinds).
FieldInfo wholeField(std::string_view name, FieldKind kind, std::string_view description) {
    FieldInfo f;
    f.name = name;
    f.description = description;
    f.kind = kind;
    f.access = &accessWhole;
    return f;
}

template <class T>
ComponentInfo component(std::string_view name, std::string_view category, std::string_view description,
                        std::span<const FieldInfo> fields, std::span<const std::string_view> required = {},
                        std::uint32_t flags = kComponentNone) {
    ComponentInfo c;
    c.name = name;
    c.category = category;
    c.description = description;
    c.fields = fields;
    c.requiredComponents = required;
    c.flags = flags;
    c.has = [](const Scene& s, Entity e) { return s.tryGet<T>(e) != nullptr; };
    c.get = [](Scene& s, Entity e) -> void* { return s.tryGet<T>(e); };
    c.getConst = [](const Scene& s, Entity e) -> const void* { return s.tryGet<T>(e); };
    c.add = [](Scene& s, Entity e) { s.set<T>(e, T{}); };
    c.remove = [](Scene& s, Entity e) { s.registry().remove<T>(e); };
    return c;
}

constexpr double kInf = kUnbounded;

// --- Tables -------------------------------------------------------------------------------------
// Field order is the order fields are written to files. Descriptions are
// shown as Studio tooltips and in docs/components.md.

struct Tables {
    std::vector<FieldInfo> transform{
        field<&Transform::position>("position", "Position relative to the parent, in world units (pixels at zoom 1). "
                                                "Y points down.")
            .withStep(1.0f),
        field<&Transform::rotation>("rotation", "Rotation in degrees, clockwise, relative to the parent.")
            .withStep(1.0f),
        field<&Transform::scale>("scale", "Scale relative to the parent. Negative values mirror.").withStep(0.05f),
    };
    std::vector<FieldInfo> sprite{
        field<&Sprite::texture>("texture", "The image to draw.")
            .ofAsset(assets::AssetType::Texture)
            .withFlags(kFieldRequired),
        field<&Sprite::source>("source", "Part of the image to draw, in texels [x, y, w, h]. Empty = the whole "
                                         "image."),
        field<&Sprite::size>("size", "Size in world units before scaling.").withRange(0.0, kInf, true).withStep(1.0f),
        field<&Sprite::pivot>("pivot", "Point of the sprite placed at the entity's position: [0, 0] = top-left, "
                                       "[0.5, 0.5] = centre.")
            .withStep(0.05f),
        field<&Sprite::tint>("tint", "Colour multiplied with the image; alpha makes it transparent."),
        field<&Sprite::layer>("layer", "Draw order: lower layers are drawn first (further back)."),
        field<&Sprite::flipX>("flipX", "Mirror horizontally."),
        field<&Sprite::flipY>("flipY", "Mirror vertically."),
        field<&Sprite::visible>("visible", "Whether the sprite is drawn."),
    };
    std::vector<FieldInfo> spriteAnimation{
        field<&SpriteAnimation::frames>("frames", "Frames as texel rectangles [x, y, w, h] of the sprite's image, "
                                                  "in playing order.")
            .withFlags(kFieldRequired)
            .withCount(1, kMaxAnimationFrames),
        field<&SpriteAnimation::framesPerSecond>("framesPerSecond", "Playback speed.")
            .withRange(0.0, 240.0, true)
            .withStep(0.5f),
        field<&SpriteAnimation::loop>("loop", "Start again after the last frame."),
        field<&SpriteAnimation::playing>("playing", "Whether the animation plays when the game starts."),
        field<&SpriteAnimation::time>("time", "Seconds played so far.").withFlags(kFieldRuntime),
    };
    std::vector<FieldInfo> tilemap{
        field<&Tilemap::tileset>("tileset", "Image with the tiles, in cells of tileSize, numbered left to right, top "
                                            "to bottom from 0.")
            .ofAsset(assets::AssetType::Texture)
            .withFlags(kFieldRequired),
        field<&Tilemap::tileSize>("tileSize", "Size of one tile in world units and in tileset texels.")
            .withRange(0.0, kInf, true)
            .withStep(1.0f),
        field<&Tilemap::tilesetColumns>("tilesetColumns", "Number of tile columns in the tileset image.")
            .withRange(1.0, kInf),
        field<&Tilemap::solid>("solid", "Every non-empty tile blocks moving bodies."),
        field<&Tilemap::layer>("layer", "Draw order: lower layers are drawn first (further back)."),
        field<&Tilemap::tint>("tint", "Colour multiplied with every tile."),
        wholeField("rows", FieldKind::TileGrid, "The tiles, one string per row: tile numbers or '.' for empty, "
                                                "separated by spaces. The top-left tile is at the entity's position.")
            .withFlags(kFieldRequired),
    };
    std::vector<FieldInfo> boxCollider{
        field<&BoxCollider::size>("size", "Width and height of the box.").withRange(0.0, kInf, true).withStep(1.0f),
        field<&BoxCollider::offset>("offset", "Centre of the box relative to the entity's position.").withStep(1.0f),
        field<&BoxCollider::isTrigger>("isTrigger", "Triggers do not block; they report overlaps to scripts "
                                                    "(TriggerEnter/TriggerExit).")
            .withLabel("Trigger"),
    };
    std::vector<FieldInfo> kinematicBody{
        field<&KinematicBody::velocity>("velocity", "Units per second.").withFlags(kFieldRuntime),
        field<&KinematicBody::onGround>("onGround", "Standing on something.").withFlags(kFieldRuntime),
        field<&KinematicBody::hitCeiling>("hitCeiling", "Bumped into something above this tick.")
            .withFlags(kFieldRuntime),
        field<&KinematicBody::hitWall>("hitWall", "Bumped into something sideways this tick.")
            .withFlags(kFieldRuntime),
    };
    std::vector<FieldInfo> platformer{
        field<&PlatformerController::maxSpeed>("maxSpeed", "Top running speed, units per second.")
            .withRange(0.0, kInf)
            .withStep(1.0f),
        field<&PlatformerController::acceleration>("acceleration", "Speed gained per second on the ground.")
            .withRange(0.0, kInf)
            .withStep(10.0f),
        field<&PlatformerController::deceleration>("deceleration", "Speed lost per second on the ground with no "
                                                                   "input.")
            .withRange(0.0, kInf)
            .withStep(10.0f),
        field<&PlatformerController::airAcceleration>("airAcceleration", "Speed gained per second in the air.")
            .withRange(0.0, kInf)
            .withStep(10.0f),
        field<&PlatformerController::jumpSpeed>("jumpSpeed", "Initial upward speed of a jump.")
            .withRange(0.0, kInf)
            .withStep(1.0f),
        field<&PlatformerController::gravity>("gravity", "Downward acceleration, units per second squared.")
            .withRange(0.0, kInf)
            .withStep(10.0f),
        field<&PlatformerController::maxFallSpeed>("maxFallSpeed", "Falling speed limit.")
            .withRange(0.0, kInf)
            .withStep(1.0f),
        field<&PlatformerController::jumpCutMultiplier>("jumpCutMultiplier", "Fraction of upward speed kept when the "
                                                                             "jump button is released early.")
            .withRange(0.0, 1.0)
            .withStep(0.01f),
        field<&PlatformerController::coyoteTime>("coyoteTime", "Seconds after walking off a ledge during which "
                                                               "jumping still works.")
            .withRange(0.0, kInf)
            .withStep(0.01f),
        field<&PlatformerController::jumpBufferTime>("jumpBufferTime", "Seconds a jump press is remembered before "
                                                                       "landing.")
            .withRange(0.0, kInf)
            .withStep(0.01f),
        field<&PlatformerController::coyoteTimer>("coyoteTimer", "Coyote time left.").withFlags(kFieldRuntime),
        field<&PlatformerController::jumpBufferTimer>("jumpBufferTimer", "Buffered jump time left.")
            .withFlags(kFieldRuntime),
        field<&PlatformerController::jumping>("jumping", "Rising from a jump.").withFlags(kFieldRuntime),
    };
    std::vector<FieldInfo> topDown{
        field<&TopDownController::maxSpeed>("maxSpeed", "Top walking speed, units per second (also diagonally).")
            .withRange(0.0, kInf)
            .withStep(1.0f),
        field<&TopDownController::acceleration>("acceleration", "Speed gained per second while a direction is "
                                                                "held.")
            .withRange(0.0, kInf)
            .withStep(10.0f),
        field<&TopDownController::deceleration>("deceleration", "Speed lost per second with no input.")
            .withRange(0.0, kInf)
            .withStep(10.0f),
        field<&TopDownController::facing>("facing", "Last direction moved in.").withFlags(kFieldRuntime),
    };
    std::vector<FieldInfo> player{
        field<&Player::spawnPoint>("spawnPoint", "Where the player respawns (its position when the game started).")
            .withFlags(kFieldRuntime),
        field<&Player::owner>("owner", "In multiplayer, the id of the player controlling this character (0 when "
                                       "playing alone).")
            .withFlags(kFieldRuntime),
        field<&Player::name>("name", "In multiplayer, the controlling player's name.").withFlags(kFieldRuntime),
    };
    std::vector<FieldInfo> camera{
        field<&Camera::follow>("follow", "Entity to keep in the centre of the view (none: the camera's own "
                                         "position).")
            .withFlags(kFieldOptional),
        field<&Camera::zoom>("zoom", "Screen pixels per world unit.").withRange(0.0, 64.0, true).withStep(0.05f),
        field<&Camera::clampToBounds>("clampToBounds", "Keep the view inside the level (the union of all "
                                                       "tilemaps)."),
        field<&Camera::position>("position", "World point at the centre of the view.").withFlags(kFieldRuntime),
    };
    std::vector<FieldInfo> tags{
        field<&Tags::values>("values", "Labels scripts can search for (World.findWithTag) and test "
                                       "(entity:hasTag).")
            .withCount(0, kMaxTags)
            .withLength(1, kMaxTagLength),
    };
    std::vector<FieldInfo> scripts{
        wholeField("items", FieldKind::ScriptList, "Luau behaviours, each with its property values. They run in "
                                                   "this order.")
            .withCount(0, kMaxScriptsPerEntity),
    };

    // Only requirements whose absence makes a scene invalid are listed;
    // adding one rejects files that loaded before.
    std::vector<std::string_view> characterRequires{"Transform", "BoxCollider", "KinematicBody"};

    std::vector<ComponentInfo> components;

    Tables() {
        components = {
            component<Transform>("Transform", "General", "Position, rotation and scale.", transform),
            component<Sprite>("Sprite", "Rendering", "Draws an image (or part of one) at the entity's position.",
                              sprite),
            component<SpriteAnimation>("SpriteAnimation", "Rendering",
                                       "Plays frames on the entity's Sprite by changing its source rectangle.",
                                       spriteAnimation),
            component<Tilemap>("Tilemap", "Rendering", "A grid of tiles from a tileset image; can be solid ground.",
                               tilemap),
            component<BoxCollider>("BoxCollider", "Physics",
                                   "An axis-aligned box. Solid boxes block moving bodies; triggers report overlaps.",
                                   boxCollider),
            component<KinematicBody>("KinematicBody", "Physics",
                                     "Moves by its velocity and stops at solid tiles and boxes (needs a "
                                     "BoxCollider and a Transform to move).",
                                     kinematicBody),
            component<PlatformerController>("PlatformerController", "Gameplay",
                                            "Side-view running and jumping driven by the move_left, move_right "
                                            "and jump input actions.",
                                            platformer, characterRequires),
            component<TopDownController>("TopDownController", "Gameplay",
                                         "Top-down walking in all directions, driven by the move_left, "
                                         "move_right, move_up and move_down input actions.",
                                         topDown, characterRequires),
            component<Player>("Player", "Gameplay", "Marks the player character; it respawns at its start "
                                                    "position after falling below the kill plane. In multiplayer "
                                                    "every player who joins gets a copy of it.",
                              player),
            component<Camera>("Camera", "Rendering", "The view of the game. The first camera in the scene is used.",
                              camera),
            component<Tags>("Tags", "Gameplay", "Labels for finding and classifying entities from scripts.", tags,
                            {}, kComponentInlineField),
            component<Scripts>("Scripts", "Scripting", "Luau behaviours that run on this entity.", scripts, {},
                               kComponentInlineField),
        };
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

std::string joinNames(const std::vector<std::string_view>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out += i + 1 == names.size() ? " and " : ", ";
        out += names[i];
    }
    return out;
}

void checkNumber(std::vector<ComponentProblem>& out, const ComponentInfo& c, const FieldInfo& f, double value) {
    if (!f.range.contains(value)) out.push_back({std::string(c.name), std::string(f.name), f.range.describe()});
}

void checkAsset(std::vector<ComponentProblem>& out, const ComponentInfo& c, std::string_view fieldName,
                const assets::AssetId& id, assets::AssetType type, bool required, const assets::AssetRegistry* assets) {
    if (id.isNil()) {
        if (required) {
            out.push_back({std::string(c.name), std::string(fieldName),
                           std::format("a {} asset id is required", assets::assetTypeName(type))});
        }
        return;
    }
    if (assets == nullptr) return;
    const auto* entry = assets->find(id);
    if (entry == nullptr) {
        out.push_back({std::string(c.name), std::string(fieldName),
                       std::format("refers to asset {}, which is not in the project", id.toString())});
    } else if (entry->type != type) {
        out.push_back({std::string(c.name), std::string(fieldName),
                       std::format("asset '{}' is a {}, not a {}", entry->name, assets::assetTypeName(entry->type),
                                   assets::assetTypeName(type))});
    }
}

} // namespace

std::string_view fieldKindName(FieldKind kind) noexcept {
    switch (kind) {
        case FieldKind::Float: return "number";
        case FieldKind::Int: return "integer";
        case FieldKind::Bool: return "boolean";
        case FieldKind::String: return "text";
        case FieldKind::Vec2: return "vector";
        case FieldKind::Rect: return "rectangle";
        case FieldKind::Color: return "colour";
        case FieldKind::Asset: return "asset";
        case FieldKind::EntityRef: return "entity";
        case FieldKind::StringList: return "list of text";
        case FieldKind::RectList: return "list of rectangles";
        case FieldKind::TileGrid: return "tile grid";
        case FieldKind::ScriptList: return "list of scripts";
    }
    return "?";
}

std::string NumberRange::describe() const {
    const bool hasMin = min != -kUnbounded;
    const bool hasMax = max != kUnbounded;
    if (hasMin && min == 0.0 && !hasMax) return minExclusive ? "must be positive" : "may not be negative";
    if (hasMin && hasMax) {
        return minExclusive ? std::format("must be greater than {} and at most {}", min, max)
                            : std::format("must be between {} and {}", min, max);
    }
    if (hasMin) return minExclusive ? std::format("must be greater than {}", min) : std::format("must be at least {}", min);
    if (hasMax) return std::format("must be at most {}", max);
    return {};
}

std::string FieldInfo::displayLabel() const {
    if (!label.empty()) return std::string(label);
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (i == 0) {
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            continue;
        }
        if (std::isupper(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(name[i - 1]))) {
            out += ' ';
        }
        out += c;
    }
    return out;
}

const FieldInfo* ComponentInfo::findField(std::string_view fieldName) const {
    for (const FieldInfo& f : fields) {
        if (f.name == fieldName) return &f;
    }
    return nullptr;
}

std::span<const ComponentInfo> componentTypes() { return tables().components; }

const ComponentInfo* findComponentType(std::string_view name) {
    for (const ComponentInfo& c : componentTypes()) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

std::vector<std::string> componentTypeNames() {
    std::vector<std::string> names;
    for (const ComponentInfo& c : componentTypes()) names.emplace_back(c.name);
    return names;
}

std::vector<ComponentProblem> validateEntity(const Scene& scene, Entity entity, const assets::AssetRegistry* assets) {
    std::vector<ComponentProblem> out;
    if (!scene.isAlive(entity)) return out;
    for (const ComponentInfo& c : componentTypes()) {
        const void* data = c.getConst(scene, entity);
        if (data == nullptr) continue;

        std::vector<std::string_view> missing;
        for (const std::string_view req : c.requiredComponents) {
            const ComponentInfo* r = findComponentType(req);
            if (r != nullptr && !r->has(scene, entity)) missing.push_back(req);
        }
        if (!missing.empty()) {
            std::vector<std::string_view> all(c.requiredComponents.begin(), c.requiredComponents.end());
            out.push_back({std::string(c.name), "", std::format("requires {} on the same entity", joinNames(all))});
        }

        for (const FieldInfo& f : c.fields) {
            if (f.isRuntime()) continue;
            switch (f.kind) {
                case FieldKind::Float: checkNumber(out, c, f, f.ref<float>(data)); break;
                case FieldKind::Int: checkNumber(out, c, f, f.ref<int>(data)); break;
                case FieldKind::Vec2: {
                    const Vec2 v = f.ref<Vec2>(data);
                    if (f.range.bounded() && (!f.range.contains(v.x) || !f.range.contains(v.y))) {
                        out.push_back({std::string(c.name), std::string(f.name), f.range.describe()});
                    }
                    break;
                }
                case FieldKind::Asset:
                    checkAsset(out, c, f.name, f.ref<assets::AssetId>(data), f.assetType, f.isRequired(), assets);
                    break;
                case FieldKind::EntityRef: {
                    const Guid id = f.ref<Guid>(data);
                    if (!id.isNil() && scene.findById(id).isNull()) {
                        out.push_back({std::string(c.name), std::string(f.name),
                                       std::format("no entity with id {} exists in this scene", id.toString())});
                    }
                    break;
                }
                case FieldKind::StringList: {
                    const auto& list = f.ref<std::vector<std::string>>(data);
                    if (list.size() > f.maxCount) {
                        out.push_back({std::string(c.name), std::string(f.name),
                                       std::format("too many entries; the limit is {}", f.maxCount)});
                    }
                    for (const auto& s : list) {
                        if (s.size() < f.minLength || s.size() > f.maxLength) {
                            out.push_back({std::string(c.name), std::string(f.name),
                                           std::format("entries must be {} to {} characters", f.minLength,
                                                       f.maxLength)});
                            break;
                        }
                    }
                    break;
                }
                case FieldKind::RectList: {
                    const auto& list = f.ref<std::vector<Rect>>(data);
                    if (list.size() < f.minCount) {
                        out.push_back({std::string(c.name), std::string(f.name), "needs at least one entry"});
                    } else if (list.size() > f.maxCount) {
                        out.push_back({std::string(c.name), std::string(f.name),
                                       std::format("too many entries; the limit is {}", f.maxCount)});
                    }
                    break;
                }
                case FieldKind::TileGrid: {
                    const auto& map = *static_cast<const Tilemap*>(data);
                    if (map.width < 1 || map.height < 1) {
                        out.push_back({std::string(c.name), std::string(f.name), "needs at least one row and column"});
                    } else if (map.width > kMaxTilemapDimension || map.height > kMaxTilemapDimension) {
                        out.push_back({std::string(c.name), std::string(f.name),
                                       std::format("may be at most {} tiles wide and high", kMaxTilemapDimension)});
                    } else if (map.tiles.size() !=
                               static_cast<std::size_t>(map.width) * static_cast<std::size_t>(map.height)) {
                        out.push_back({std::string(c.name), std::string(f.name), "has the wrong number of tiles"});
                    }
                    break;
                }
                case FieldKind::ScriptList: {
                    const auto& list = static_cast<const Scripts*>(data)->items;
                    if (list.size() > f.maxCount) {
                        out.push_back({std::string(c.name), std::string(f.name),
                                       std::format("too many scripts; the limit is {}", f.maxCount)});
                    }
                    for (const AttachedScript& s : list) {
                        checkAsset(out, c, f.name, s.script, assets::AssetType::Script, true, assets);
                        if (s.properties.size() > kMaxScriptProperties) {
                            out.push_back({std::string(c.name), std::string(f.name),
                                           std::format("too many properties; the limit is {}", kMaxScriptProperties)});
                        }
                    }
                    break;
                }
                case FieldKind::Bool:
                case FieldKind::String:
                case FieldKind::Rect:
                case FieldKind::Color: break;
            }
        }
    }
    return out;
}

std::vector<const ComponentInfo*> missingRequirements(const Scene& scene, Entity entity, const ComponentInfo& component) {
    std::vector<const ComponentInfo*> needed;
    std::vector<const ComponentInfo*> pending{&component};
    while (!pending.empty()) {
        const ComponentInfo* c = pending.back();
        pending.pop_back();
        for (const std::string_view req : c->requiredComponents) {
            const ComponentInfo* r = findComponentType(req);
            if (r == nullptr || r->has(scene, entity)) continue;
            if (std::find(needed.begin(), needed.end(), r) != needed.end()) continue;
            needed.push_back(r);
            pending.push_back(r);
        }
    }
    // Table order, for predictable results.
    std::vector<const ComponentInfo*> ordered;
    for (const ComponentInfo& c : componentTypes()) {
        if (std::find(needed.begin(), needed.end(), &c) != needed.end()) ordered.push_back(&c);
    }
    return ordered;
}

std::vector<const ComponentInfo*> dependents(const Scene& scene, Entity entity, const ComponentInfo& component) {
    std::vector<const ComponentInfo*> out;
    for (const ComponentInfo& c : componentTypes()) {
        if (!c.has(scene, entity)) continue;
        if (std::find(c.requiredComponents.begin(), c.requiredComponents.end(), component.name) !=
            c.requiredComponents.end()) {
            out.push_back(&c);
        }
    }
    return out;
}

void remapEntityReferences(Scene& scene, std::span<const Entity> entities,
                           const std::vector<std::pair<Guid, Guid>>& remap) {
    for (const Entity e : entities) {
        forEachComponent(scene, e, [&](const ComponentInfo& c, void* data) {
            for (const FieldInfo& f : c.fields) {
                if (f.kind != FieldKind::EntityRef) continue;
                Guid& id = f.ref<Guid>(data);
                for (const auto& [from, to] : remap) {
                    if (id == from) {
                        id = to;
                        break;
                    }
                }
            }
        });
    }
}

std::size_t clearEntityReferences(Scene& scene, Guid id) {
    std::size_t cleared = 0;
    for (const Entity e : scene.entities()) {
        forEachComponent(scene, e, [&](const ComponentInfo& c, void* data) {
            for (const FieldInfo& f : c.fields) {
                if (f.kind != FieldKind::EntityRef) continue;
                Guid& ref = f.ref<Guid>(data);
                if (ref == id) {
                    ref = Guid{};
                    ++cleared;
                }
            }
        });
    }
    return cleared;
}

std::vector<assets::AssetId> assetReferences(const Scene& scene, Entity entity) {
    std::vector<assets::AssetId> out;
    const auto add = [&out](const assets::AssetId& id) {
        if (!id.isNil() && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    };
    forEachComponent(scene, entity, [&](const ComponentInfo& c, const void* data) {
        for (const FieldInfo& f : c.fields) {
            if (f.kind == FieldKind::Asset) add(f.ref<assets::AssetId>(data));
            if (f.kind == FieldKind::ScriptList) {
                for (const AttachedScript& s : static_cast<const Scripts*>(data)->items) add(s.script);
            }
        }
    });
    return out;
}

std::string formatFieldValue(const FieldInfo& f, const void* data) {
    const auto num = [](double v) { return std::format("{}", v); };
    switch (f.kind) {
        case FieldKind::Float: return num(f.ref<float>(data));
        case FieldKind::Int: return std::to_string(f.ref<int>(data));
        case FieldKind::Bool: return f.ref<bool>(data) ? "true" : "false";
        case FieldKind::String: return "\"" + f.ref<std::string>(data) + "\"";
        case FieldKind::Vec2: {
            const Vec2 v = f.ref<Vec2>(data);
            return std::format("[{}, {}]", num(v.x), num(v.y));
        }
        case FieldKind::Rect: {
            const Rect r = f.ref<Rect>(data);
            return std::format("[{}, {}, {}, {}]", num(r.x), num(r.y), num(r.w), num(r.h));
        }
        case FieldKind::Color: return f.ref<Color>(data).toHex();
        case FieldKind::Asset: {
            const auto& id = f.ref<assets::AssetId>(data);
            return id.isNil() ? std::string("none") : id.toString();
        }
        case FieldKind::EntityRef: {
            const Guid& id = f.ref<Guid>(data);
            return id.isNil() ? std::string("none") : id.toString();
        }
        case FieldKind::StringList: return std::format("{} entries", f.ref<std::vector<std::string>>(data).size());
        case FieldKind::RectList: return std::format("{} entries", f.ref<std::vector<Rect>>(data).size());
        case FieldKind::TileGrid: {
            const auto& map = *static_cast<const Tilemap*>(data);
            return std::format("{} x {} tiles", map.width, map.height);
        }
        case FieldKind::ScriptList: return std::format("{} scripts", static_cast<const Scripts*>(data)->items.size());
    }
    return {};
}

std::string generateComponentReferenceMarkdown() {
    std::string out =
        "# Component reference\n\n"
        "Generated from the component descriptors in `engine/scene/src/ComponentInfo.cpp` with\n"
        "`px-headless --print-component-reference`; do not edit by hand. The scene file\n"
        "format, Studio's inspector and this page all come from the same table.\n\n"
        "In scene files each component is an object of fields under the entity's\n"
        "`components` (see [project-format.md](project-format.md)); fields that are left\n"
        "out take their default. Runtime fields are simulation state: they are not\n"
        "saved, and Studio shows them read-only while the game is playing.\n";
    Scene scratch;
    const Entity e = scratch.createEntity("defaults");
    for (const ComponentInfo& c : componentTypes()) {
        c.add(scratch, e);
        const void* data = c.getConst(scratch, e);
        out += std::format("\n## {}\n\n*{}.* {}\n", c.name, c.category, c.description);
        if (!c.requiredComponents.empty()) {
            std::vector<std::string_view> req(c.requiredComponents.begin(), c.requiredComponents.end());
            out += std::format("\nRequires {} on the same entity.\n", joinNames(req));
        }
        if ((c.flags & kComponentInlineField) != 0) {
            const FieldInfo& f = c.fields.front();
            out += std::format("\nWritten as a {} directly: `\"{}\": [...]`. {}\n", fieldKindName(f.kind), c.name,
                               f.description);
            continue;
        }
        bool header = false;
        for (const FieldInfo& f : c.fields) {
            if (f.isRuntime()) continue;
            if (!header) {
                out += "\n| Field | Type | Default | Allowed values | Description |\n|---|---|---|---|---|\n";
                header = true;
            }
            std::string allowed = f.range.bounded() ? f.range.describe() : std::string();
            if (f.kind == FieldKind::Asset) allowed = std::format("a {} asset", assets::assetTypeName(f.assetType));
            if (f.maxCount != std::numeric_limits<std::size_t>::max() && f.kind != FieldKind::ScriptList) {
                allowed = std::format("{} to {} entries", f.minCount, f.maxCount);
            }
            const std::string def = f.isRequired() ? std::string("required") : formatFieldValue(f, data);
            out += std::format("| `{}` | {} | {} | {} | {} |\n", f.name, fieldKindName(f.kind), def, allowed,
                               f.description);
        }
        if (!header) out += "\nNo saved fields: `{}`.\n";
        std::string runtime;
        for (const FieldInfo& f : c.fields) {
            if (!f.isRuntime()) continue;
            runtime += std::format("{}`{}` ({})", runtime.empty() ? "" : ", ", f.name, f.description);
        }
        if (!runtime.empty()) out += std::format("\nRuntime: {}\n", runtime);
    }
    return out;
}

} // namespace px::scene
