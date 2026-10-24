#include "px/net/GameServer.hpp"

#include "px/core/Log.hpp"
#include "px/core/Version.hpp"

#include <algorithm>
#include <format>
#include <unordered_map>

namespace px::net {

namespace {

constexpr std::size_t kSnapshotHistory = 64; // about two seconds at 30 snapshots per second
constexpr std::size_t kMaxQueuedInputs = 128;
constexpr std::uint32_t kMaxInputLead = 512; // inputs further ahead than this are ignored
constexpr std::size_t kCatchUpSlack = 4;     // queued frames beyond the buffer before frames are merged

} // namespace

GameServer::GameServer(const runtime::LoadedGame& game, std::unique_ptr<INetEndpoint> endpoint,
                       const ServerOptions& options)
    : m_game(game), m_endpoint(std::move(endpoint)), m_options(options) {}

GameServer::~GameServer() { shutdown(); }

Result<std::unique_ptr<GameServer>> GameServer::create(const runtime::LoadedGame& game,
                                                       std::unique_ptr<INetEndpoint> endpoint,
                                                       const ServerOptions& options) {
    if (!endpoint) return makeError(ErrorCode::InvalidArgument, "The server needs a network endpoint");
    std::unique_ptr<GameServer> server(new GameServer(game, std::move(endpoint), options));
    server->m_options.maxPlayers = std::clamp<std::size_t>(options.maxPlayers, 1, runtime::kMaxPlayers);
    server->m_options.snapshotInterval = std::clamp(options.snapshotInterval, 1, 60);
    server->m_tickRate = std::max(1, game.manifest.settings.tickRate);
    const runtime::ActionSet actions(game.manifest.input);
    server->m_actionMask = actions.size() >= 32 ? 0xFFFFFFFFu : (1u << actions.size()) - 1u;

    // The state every client starts from: the start scene without the Player templates.
    scene::Scene prepared = game.startScene;
    (void)runtime::extractPlayerTemplates(prepared);
    server->m_ids.assignInitial(prepared);
    server->m_initial = captureWorld(prepared, server->m_ids, 0);
    server->m_contentHash = net::contentHash(game.manifest, server->m_initial);
    if (auto started = server->startSession(); !started) return started.error();
    return server;
}

Status GameServer::startSession() {
    runtime::SessionOptions options;
    options.multiplayer = true;
    options.seed = m_options.seed;
    auto session = runtime::GameSession::create(m_game.manifest, m_game.startScene, m_game.scripts, options);
    if (!session) return session.error();
    m_session = std::move(session.value());
    m_history.clear();
    m_restartCountdown = -1;
    return {};
}

void GameServer::shutdown() {
    if (!m_endpoint) return;
    for (auto& [peer, client] : m_clients) {
        const auto reject = encode(RejectMessage{DisconnectReason::ShuttingDown, "The server is shutting down"});
        (void)m_endpoint->send(peer, Delivery::Reliable, reject);
        m_endpoint->disconnect(peer, static_cast<std::uint32_t>(DisconnectReason::ShuttingDown));
    }
    m_clients.clear();
    m_endpoint->flush();
}

std::size_t GameServer::playerCount() const {
    return static_cast<std::size_t>(
        std::count_if(m_clients.begin(), m_clients.end(), [](const auto& c) { return c.second.joined; }));
}

std::vector<ServerClientInfo> GameServer::clients() const {
    std::vector<ServerClientInfo> out;
    for (const auto& [peer, c] : m_clients) {
        if (!c.joined) continue;
        ServerClientInfo info;
        info.peer = peer;
        info.player = c.player;
        info.name = c.name;
        info.address = m_endpoint->describe(peer);
        info.stats = m_endpoint->stats(peer);
        info.queuedInputs = c.inputs.size();
        info.inputStalls = c.stalls;
        out.push_back(std::move(info));
    }
    return out;
}

void GameServer::tick() {
    receive();

    // Connections that never introduce themselves are closed.
    const auto helloTicks = static_cast<std::uint64_t>(std::max(0.0, m_options.helloTimeoutSeconds) * m_tickRate);
    std::vector<PeerId> silent;
    for (const auto& [peer, c] : m_clients) {
        if (!c.joined && m_ticksRun - c.connectedAtTick > helloTicks) silent.push_back(peer);
    }
    for (const PeerId peer : silent) reject(m_clients.at(peer), DisconnectReason::Timeout, "No hello received");

    applyInputs();
    m_session->tick();
    ++m_ticksRun;
    for (const auto& event : m_session->events()) {
        if (event.type == runtime::GameEvent::Type::LevelCompleted) {
            log::info("Level complete after {:.1f} s: {}", m_session->elapsedSeconds(), event.text);
        }
    }

    if (m_session->state().levelComplete && m_options.restartDelaySeconds >= 0.0) {
        if (m_restartCountdown < 0) {
            m_restartCountdown = std::max(1, static_cast<int>(m_options.restartDelaySeconds * m_tickRate));
        } else if (--m_restartCountdown == 0) {
            restartLevel();
        }
    }

    if (m_session->tickCount() % static_cast<std::uint64_t>(m_options.snapshotInterval) == 0) sendSnapshots();
    m_endpoint->flush();
}

void GameServer::receive() {
    m_events.clear();
    m_endpoint->poll(m_events);
    for (NetEvent& ev : m_events) {
        switch (ev.type) {
            case NetEvent::Type::Connected: {
                Client c;
                c.peer = ev.peer;
                c.connectedAtTick = m_ticksRun;
                m_clients[ev.peer] = std::move(c);
                log::debug("Connection from {}", m_endpoint->describe(ev.peer));
                break;
            }
            case NetEvent::Type::Disconnected:
                dropClient(ev.peer, describeDisconnect(ev.reason).c_str());
                break;
            case NetEvent::Type::Received: {
                const auto it = m_clients.find(ev.peer);
                if (it == m_clients.end()) break;
                if (ev.data.size() > kMaxClientMessageBytes) {
                    reject(it->second, DisconnectReason::BadMessage, "Message too large");
                    break;
                }
                handleMessage(it->second, ev.data);
                break;
            }
        }
    }
}

void GameServer::handleMessage(Client& client, const std::vector<std::uint8_t>& data) {
    const auto type = messageType(data);
    if (type == MessageType::Hello && !client.joined) {
        handleHello(client, data);
    } else if (type == MessageType::Input && client.joined) {
        handleInput(client, data);
    } else if (type == MessageType::Goodbye) {
        const PeerId peer = client.peer;
        m_endpoint->disconnect(peer, static_cast<std::uint32_t>(DisconnectReason::Left));
        dropClient(peer, "left the game");
    } else {
        reject(client, DisconnectReason::BadMessage, "Unexpected message");
    }
}

void GameServer::handleHello(Client& client, const std::vector<std::uint8_t>& data) {
    auto hello = decodeHello(data);
    if (!hello) {
        reject(client, DisconnectReason::BadMessage, hello.error().message);
        return;
    }
    if (hello->protocol != kProtocolVersion || hello->engineVersion != version::string) {
        reject(client, DisconnectReason::VersionMismatch,
               std::format("The server runs engine {} (protocol {}); you have {} (protocol {})", version::string,
                           kProtocolVersion, hello->engineVersion, hello->protocol));
        return;
    }
    if (hello->contentHash != m_contentHash) {
        reject(client, DisconnectReason::ContentMismatch,
               std::format("The server runs a different version of '{}'", m_game.manifest.name));
        return;
    }
    if (playerCount() >= m_options.maxPlayers) {
        reject(client, DisconnectReason::ServerFull, std::format("The server is full ({} players)", m_options.maxPlayers));
        return;
    }
    const runtime::PlayerId id = allocatePlayerId();
    std::string name = sanitizePlayerName(hello->playerName);
    if (name.empty()) name = std::format("Player {}", id);
    name = uniqueName(std::move(name));
    auto character = m_session->addPlayer(id, name);
    if (!character) {
        reject(client, DisconnectReason::ServerFull, character.error().message);
        return;
    }
    client.joined = true;
    client.player = id;
    client.name = name;
    WelcomeMessage welcome;
    welcome.player = id;
    welcome.tickRate = static_cast<std::uint16_t>(m_tickRate);
    welcome.snapshotInterval = static_cast<std::uint16_t>(m_options.snapshotInterval);
    welcome.serverName = m_options.name;
    (void)m_endpoint->send(client.peer, Delivery::Reliable, encode(welcome));
    log::info("{} joined as player {} from {} ({} playing)", name, id, m_endpoint->describe(client.peer),
              playerCount());
}

void GameServer::handleInput(Client& client, const std::vector<std::uint8_t>& data) {
    auto input = decodeInput(data);
    if (!input) {
        reject(client, DisconnectReason::BadMessage, input.error().message);
        return;
    }
    if (input->ackEpoch == m_epoch && input->ackTick <= m_session->tickCount() &&
        (client.ackEpoch != m_epoch || input->ackTick > client.ackTick)) {
        client.ackEpoch = m_epoch;
        client.ackTick = input->ackTick;
    }
    const std::size_t n = input->frames.size();
    const std::uint32_t first = input->lastSequence - static_cast<std::uint32_t>(n) + 1;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t sequence = first + static_cast<std::uint32_t>(i);
        if (sequence <= client.lastSequence || sequence > client.lastSequence + kMaxInputLead) continue;
        runtime::InputFrame frame = input->frames[i];
        frame.held &= m_actionMask;
        frame.pressed &= m_actionMask;
        client.inputs.emplace(sequence, frame);
    }
    while (client.inputs.size() > kMaxQueuedInputs) client.inputs.erase(client.inputs.begin());
}

void GameServer::applyInputs() {
    const std::size_t buffer = m_options.inputBufferFrames;
    for (auto& [peer, c] : m_clients) {
        if (!c.joined) continue;
        runtime::InputFrame frame{c.lastFrame.held, 0};
        if (!c.inputStarted && c.inputs.size() >= std::max<std::size_t>(1, buffer)) c.inputStarted = true;
        if (c.inputStarted) {
            if (c.inputs.empty()) {
                // The input for this tick is late: keep holding what was held,
                // and build the buffer up again before going on.
                ++c.stalls;
                c.inputStarted = false;
            } else {
                auto it = c.inputs.begin();
                frame = it->second;
                c.lastSequence = it->first;
                c.inputs.erase(it);
                // Running behind (a burst arrived, or the client's clock is
                // faster): merge frames to catch up. Presses are kept.
                while (c.inputs.size() > buffer + kCatchUpSlack) {
                    auto next = c.inputs.begin();
                    frame.held = next->second.held;
                    frame.pressed |= next->second.pressed;
                    c.lastSequence = next->first;
                    c.inputs.erase(next);
                }
            }
        }
        c.lastFrame = frame;
        m_session->setPlayerInput(c.player, frame);
    }
}

void GameServer::restartLevel() {
    struct Joined {
        runtime::PlayerId id;
        std::string name;
    };
    std::vector<Joined> players;
    for (const auto& p : m_session->players()) players.push_back({p.id, p.name});
    if (auto started = startSession(); !started) {
        log::error("Could not restart the level: {}", started.error().message);
        return;
    }
    ++m_epoch;
    for (const Joined& p : players) {
        if (auto added = m_session->addPlayer(p.id, p.name); !added) log::error("{}", added.error().message);
    }
    log::info("Level restarted ({} players)", players.size());
}

NetGameState GameServer::gameState() const {
    const auto& s = m_session->state();
    NetGameState g;
    g.score = s.score;
    g.objective = s.objective;
    g.levelComplete = s.levelComplete;
    g.completionMessage = s.completionMessage;
    g.respawns = s.respawns;
    g.scriptErrors = s.scriptErrors;
    g.lastScriptError = s.lastScriptError;
    for (const auto& p : m_session->players()) g.players.push_back({p.id, p.name});
    g.restartTicks = m_restartCountdown;
    return g;
}

void GameServer::sendSnapshots() {
    WorldState world = captureWorld(m_session->scene(), m_ids, static_cast<std::uint32_t>(m_session->tickCount()));
    world.game = gameState();
    m_history.push_back(world);
    while (m_history.size() > kSnapshotHistory) m_history.pop_front();

    // Clients that acknowledged the same state share one encoded change.
    std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> bodies;
    m_lastSnapshotBytes = 0;
    for (auto& [peer, c] : m_clients) {
        if (!c.joined) continue;
        const WorldState* baseline = &m_initial;
        if (c.ackEpoch == m_epoch && c.ackTick != 0) {
            for (const WorldState& h : m_history) {
                if (h.tick == c.ackTick) baseline = &h;
            }
        }
        auto& body = bodies[baseline->tick];
        if (body.empty()) {
            ByteWriter b;
            writeWorldDelta(b, *baseline, world);
            body = std::move(b.data());
        }
        ByteWriter w;
        writeSnapshotHeader(w, {m_epoch, world.tick, baseline->tick, c.lastSequence});
        w.bytes(body);
        m_lastSnapshotBytes = std::max(m_lastSnapshotBytes, w.size());
        (void)m_endpoint->send(peer, Delivery::Unreliable, w.data());
    }
}

void GameServer::reject(Client& client, DisconnectReason reason, const std::string& text) {
    const PeerId peer = client.peer;
    log::info("Refusing {}: {} ({})", m_endpoint->describe(peer), describeReason(reason), text);
    (void)m_endpoint->send(peer, Delivery::Reliable, encode(RejectMessage{reason, text}));
    m_endpoint->disconnect(peer, static_cast<std::uint32_t>(reason));
    dropClient(peer, describeReason(reason));
}

void GameServer::dropClient(PeerId peer, const char* why) {
    const auto it = m_clients.find(peer);
    if (it == m_clients.end()) return;
    if (it->second.joined) {
        m_session->removePlayer(it->second.player);
        log::info("{} left ({}); {} playing", it->second.name, why, playerCount() - 1);
    }
    m_clients.erase(it);
}

runtime::PlayerId GameServer::allocatePlayerId() {
    for (int attempt = 0; attempt < 65536; ++attempt) {
        const runtime::PlayerId id = m_nextPlayer;
        m_nextPlayer = static_cast<runtime::PlayerId>(m_nextPlayer == 0xFFFF ? 1 : m_nextPlayer + 1);
        if (id != runtime::kNoPlayer && m_session->findPlayer(id) == nullptr) return id;
    }
    return runtime::kNoPlayer; // cannot happen: at most kMaxPlayers are in use
}

std::string GameServer::uniqueName(std::string wanted) const {
    const auto taken = [this](const std::string& name) {
        return std::any_of(m_clients.begin(), m_clients.end(),
                           [&](const auto& c) { return c.second.joined && c.second.name == name; });
    };
    if (!taken(wanted)) return wanted;
    for (int n = 2;; ++n) {
        const std::string suffix = std::format(" {}", n);
        std::string base = wanted.substr(0, runtime::kMaxPlayerNameLength - suffix.size());
        if (!taken(base + suffix)) return base + suffix;
    }
}

} // namespace px::net
