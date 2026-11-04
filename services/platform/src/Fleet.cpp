#include "Fleet.hpp"

#include "px/core/Log.hpp"
#include "px/crypto/Crypto.hpp"
#include "px/crypto/Encoding.hpp"

#include <format>

namespace px::platform {

namespace {
std::string keyHash(std::string_view key) { return crypto::toHex(crypto::hash256(key)); }
} // namespace

Fleet::Fleet(FleetConfig config) : m_config(std::move(config)) {
    std::error_code ec;
    std::filesystem::create_directories(m_config.logDir, ec);
}

Fleet::~Fleet() { stopAll(); }

Fleet::Session* Fleet::find(const std::string& id) {
    const auto it = m_sessions.find(id);
    return it == m_sessions.end() ? nullptr : it->second.get();
}
const Fleet::Session* Fleet::find(const std::string& id) const {
    const auto it = m_sessions.find(id);
    return it == m_sessions.end() ? nullptr : it->second.get();
}

std::size_t Fleet::occupancy(const Session& s) const {
    std::set<std::string> seats(s.players.begin(), s.players.end());
    for (const auto& [user, until] : s.reserved) seats.insert(user);
    return seats.size();
}

Result<Fleet::Session*> Fleet::startSession(const GameRef& game) {
    std::size_t active = 0;
    std::set<std::uint16_t> used;
    for (const auto& [id, s] : m_sessions) {
        if (alive(*s)) ++active;
        used.insert(s->port);
    }
    if (active >= m_config.maxServers) {
        return makeError(ErrorCode::LimitExceeded, "All game servers are busy; try again in a minute");
    }
    std::uint16_t port = 0;
    for (std::uint16_t i = 0; i < m_config.portCount; ++i) {
        const auto candidate = static_cast<std::uint16_t>(m_config.firstPort + i);
        if (!used.contains(candidate)) {
            port = candidate;
            break;
        }
    }
    if (port == 0) return makeError(ErrorCode::LimitExceeded, "No free port for another game server");

    auto session = std::make_unique<Session>();
    std::array<std::uint8_t, 8> idBytes{};
    if (auto s = crypto::randomBytes(idBytes); !s) return s.error();
    session->id = crypto::toHex(idBytes);
    auto key = crypto::randomToken(32);
    if (!key) return key.error();
    session->keyHash = keyHash(key.value());
    session->game = game;
    session->port = port;
    session->started = Clock::now();
    session->lastHeartbeat = session->started;

    ProcessOptions options;
    options.args = {game.package.string(),
                    "--port",
                    std::to_string(port),
                    "--max-players",
                    std::to_string(game.maxPlayers),
                    "--name",
                    game.title,
                    "--platform",
                    m_config.platformUrl,
                    "--session",
                    session->id,
                    "--platform-key",
                    m_config.platformKeyHex};
    options.env = {{"PX_SERVER_KEY", key.value()}}; // not on the command line, where other users could read it
    options.outputFile = m_config.logDir / std::format("server-{}.log", session->id);
    options.memoryLimitMiB = m_config.memoryLimitMiB;
    auto process = Process::start(m_config.serverExe, options);
    if (!process) {
        log::error("Cannot start a game server: {}", process.error().message);
        return makeError(ErrorCode::IoError, "The platform could not start a game server");
    }
    session->process = std::move(process.value());
    log::info("Game server {} started for game {} version {} on port {} (pid {})", session->id, game.gameId,
              game.version, port, session->process->pid());
    Session* raw = session.get();
    m_sessions.emplace(raw->id, std::move(session));
    return raw;
}

Result<Seat> Fleet::reserve(const GameRef& game, const std::string& userId) {
    std::unique_lock lock(m_mutex);
    Session* chosen = nullptr;
    if (const auto it = m_userSession.find(userId); it != m_userSession.end()) {
        Session* current = find(it->second);
        if (current != nullptr && alive(*current) && !current->stopRequested) {
            if (current->game.gameId == game.gameId && current->game.version == game.version) {
                chosen = current; // back to the same server (a reconnect)
            } else {
                return makeError(ErrorCode::InvalidArgument,
                                 "You are still in another game ('{}'). Leave it first, or wait a minute.",
                                 current->game.title);
            }
        } else if (current != nullptr && alive(*current)) {
            return makeError(ErrorCode::InvalidArgument,
                             "Your last game is still saving your progress; try again in a moment");
        } else {
            m_userSession.erase(it);
        }
    }
    if (chosen == nullptr) {
        for (auto& [id, s] : m_sessions) {
            if (alive(*s) && !s->stopRequested && s->game.gameId == game.gameId && s->game.version == game.version &&
                occupancy(*s) < static_cast<std::size_t>(game.maxPlayers)) {
                chosen = s.get();
                break;
            }
        }
    }
    if (chosen == nullptr) {
        auto started = startSession(game);
        if (!started) return started.error();
        chosen = started.value();
    }
    chosen->reserved[userId] = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                                  std::chrono::duration<double>(m_config.seatSeconds));
    m_userSession[userId] = chosen->id;
    const std::string id = chosen->id;
    const auto deadline = chosen->started + std::chrono::duration_cast<Clock::duration>(
                                                std::chrono::duration<double>(m_config.readySeconds));
    // Wait for a new server to say it is listening.
    while (true) {
        Session* s = find(id);
        if (s == nullptr || !alive(*s)) {
            if (m_userSession.contains(userId) && m_userSession[userId] == id) m_userSession.erase(userId);
            return makeError(ErrorCode::IoError, "The game server stopped before it was ready; try again");
        }
        if (s->state == Session::State::Ready) return Seat{s->id, m_config.publicHost, s->port};
        if (s->process->exitCode()) {
            endSession(*s, "exited while starting");
            continue;
        }
        if (Clock::now() >= deadline) {
            endSession(*s, "not ready in time");
            continue;
        }
        m_changed.wait_for(lock, std::chrono::milliseconds(100));
    }
}

std::optional<std::string> Fleet::sessionForKey(std::string_view serverKey) const {
    const std::string hash = keyHash(serverKey);
    std::lock_guard lock(m_mutex);
    for (const auto& [id, s] : m_sessions) {
        if (alive(*s) && crypto::constantTimeEqual(crypto::bytesOf(s->keyHash), crypto::bytesOf(hash))) return id;
    }
    return std::nullopt;
}

void Fleet::ready(const std::string& session, std::uint16_t port) {
    std::lock_guard lock(m_mutex);
    Session* s = find(session);
    if (s == nullptr || !alive(*s)) return;
    if (port != s->port) log::warn("Game server {} reports port {}, expected {}", session, port, s->port);
    s->state = Session::State::Ready;
    s->lastHeartbeat = Clock::now();
    m_changed.notify_all();
}

bool Fleet::heartbeat(const std::string& session, const std::vector<std::string>& players) {
    std::lock_guard lock(m_mutex);
    Session* s = find(session);
    if (s == nullptr || !alive(*s)) return true;
    const auto now = Clock::now();
    s->lastHeartbeat = now;
    s->players.clear();
    for (const std::string& p : players) {
        // Only players the platform sent here count; anything else is ignored.
        const auto lockIt = m_userSession.find(p);
        if (lockIt == m_userSession.end() || lockIt->second != session) continue;
        s->players.insert(p);
        s->reserved.erase(p);
        s->lastSeen[p] = now;
    }
    return s->stopRequested;
}

void Fleet::endSession(Session& s, const char* why) {
    if (!alive(s)) return;
    s.state = Session::State::Ended;
    if (s.process) s.process->kill();
    for (auto it = m_userSession.begin(); it != m_userSession.end();) {
        it = it->second == s.id ? m_userSession.erase(it) : std::next(it);
    }
    log::info("Game server {} ended ({})", s.id, why);
    m_changed.notify_all();
}

void Fleet::ended(const std::string& session) {
    std::lock_guard lock(m_mutex);
    if (Session* s = find(session)) {
        // A clean exit: give the process a moment to finish by itself.
        if (s->process) (void)s->process->wait(2.0);
        endSession(*s, "the server finished");
    }
}

bool Fleet::holdsPlayer(const std::string& session, const std::string& userId) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_userSession.find(userId);
    return it != m_userSession.end() && it->second == session;
}

void Fleet::release(const std::string& session, const std::string& userId) {
    std::lock_guard lock(m_mutex);
    const auto it = m_userSession.find(userId);
    if (it != m_userSession.end() && it->second == session) m_userSession.erase(it);
    if (Session* s = find(session)) {
        s->players.erase(userId);
        s->reserved.erase(userId);
        s->lastSeen.erase(userId);
    }
    m_changed.notify_all();
}

std::optional<std::int64_t> Fleet::gameOf(const std::string& session) const {
    std::lock_guard lock(m_mutex);
    const Session* s = find(session);
    if (s == nullptr) return std::nullopt;
    return s->game.gameId;
}

int Fleet::playing(std::int64_t gameId) const {
    std::lock_guard lock(m_mutex);
    int n = 0;
    for (const auto& [id, s] : m_sessions) {
        if (alive(*s) && s->game.gameId == gameId) n += static_cast<int>(s->players.size());
    }
    return n;
}

std::map<std::int64_t, int> Fleet::playingByGame() const {
    std::lock_guard lock(m_mutex);
    std::map<std::int64_t, int> out;
    for (const auto& [id, s] : m_sessions) {
        if (alive(*s)) out[s->game.gameId] += static_cast<int>(s->players.size());
    }
    return out;
}

void Fleet::stopGame(std::int64_t gameId) {
    std::lock_guard lock(m_mutex);
    for (auto& [id, s] : m_sessions) {
        if (alive(*s) && s->game.gameId == gameId) s->stopRequested = true;
    }
}

void Fleet::stopAll() {
    std::lock_guard lock(m_mutex);
    for (auto& [id, s] : m_sessions) endSession(*s, "the platform is stopping");
}

void Fleet::maintain() {
    std::lock_guard lock(m_mutex);
    const auto now = Clock::now();
    const auto seconds = [](double s) {
        return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s));
    };
    for (auto& [id, s] : m_sessions) {
        if (!alive(*s)) continue;
        if (s->process && s->process->exitCode()) {
            endSession(*s, std::format("exited with code {}", *s->process->exitCode()).c_str());
            continue;
        }
        if (s->state == Session::State::Starting && now - s->started > seconds(m_config.readySeconds)) {
            endSession(*s, "not ready in time");
            continue;
        }
        if (s->state == Session::State::Ready && now - s->lastHeartbeat > seconds(m_config.heartbeatSeconds)) {
            endSession(*s, "no heartbeat");
            continue;
        }
        if (s->stopRequested && now - s->lastHeartbeat > seconds(m_config.heartbeatSeconds)) {
            endSession(*s, "asked to stop");
            continue;
        }
        for (auto it = s->reserved.begin(); it != s->reserved.end();) {
            if (it->second >= now) {
                ++it;
                continue;
            }
            // The player never arrived: free the seat and their lock.
            const auto lockIt = m_userSession.find(it->first);
            if (lockIt != m_userSession.end() && lockIt->second == id && !s->players.contains(it->first)) {
                m_userSession.erase(lockIt);
            }
            it = s->reserved.erase(it);
        }
        // A player gone for two minutes whose server never released them.
        for (auto it = s->lastSeen.begin(); it != s->lastSeen.end();) {
            if (!s->players.contains(it->first) && now - it->second > std::chrono::minutes(2)) {
                const auto lockIt = m_userSession.find(it->first);
                if (lockIt != m_userSession.end() && lockIt->second == id) m_userSession.erase(lockIt);
                it = s->lastSeen.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Forget ended sessions (their ports become free).
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        it = alive(*it->second) ? std::next(it) : m_sessions.erase(it);
    }
}

std::vector<SessionInfo> Fleet::sessions() const {
    std::lock_guard lock(m_mutex);
    std::vector<SessionInfo> out;
    for (const auto& [id, s] : m_sessions) {
        SessionInfo info;
        info.id = id;
        info.gameId = s->game.gameId;
        info.version = s->game.version;
        info.port = s->port;
        info.state = s->state == Session::State::Starting ? "starting" : s->state == Session::State::Ready ? "ready" : "ended";
        info.players = s->players.size();
        info.reserved = s->reserved.size();
        info.pid = s->process ? s->process->pid() : 0;
        out.push_back(info);
    }
    return out;
}

} // namespace px::platform
