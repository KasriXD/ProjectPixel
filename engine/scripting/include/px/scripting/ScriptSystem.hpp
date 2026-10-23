#pragma once

#include "px/core/Result.hpp"
#include "px/scene/Scene.hpp"
#include "px/scripting/ScriptHost.hpp"
#include "px/scripting/ScriptLibrary.hpp"
#include "px/scripting/ScriptTypes.hpp"
#include "px/scripting/ScriptVm.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace px::scripting {

// Lifecycle events a behaviour can handle. Order matches kEventNames.
enum class LifecycleEvent : std::uint8_t { Start, Tick, TriggerEnter, TriggerExit, Destroy, Count };
inline constexpr std::size_t kLifecycleEventCount = static_cast<std::size_t>(LifecycleEvent::Count);
inline constexpr std::array<const char*, kLifecycleEventCount> kLifecycleEventNames = {
    "Start", "Tick", "TriggerEnter", "TriggerExit", "Destroy"};

struct ScriptSystemOptions {
    Realm realm = Realm::Authority;
    ScriptLimits limits;
    std::uint64_t seed = 1; // seeds math.random; same seed, same numbers
};

// What a behaviour declares, for tools such as Studio's inspector.
struct BehaviourDescription {
    std::vector<std::pair<std::string, scene::ScriptValue>> properties; // declared defaults, sorted by name
    std::vector<std::string> events;                                     // lifecycle events it handles
};

struct ScriptStats {
    std::size_t behaviours = 0;      // script modules loaded
    std::size_t instances = 0;       // live script instances
    std::size_t disabledInstances = 0;
    std::size_t errors = 0;          // errors reported so far
    std::size_t tickHandlers = 0;    // instances whose behaviour defines Tick
    std::size_t memoryBytes = 0;
};

// Runs the Luau behaviours attached to scene entities (Scripts components).
//
// Lifecycle (see docs/scripting.md#lifecycle):
//  * create(): modules are loaded and one instance is created per attached
//    script, in hierarchy order and then attachment order. Property errors and
//    module errors fail creation (they are content errors, like bad JSON).
//  * start(): Start() runs once per instance before its first Tick.
//  * tick(dt): Tick(dt) runs for instances whose behaviour defines it.
//  * triggerEnter/Exit: delivered by the runtime's trigger detection.
//  * entityDestroyed(): Destroy() runs, then the instance is released.
// A runtime error disables only the failing instance; the game continues.
class ScriptSystem {
public:
    [[nodiscard]] static Result<std::unique_ptr<ScriptSystem>> create(scene::Scene& scene,
                                                                      std::shared_ptr<const ScriptLibrary> library,
                                                                      IScriptHost& host,
                                                                      const ScriptSystemOptions& options = {});
    ~ScriptSystem();
    ScriptSystem(const ScriptSystem&) = delete;

    // Loads one behaviour in a throwaway sandbox (its top-level code runs,
    // under the usual limits, against an empty scene) and reports what it
    // declares. Errors are the same as when a game loads the script.
    [[nodiscard]] static Result<BehaviourDescription> describe(std::shared_ptr<const ScriptLibrary> library,
                                                               assets::AssetId script,
                                                               const ScriptLimits& limits = {});
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    void beginTick();             // resets per-tick budgets
    void start();                 // Start for instances that have not started yet
    void tick(float dt);
    void triggerEnter(scene::Entity self, scene::Entity other);
    void triggerExit(scene::Entity self, scene::Entity other);
    // Called before the entity is removed from the scene (it is still alive).
    void entityDestroyed(scene::Entity entity);
    // Delivers queued custom events (Events.emit). Returns true if any were delivered.
    bool deliverEvents();

    [[nodiscard]] bool hasScript(scene::Entity entity) const;
    [[nodiscard]] ScriptStats stats() const;
    [[nodiscard]] Realm realm() const { return m_options.realm; }
    [[nodiscard]] ScriptVm& vm() { return *m_vm; }

private:
    friend struct Bindings;

    struct Behaviour {
        const CompiledScript* script = nullptr;
        int moduleRef = 0;
        int instanceMetatableRef = 0;
        std::array<int, kLifecycleEventCount> handlers{}; // registry refs, 0 = not defined
        std::vector<std::pair<std::string, scene::ScriptValue>> defaults; // declared properties
    };
    struct Instance {
        scene::Entity entity;
        std::size_t behaviour = 0;
        int tableRef = 0;
        std::string entityName;
        bool started = false;
        bool enabled = true;
        bool destroyed = false;
    };
    using EventValue = std::variant<std::monostate, double, bool, std::string, Vec2, scene::Entity>;
    struct QueuedEvent {
        std::string name;
        EventValue value;
    };
    struct Listener {
        int id = 0;
        std::string event;
        int functionRef = 0;
        int ownerInstance = -1; // -1: registered by module code, lives for the session
        std::string script;     // for error messages
    };
    struct EntityHandle {
        std::uint32_t generation = 0;
        int ref = 0;
    };

    ScriptSystem(scene::Scene& scene, std::shared_ptr<const ScriptLibrary> library, IScriptHost& host,
                 const ScriptSystemOptions& options);

    Result<std::size_t> loadBehaviour(const CompiledScript& script);
    Status createInstance(scene::Entity entity, const scene::AttachedScript& attached);
    void call(std::size_t instance, LifecycleEvent event, scene::Entity other = scene::kNullEntity, float dt = 0.0f);
    void reportError(const std::string& script, const std::string& entity, const std::string& phase,
                     const ScriptVm::CallResult& result);
    void disable(std::size_t instance);
    // Live instances of the entity, in attachment order.
    [[nodiscard]] std::vector<std::size_t> instancesOf(scene::Entity entity) const;
    [[nodiscard]] std::string currentScriptName() const;

    // Used by the bindings (Bindings is a friend).
    void pushEntity(lua_State* L, scene::Entity entity);
    void pushEventValue(lua_State* L, const EventValue& value);

    scene::Scene& m_scene;
    std::shared_ptr<const ScriptLibrary> m_library;
    IScriptHost& m_host;
    ScriptSystemOptions m_options;
    std::unique_ptr<ScriptVm> m_vm;

    std::vector<Behaviour> m_behaviours;
    std::unordered_map<assets::AssetId, std::size_t> m_behaviourIndex;
    std::vector<Instance> m_instances;                          // creation order
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> m_byEntity; // entity index -> instances
    std::unordered_map<std::uint32_t, EntityHandle> m_entityHandles;
    std::vector<QueuedEvent> m_eventQueue;
    std::vector<Listener> m_listeners;
    int m_nextListenerId = 1;
    int m_entityMethodsRef = 0;

    // Execution context, for bindings.
    int m_currentInstance = -1;
    const CompiledScript* m_loadingScript = nullptr;
    std::size_t m_errorCount = 0;
    int m_logLinesThisTick = 0;
    bool m_logLimitReported = false;
};

} // namespace px::scripting
