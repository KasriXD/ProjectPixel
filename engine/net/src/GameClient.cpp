#include "px/net/GameClient.hpp"

#include "px/core/Log.hpp"
#include "px/core/Version.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <format>

namespace px::net {

using namespace px::scene;

namespace {

constexpr std::size_t kMaxStates = 128;          // received snapshots kept (about four seconds)
constexpr std::size_t kMaxPendingInputs = 256;   // unconfirmed inputs kept for prediction
constexpr std::size_t kInputsPerMessage = 16;    // newest frames repeated in every input message
constexpr float kSmoothingSeconds = 0.1f;        // corrections fade out over about this long
constexpr float kCorrectionEpsilon = 0.01f;

Scene prepare(const runtime::LoadedGame& game) {
    Scene scene = game.startScene;
    (void)runtime::extractPlayerTemplates(scene);
    return scene;
}

template <class T>
bool readInto(const SceneReplica& replica, const EntityState& entity, std::string_view component, T& out) {
    const ComponentInfo* info = findComponentType(component);
    return info != nullptr && replica.readComponent(entity, *info, &out);
}

} // namespace

GameClient::GameClient(const runtime::LoadedGame& game, std::unique_ptr<INetEndpoint> endpoint,
                       const ClientOptions& options)
    : m_loaded(game),
      m_endpoint(std::move(endpoint)),
      m_options(options),
      m_actions(game.manifest.input),
      m_characterActions(runtime::CharacterActions::from(m_actions)),
      m_tickRate(std::max(1, game.manifest.settings.tickRate)),
      m_viewSize(static_cast<float>(game.manifest.settings.display.virtualResolution.x),
                 static_cast<float>(game.manifest.settings.display.virtualResolution.y)),
      m_view(prepare(game)),
      m_viewReplica(m_view),
      m_latest(prepare(game)),
      m_latestReplica(m_latest) {}

GameClient::~GameClient() { disconnect(); }

Result<std::unique_ptr<GameClient>> GameClient::create(const runtime::LoadedGame& game,
                                                       std::unique_ptr<INetEndpoint> endpoint,
                                                       const ClientOptions& options) {
    if (!endpoint) return makeError(ErrorCode::InvalidArgument, "The client needs a network endpoint");
    std::unique_ptr<GameClient> client(new GameClient(game, std::move(endpoint), options));
    client->m_ids.assignInitial(client->m_view);
    client->m_initial = captureWorld(client->m_view, client->m_ids, 0);
    client->m_contentHash = net::contentHash(game.manifest, client->m_initial);
    client->rebuildCollision();
    runtime::updateCameras(client->m_view, client->m_worldBounds, client->m_viewSize);
    return client;
}

void GameClient::setDisconnected(std::string why) {
    if (m_status == Status::Disconnected) return;
    m_status = Status::Disconnected;
    m_statusText = std::move(why);
    m_predicted.reset();
    log::info("Disconnected: {}", m_statusText);
}

void GameClient::disconnect() {
    if (m_status == Status::Disconnected) return;
    if (m_server != kNoPeer) {
        (void)m_endpoint->send(m_server, Delivery::Reliable, encodeGoodbye());
        m_endpoint->flush();
        m_endpoint->disconnect(m_server, static_cast<std::uint32_t>(DisconnectReason::Left));
        m_endpoint->flush();
    }
    setDisconnected("Left the game");
}

void GameClient::receive(double now) {
    if (m_startTime < 0.0) m_startTime = now;
    m_events.clear();
    m_endpoint->poll(m_events);
    for (NetEvent& ev : m_events) {
        switch (ev.type) {
            case NetEvent::Type::Connected: {
                if (m_status != Status::Connecting) break;
                m_server = ev.peer;
                m_status = Status::Joining;
                m_statusText = "Joining...";
                HelloMessage hello;
                hello.engineVersion = version::string;
                hello.contentHash = m_contentHash;
                hello.playerName = m_options.playerName;
                (void)m_endpoint->send(m_server, Delivery::Reliable, encode(hello));
                m_endpoint->flush();
                break;
            }
            case NetEvent::Type::Disconnected: {
                std::string why = describeDisconnect(ev.reason);
                why[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(why[0])));
                setDisconnected(std::move(why));
                break;
            }
            case NetEvent::Type::Received:
                if (ev.data.size() <= kMaxServerMessageBytes) handleMessage(ev.data, now);
                break;
        }
    }
    if (m_status == Status::Connecting && now - m_startTime > m_options.connectTimeoutSeconds) {
        setDisconnected("Could not connect (timed out)");
    }
}

void GameClient::handleMessage(const std::vector<std::uint8_t>& data, double now) {
    switch (messageType(data).value_or(MessageType::Goodbye)) {
        case MessageType::Welcome: {
            if (m_status != Status::Joining) return;
            auto welcome = decodeWelcome(data);
            if (!welcome) return;
            if (welcome->tickRate != m_tickRate) {
                setDisconnected("The server runs the game at a different speed");
                return;
            }
            m_player = welcome->player;
            m_snapshotInterval = welcome->snapshotInterval;
            m_serverName = welcome->serverName;
            m_status = Status::Playing;
            m_statusText = std::format("Playing on {}", m_serverName);
            log::info("Joined '{}' as player {}", m_serverName, m_player);
            break;
        }
        case MessageType::Reject: {
            auto reject = decodeReject(data);
            setDisconnected(reject && !reject->text.empty() ? reject->text
                                                             : std::string("The server refused the connection"));
            break;
        }
        case MessageType::Snapshot:
            if (m_status == Status::Playing) handleSnapshot(data, now);
            break;
        default: break;
    }
}

void GameClient::handleSnapshot(const std::vector<std::uint8_t>& data, double now) {
    ByteReader r(data);
    auto header = readSnapshotHeader(r);
    if (!header || header->epoch < m_epoch) {
        ++m_stats.snapshotsDropped;
        return;
    }
    if (header->epoch > m_epoch) {
        // The server restarted the level: older states no longer apply.
        m_epoch = header->epoch;
        m_states.clear();
    }
    if (m_states.contains(header->tick)) {
        ++m_stats.snapshotsDropped;
        return;
    }
    const WorldState* baseline = &m_initial;
    if (header->baselineTick != 0) {
        const auto it = m_states.find(header->baselineTick);
        if (it == m_states.end()) {
            ++m_stats.snapshotsDropped;
            return;
        }
        baseline = &it->second;
    }
    auto state = readWorldDelta(r, *baseline);
    if (!state) {
        ++m_stats.snapshotsDropped;
        log::warn("Ignoring a snapshot: {}", state.error().message);
        return;
    }
    state->tick = header->tick;
    ++m_stats.snapshots;
    m_stats.lastSnapshotBytes = data.size();
    const bool newest = m_states.empty() || header->tick > m_states.rbegin()->first;
    m_states.emplace(header->tick, std::move(state.value()));
    while (m_states.size() > kMaxStates) m_states.erase(m_states.begin());

    // Estimate the server clock. Messages that arrive quickly say the most,
    // so the estimate moves up fast and down slowly.
    const double sample = static_cast<double>(header->tick) - now * m_tickRate;
    if (!m_haveClock || std::abs(sample - m_clockOffset) > 2.0 * m_tickRate) {
        m_clockOffset = sample;
        m_haveClock = true;
    } else {
        m_clockOffset += (sample - m_clockOffset) * (sample > m_clockOffset ? 0.25 : 0.02);
    }

    if (newest) {
        const WorldState& s = m_states.rbegin()->second;
        m_game = s.game;
        m_latestReplica.apply(s);
        rebuildCollision();
        reconcile(s, header->lastInputSequence);
    }
}

void GameClient::rebuildCollision() {
    Fnv1a64 h;
    for (const Entity e : m_latest.entities()) {
        const auto* map = m_latest.tryGet<Tilemap>(e);
        if (map == nullptr || m_latest.tryGet<Transform>(e) == nullptr) continue;
        const Vec2 p = m_latest.worldPosition(e);
        h.addU64(std::bit_cast<std::uint32_t>(p.x));
        h.addU64(std::bit_cast<std::uint32_t>(p.y));
        h.addU64(static_cast<std::uint64_t>(map->width));
        h.addU64(map->solid ? 1 : 0);
        h.addU64(std::bit_cast<std::uint32_t>(map->tileSize.x));
        h.addU64(std::bit_cast<std::uint32_t>(map->tileSize.y));
        for (const int t : map->tiles) h.addU64(t >= 0 ? 1 : 0);
    }
    if (h.value() != m_tilemapHash || m_collision.grids().empty()) {
        m_tilemapHash = h.value();
        m_collision.clear();
        runtime::addTilemapCollision(m_latest, m_collision);
        m_worldBounds = runtime::tilemapBounds(m_latest);
    }
    m_collision.clearBoxes();
    for (const Entity e : m_latest.entities()) {
        const auto* c = m_latest.tryGet<BoxCollider>(e);
        if (c == nullptr || c->isTrigger || m_latest.tryGet<Transform>(e) == nullptr ||
            m_latest.tryGet<KinematicBody>(e) != nullptr) {
            continue;
        }
        m_collision.addBox(c->worldRect(m_latest.worldPosition(e)), e.index + 1);
    }
}

NetId GameClient::ownNetId(const WorldState& state) const {
    const auto comps = replicatedComponents();
    std::size_t playerBit = comps.size();
    for (std::size_t i = 0; i < comps.size(); ++i) {
        if (comps[i].info->name == "Player") playerBit = i;
    }
    for (const EntityState& e : state.entities) {
        if (playerBit >= comps.size() || ((e.components >> playerBit) & 1u) == 0) continue;
        Player p;
        if (readInto(m_viewReplica, e, "Player", p) && p.owner == m_player) return e.id;
    }
    return kNoNetId;
}

Entity GameClient::character() const {
    if (m_player == runtime::kNoPlayer) return kNullEntity;
    for (const Entity e : m_view.entities()) {
        const auto* p = m_view.tryGet<Player>(e);
        if (p != nullptr && p->owner == m_player) return e;
    }
    return kNullEntity;
}

std::optional<Vec2> GameClient::predictedPosition() const {
    if (!m_predicted) return std::nullopt;
    return m_predicted->position;
}

void GameClient::reconcile(const WorldState& newest, std::uint32_t lastInputSequence) {
    // Inputs the server has used no longer need predicting (or resending).
    while (!m_pending.empty() && m_pending.front().sequence <= lastInputSequence) m_pending.pop_front();
    if (!m_options.predict) return;

    const NetId own = ownNetId(newest);
    const EntityState* es = own != kNoNetId ? newest.find(own) : nullptr;
    // Only characters at the top of the hierarchy are predicted (players are).
    if (es == nullptr || es->parent != kNoNetId) {
        m_predicted.reset();
        return;
    }
    Predicted p;
    Transform t;
    Player player;
    PlatformerController platformer;
    TopDownController topDown;
    if (!readInto(m_viewReplica, *es, "Transform", t) || !readInto(m_viewReplica, *es, "KinematicBody", p.body) ||
        !readInto(m_viewReplica, *es, "BoxCollider", p.box) || !readInto(m_viewReplica, *es, "Player", player)) {
        m_predicted.reset();
        return;
    }
    if (readInto(m_viewReplica, *es, "PlatformerController", platformer)) p.platformer = platformer;
    if (readInto(m_viewReplica, *es, "TopDownController", topDown)) p.topDown = topDown;
    if (!p.platformer && !p.topDown) {
        m_predicted.reset();
        return;
    }
    Sprite sprite;
    if (readInto(m_viewReplica, *es, "Sprite", sprite)) p.flipX = sprite.flipX;
    p.position = t.position;
    p.spawnPoint = player.spawnPoint;

    // Replay what the server has not seen yet.
    for (const PendingInput& in : m_pending) step(p, in.frame);

    if (m_predicted) {
        const Vec2 error = m_predicted->position - p.position;
        const float distance = std::sqrt(error.x * error.x + error.y * error.y);
        if (distance > kTeleportDistance) {
            m_smoothing = {};
        } else {
            m_smoothing += error;
        }
        if (distance > kCorrectionEpsilon) {
            ++m_stats.corrections;
            m_stats.lastCorrection = distance;
        }
    }
    m_predicted = p;
}

void GameClient::step(Predicted& p, const runtime::InputFrame& input) const {
    const float dt = 1.0f / static_cast<float>(m_tickRate);
    Sprite sprite;
    sprite.flipX = p.flipX;
    if (p.platformer) runtime::stepPlatformerControl(*p.platformer, p.body, &sprite, input, m_characterActions, dt);
    if (p.topDown) runtime::stepTopDownControl(*p.topDown, p.body, &sprite, input, m_characterActions, dt);
    p.flipX = sprite.flipX;
    const physics::MoveResult r = runtime::moveBody(m_collision, p.body, p.box.worldRect(p.position), dt);
    p.position += r.moved;
    if (const auto& kill = m_latest.settings().killPlaneY; kill && p.position.y > *kill) {
        p.position = p.spawnPoint;
        runtime::resetMotion(&p.body, p.platformer ? &*p.platformer : nullptr);
    }
}

void GameClient::tick(const runtime::InputFrame& input) {
    if (m_status != Status::Playing) return;
    ++m_sequence;
    m_pending.push_back({m_sequence, input});
    while (m_pending.size() > kMaxPendingInputs) m_pending.pop_front();
    sendInputs();
    if (m_predicted) step(*m_predicted, input);
}

void GameClient::sendInputs() {
    InputMessage m;
    m.ackEpoch = m_epoch;
    m.ackTick = m_states.empty() ? 0 : m_states.rbegin()->first;
    m.lastSequence = m_sequence;
    const std::size_t n = std::min(kInputsPerMessage, m_pending.size());
    for (std::size_t i = m_pending.size() - n; i < m_pending.size(); ++i) m.frames.push_back(m_pending[i].frame);
    (void)m_endpoint->send(m_server, Delivery::Unreliable, encode(m));
    m_endpoint->flush();
}

double GameClient::serverTickAt(double now) const { return now * m_tickRate + m_clockOffset; }

void GameClient::updateView(double now) {
    const double dt = m_lastViewTime < 0.0 ? 0.0 : std::max(0.0, now - m_lastViewTime);
    m_lastViewTime = now;
    if (m_states.empty()) return;

    const double delayTicks = m_options.interpolationDelaySeconds >= 0.0
                                  ? m_options.interpolationDelaySeconds * m_tickRate
                                  : 2.0 * m_snapshotInterval + 2.0;
    const double renderTick = serverTickAt(now) - delayTicks;
    const auto key = static_cast<std::uint32_t>(std::clamp(std::floor(renderTick), 0.0, 4294967295.0));
    auto after = m_states.upper_bound(key);
    if (after == m_states.begin()) {
        m_viewReplica.apply(after->second); // older than anything we have: show the oldest
    } else {
        const auto before = std::prev(after);
        if (after == m_states.end()) {
            m_viewReplica.apply(before->second); // no newer state yet: hold the newest
        } else {
            const double span = static_cast<double>(after->first - before->first);
            const auto alpha = static_cast<float>(std::clamp((renderTick - before->first) / span, 0.0, 1.0));
            m_viewReplica.apply(before->second, &after->second, alpha);
        }
    }

    // The own character is shown where it is predicted to be now.
    const Entity own = character();
    if (!own.isNull() && m_predicted) {
        m_smoothing = m_smoothing * static_cast<float>(std::exp(-dt / kSmoothingSeconds));
        m_view.setWorldPosition(own, m_predicted->position + m_smoothing);
        if (auto* sprite = m_view.tryGet<Sprite>(own)) sprite->flipX = m_predicted->flipX;
    }
    // Every client's camera follows its own character.
    if (!own.isNull()) {
        const Guid id = m_view.get<EntityInfo>(own).id;
        m_view.registry().each<Camera>([&](Entity, Camera& cam) { cam.follow = id; });
    }
    runtime::updateCameras(m_view, m_worldBounds, m_viewSize);
}

ClientStats GameClient::stats() const {
    ClientStats s = m_stats;
    if (m_server != kNoPeer) {
        const PeerStats p = m_endpoint->stats(m_server);
        s.roundTripMs = p.roundTripMs;
        s.packetLoss = p.packetLoss;
        s.bytesReceived = p.bytesReceived;
        s.bytesSent = p.bytesSent;
    }
    s.pendingInputs = m_pending.size();
    return s;
}

} // namespace px::net
