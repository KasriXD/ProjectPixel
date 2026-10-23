#pragma once

#include "px/assets/AssetRegistry.hpp"
#include "px/scene/Components.hpp"
#include "px/scene/Scene.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Component descriptors: one machine-readable description of every built-in
// component and its fields. The scene file format, the Studio inspector, the
// component reference (docs/components.md) and Studio's validation all use
// these tables, so a field added here is saved, loaded, documented and
// editable without further code. See docs/adr/0014-component-descriptors.md.
namespace px::scene {

enum class FieldKind : std::uint8_t {
    Float,      // float
    Int,        // int
    Bool,       // bool
    String,     // std::string
    Vec2,       // Vec2
    Rect,       // Rect
    Color,      // Color
    Asset,      // assets::AssetId of FieldInfo::assetType
    EntityRef,  // Guid of another entity in the same scene (nil = none)
    StringList, // std::vector<std::string>
    RectList,   // std::vector<Rect>
    TileGrid,   // a Tilemap's width, height and tiles (access returns the Tilemap)
    ScriptList, // the Scripts component's attached scripts (access returns the Scripts)
};

[[nodiscard]] std::string_view fieldKindName(FieldKind kind) noexcept;

enum FieldFlags : std::uint32_t {
    kFieldNone = 0,
    kFieldRuntime = 1u << 0,  // simulation state: not saved; Studio shows it read-only while playing
    kFieldRequired = 1u << 1, // must be present in files
    kFieldOptional = 1u << 2, // written only when it has a value (EntityRef that is not nil)
};

inline constexpr double kUnbounded = std::numeric_limits<double>::infinity();

// Allowed values of a number (or of each axis of a vector).
struct NumberRange {
    double min = -kUnbounded;
    double max = kUnbounded;
    bool minExclusive = false;

    [[nodiscard]] constexpr bool bounded() const { return min != -kUnbounded || max != kUnbounded; }
    [[nodiscard]] constexpr bool contains(double v) const {
        return (minExclusive ? v > min : v >= min) && v <= max;
    }
    // "must be positive", "must be between 0 and 1", ...
    [[nodiscard]] std::string describe() const;
};

struct FieldInfo {
    std::string_view name;        // key in files and identifier, e.g. "jumpSpeed"
    std::string_view label;       // shown in Studio; empty = derived from name ("Jump Speed")
    std::string_view description; // tooltip and reference text
    FieldKind kind = FieldKind::Float;
    void* (*access)(void* component) = nullptr;
    std::uint32_t flags = kFieldNone;
    NumberRange range;
    float step = 0.1f;                                  // Studio drag speed for numbers
    assets::AssetType assetType = assets::AssetType::Texture; // Asset fields
    std::size_t maxCount = std::numeric_limits<std::size_t>::max(); // list fields
    std::size_t minCount = 0;                           // list fields
    std::size_t maxLength = std::numeric_limits<std::size_t>::max(); // strings / list items
    std::size_t minLength = 0;

    [[nodiscard]] bool isRuntime() const { return (flags & kFieldRuntime) != 0; }
    [[nodiscard]] bool isRequired() const { return (flags & kFieldRequired) != 0; }
    [[nodiscard]] std::string displayLabel() const;

    // Typed access to the field of a component instance. T must match kind.
    template <class T>
    [[nodiscard]] T& ref(void* component) const { return *static_cast<T*>(access(component)); }
    template <class T>
    [[nodiscard]] const T& ref(const void* component) const {
        return *static_cast<const T*>(access(const_cast<void*>(component)));
    }

    // Builder helpers for the descriptor tables.
    constexpr FieldInfo& withLabel(std::string_view l) { label = l; return *this; }
    constexpr FieldInfo& withRange(double lo, double hi, bool loExclusive = false) {
        range = {lo, hi, loExclusive};
        return *this;
    }
    constexpr FieldInfo& withStep(float s) { step = s; return *this; }
    constexpr FieldInfo& withFlags(std::uint32_t f) { flags |= f; return *this; }
    constexpr FieldInfo& ofAsset(assets::AssetType t) { assetType = t; return *this; }
    constexpr FieldInfo& withCount(std::size_t lo, std::size_t hi) { minCount = lo; maxCount = hi; return *this; }
    constexpr FieldInfo& withLength(std::size_t lo, std::size_t hi) { minLength = lo; maxLength = hi; return *this; }
};

enum ComponentFlags : std::uint32_t {
    kComponentNone = 0,
    // In files the component is written as the value of its single field
    // (e.g. "Tags": ["coin"]) instead of an object of fields.
    kComponentInlineField = 1u << 0,
};

struct ComponentInfo {
    std::string_view name;        // "Sprite"; key in files and in scripts (entity:hasComponent)
    std::string_view category;    // grouping in Studio's "Add Component" menu
    std::string_view description;
    std::span<const FieldInfo> fields;
    std::span<const std::string_view> requiredComponents; // must be on the same entity
    std::uint32_t flags = kComponentNone;

    bool (*has)(const Scene&, Entity) = nullptr;
    void* (*get)(Scene&, Entity) = nullptr; // nullptr if the entity lacks it
    const void* (*getConst)(const Scene&, Entity) = nullptr;
    void (*add)(Scene&, Entity) = nullptr;  // adds a default-constructed component
    void (*remove)(Scene&, Entity) = nullptr;

    [[nodiscard]] const FieldInfo* findField(std::string_view fieldName) const;
};

// All built-in components, in the order they are written to files.
[[nodiscard]] std::span<const ComponentInfo> componentTypes();
[[nodiscard]] const ComponentInfo* findComponentType(std::string_view name);
[[nodiscard]] std::vector<std::string> componentTypeNames();

// Limits that keep untrusted content from exhausting resources.
inline constexpr std::size_t kMaxTags = 32;
inline constexpr std::size_t kMaxTagLength = 64;
inline constexpr std::size_t kMaxScriptsPerEntity = 16;
inline constexpr std::size_t kMaxScriptProperties = 64;
inline constexpr std::size_t kMaxScriptStringLength = 1000;
inline constexpr std::size_t kMaxAnimationFrames = 1024;
inline constexpr int kMaxTilemapDimension = 4096;

// A problem with an entity's components that would make its scene fail to
// load, e.g. a missing required component. Checked by the file format and
// shown by Studio.
struct ComponentProblem {
    std::string component; // component name ("" for the entity itself)
    std::string field;     // field name, if any
    std::string message;   // "requires Transform, BoxCollider and KinematicBody on the same entity"
};

// Problems that a scene loader would reject, for one entity: required
// components, value ranges, list sizes, asset types (when `assets` is given)
// and entity references.
[[nodiscard]] std::vector<ComponentProblem> validateEntity(const Scene& scene, Entity entity,
                                                           const assets::AssetRegistry* assets = nullptr);

// Components that `component` requires (transitively), in table order, that
// the entity does not have yet. Studio adds these together with it.
[[nodiscard]] std::vector<const ComponentInfo*> missingRequirements(const Scene& scene, Entity entity,
                                                                    const ComponentInfo& component);
// Components on the entity that require `component` (removing it would break them).
[[nodiscard]] std::vector<const ComponentInfo*> dependents(const Scene& scene, Entity entity,
                                                           const ComponentInfo& component);

// Calls fn(info, componentPointer) for every component the entity has, in table order.
template <class Fn>
void forEachComponent(Scene& scene, Entity entity, Fn&& fn) {
    for (const ComponentInfo& info : componentTypes()) {
        if (void* c = info.get(scene, entity)) fn(info, c);
    }
}
template <class Fn>
void forEachComponent(const Scene& scene, Entity entity, Fn&& fn) {
    for (const ComponentInfo& info : componentTypes()) {
        if (const void* c = info.getConst(scene, entity)) fn(info, c);
    }
}

// Rewrites entity references (EntityRef fields) on the given entities using
// `remap` (old id -> new id); references not in the map are left alone.
// Used when duplicating entities so copies refer to each other.
void remapEntityReferences(Scene& scene, std::span<const Entity> entities,
                           const std::vector<std::pair<Guid, Guid>>& remap);
// Clears every EntityRef field in the scene that refers to `id`.
// Returns how many references were cleared.
std::size_t clearEntityReferences(Scene& scene, Guid id);
// Asset ids referenced by the entity's components (Asset fields and attached scripts).
[[nodiscard]] std::vector<assets::AssetId> assetReferences(const Scene& scene, Entity entity);

// The value of a field as short display text ("[16, 16]", "#ffffff", "0.45").
[[nodiscard]] std::string formatFieldValue(const FieldInfo& field, const void* component);

// docs/components.md, generated from the descriptors
// (px-headless --print-component-reference).
[[nodiscard]] std::string generateComponentReferenceMarkdown();

} // namespace px::scene
