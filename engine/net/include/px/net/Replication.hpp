#pragma once

#include "px/core/Result.hpp"
#include "px/net/Bytes.hpp"
#include "px/project/Project.hpp"
#include "px/runtime/GameSession.hpp"
#include "px/scene/ComponentInfo.hpp"
#include "px/scene/Scene.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// Snapshot replication (docs/multiplayer.md#replication). The server
// captures the replicated part of the world after a tick as a WorldState and
// sends each client the difference from the newest state that client has
// acknowledged (or from the initial scene, which both sides load from the
// same project). Which components and fields replicate is decided here, on
// top of the component descriptors, so new fields replicate automatically.
namespace px::net {

using NetId = std::uint32_t; // compact entity id used on the wire
inline constexpr NetId kNoNetId = 0;

struct NetPlayer {
    runtime::PlayerId id = runtime::kNoPlayer;
    std::string name;
    bool operator==(const NetPlayer&) const = default;
};

// Game state outside the scene that clients show.
struct NetGameState {
    int score = 0;
    std::string objective;
    bool levelComplete = false;
    std::string completionMessage;
    int respawns = 0;
    int scriptErrors = 0;
    std::string lastScriptError;
    std::vector<NetPlayer> players; // in join order
    int restartTicks = -1;          // ticks until the level restarts (-1: not scheduled)
    bool operator==(const NetGameState&) const = default;
};

// The replicated part of one entity.
struct EntityState {
    NetId id = kNoNetId;
    Guid guid;
    std::string name;
    NetId parent = kNoNetId;
    std::uint32_t components = 0;          // bit i: replicatedComponents()[i] is present
    std::vector<std::uint8_t> data;        // the replicated fields of those components, encoded, in order
    std::vector<std::uint32_t> fieldStarts; // offset of each of those fields in data

    // The encoded bytes of field `n` (counting over all present components' replicated fields).
    [[nodiscard]] std::span<const std::uint8_t> field(std::size_t n) const;
};

struct WorldState {
    std::uint32_t tick = 0;
    std::vector<EntityState> entities; // parents before children
    NetGameState game;

    // Lookup by id; valid after reindex() (captureWorld and readWorldDelta
    // return indexed states; call it again after editing `entities`).
    [[nodiscard]] const EntityState* find(NetId id) const;
    void reindex();

private:
    std::unordered_map<NetId, std::size_t> m_lookup;
};

// A component type that replicates, and which of its fields do.
struct ReplicatedComponent {
    const scene::ComponentInfo* info = nullptr;
    std::vector<const scene::FieldInfo*> fields;
    bool interpolate = false; // blended between snapshots on clients (Transform)
};
[[nodiscard]] std::span<const ReplicatedComponent> replicatedComponents();

// Network ids by entity GUID. The initial scene's entities get 1..N in scene
// order, identically on server and clients; later entities get the next
// numbers, and an entity that comes back (a restarted level) keeps its id.
class NetIdMap {
public:
    void assignInitial(const scene::Scene& scene);
    NetId idFor(const Guid& guid);
    [[nodiscard]] NetId find(const Guid& guid) const;

private:
    std::unordered_map<Guid, NetId> m_ids;
    NetId m_next = 1;
};

[[nodiscard]] WorldState captureWorld(const scene::Scene& scene, NetIdMap& ids, std::uint32_t tick);

// Writes `current` as a change from `baseline`.
void writeWorldDelta(ByteWriter& w, const WorldState& baseline, const WorldState& current);
// Reads a change written by writeWorldDelta and applies it to a copy of
// `baseline`. Validates everything; the result's tick is left at 0.
[[nodiscard]] Result<WorldState> readWorldDelta(ByteReader& r, const WorldState& baseline);

// Identifies the replication format (components, fields, kinds).
[[nodiscard]] std::uint64_t schemaHash();
// Identifies a game as clients and the server must agree on it: engine and
// protocol version, replication format, project id, tick rate, input
// actions and the initial world state. Different hashes mean different games.
[[nodiscard]] std::uint64_t contentHash(const project::ProjectManifest& manifest, const WorldState& initial);

// Keeps a client's scene in step with received world states.
class SceneReplica {
public:
    // `scene` must be the client's copy of the initial scene, with the player
    // templates removed (runtime::extractPlayerTemplates), as on the server.
    explicit SceneReplica(scene::Scene& scene);

    // Makes the scene match `state`. When `next` is given, interpolated
    // components (Transform) are blended towards it by `alpha` (0..1) unless
    // the entity moved too far (a teleport).
    void apply(const WorldState& state, const WorldState* next = nullptr, float alpha = 0.0f);
    // Writes one component of an entity from a state (used for prediction).
    [[nodiscard]] bool readComponent(const EntityState& entity, const scene::ComponentInfo& component,
                                     void* target) const;

    [[nodiscard]] scene::Entity entityOf(NetId id) const;
    [[nodiscard]] scene::Scene& scene() { return m_scene; }

private:
    struct Applied {
        scene::Entity entity;
        std::vector<std::uint8_t> data;
        std::uint32_t components = 0;
        bool blended = false;
    };
    scene::Scene& m_scene;
    std::unordered_map<NetId, Applied> m_entities;
    std::unordered_map<Guid, scene::Entity> m_initial; // initial entities not yet seen in a state
};

// Distance above which a replicated move is treated as a teleport (not blended).
inline constexpr float kTeleportDistance = 64.0f;

} // namespace px::net
