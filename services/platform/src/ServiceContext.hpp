#pragma once

// Inside the platform service: request helpers, accounts and games shared by
// the handler groups (PlatformService.cpp, Social.cpp, Leaderboards.cpp, ...).
// Not part of the service's interface.
#include "Database.hpp"
#include "PlatformService.hpp"
#include "px/core/Log.hpp"
#include "px/crypto/Encoding.hpp"
#include "px/net/ConnectToken.hpp"
#include "px/online/WireJson.hpp"
#include "px/crypto/Crypto.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace px::platform {

namespace service {

using nlohmann::json;
using online::GameInfo;
using online::Role;
using online::UserInfo;
using online::Visibility;

// An answer other than success: thrown by handlers, turned into
// {"error": {"code": ..., "message": ...}} with the HTTP status.
struct HttpError {
    int status;
    std::string code;
    std::string message;
};

[[noreturn]] inline void fail(int status, std::string code, std::string message) {
    throw HttpError{status, std::move(code), std::move(message)};
}

inline std::int64_t now() { return net::unixTimeNow(); }

inline constexpr std::size_t kMaxJsonBody = 64 * 1024;

inline json parseBody(const httplib::Request& req, std::size_t limit = kMaxJsonBody) {
    if (req.body.size() > limit) fail(413, "too_large", "The request is too large");
    try {
        json j = req.body.empty() ? json::object() : json::parse(req.body);
        if (!j.is_object()) fail(400, "bad_request", "Expected a JSON object");
        return j;
    } catch (const json::exception&) {
        fail(400, "bad_request", "The request is not valid JSON");
    }
}

inline std::string stringField(const json& j, const char* name, std::size_t maxLength, bool required, const char* label) {
    const auto it = j.find(name);
    if (it == j.end() || it->is_null()) {
        if (required) fail(400, "bad_request", std::format("{} is required", label));
        return {};
    }
    if (!it->is_string()) fail(400, "bad_request", std::format("{} must be text", label));
    std::string value = it->get<std::string>();
    if (value.size() > maxLength) fail(400, "bad_request", std::format("{} is longer than {} characters", label, maxLength));
    return value;
}

// Titles and similar single-line text: trimmed, no control characters.
inline std::string cleanLine(std::string text, const char* label) {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            fail(400, "bad_request", std::format("{} cannot contain line breaks or control characters", label));
        }
    }
    const auto first = text.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(' ');
    return text.substr(first, last - first + 1);
}

inline std::string cleanText(std::string text, const char* label) {
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) < 0x20 && c != '\n') || c == 0x7F) {
            fail(400, "bad_request", std::format("{} cannot contain control characters", label));
        }
    }
    return text;
}

inline std::string likePattern(const std::string& words) {
    std::string out = "%";
    for (const char c : words) {
        if (c == '%' || c == '_' || c == '\\') out += '\\';
        out += c;
    }
    return out + "%";
}

inline std::string htmlEscape(std::string_view s) {
    std::string out;
    for (const char c : s) {
        switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

inline std::int64_t pathId(const httplib::Request& req, std::size_t index = 1) {
    try {
        return std::stoll(req.matches[static_cast<int>(index)].str());
    } catch (const std::exception&) {
        fail(404, "not_found", "Not found");
    }
}

inline constexpr const char* kGameColumns = R"(g.id, g.title, g.description, g.owner_id, u.username, g.visibility,
    g.game_type, g.max_players, g.latest_version,
    COALESCE((SELECT package_hash FROM versions v WHERE v.game_id = g.id AND v.number = g.latest_version), ''),
    g.plays, g.created, g.updated, g.hidden, g.hidden_note, u.banned, g.thumbnail)";

struct GameRow {
    GameInfo info;
    bool ownerBanned = false;
};

inline GameRow readGame(const Statement& s) {
    GameRow row;
    GameInfo& g = row.info;
    g.id = s.integer(0);
    g.title = s.text(1);
    g.description = s.text(2);
    g.ownerId = s.integer(3);
    g.ownerName = s.text(4);
    g.visibility = online::parseVisibility(s.text(5)).value_or(Visibility::Private);
    g.gameType = s.text(6);
    g.maxPlayers = static_cast<int>(s.integer(7));
    g.latestVersion = static_cast<int>(s.integer(8));
    g.packageHash = s.text(9);
    g.plays = s.integer(10);
    g.created = s.integer(11);
    g.updated = s.integer(12);
    g.hidden = s.integer(13) != 0;
    g.moderationNote = s.text(14);
    row.ownerBanned = s.integer(15) != 0;
    g.thumbnail = s.text(16);
    return row;
}

struct User {
    std::int64_t id = 0;
    std::string username;
    Role role = Role::User;
    bool banned = false;
    std::int64_t created = 0;

    [[nodiscard]] bool admin() const { return role == Role::Admin; }
    [[nodiscard]] UserInfo info(bool withBan = false) const {
        UserInfo u{id, username, role, created, withBan && banned};
        return u;
    }
};

inline void sendJson(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

} // namespace service

// Handler code uses the helpers above unqualified (internal header only).
using namespace service;

// What every handler group needs: the database, the account of a request,
// rate limits, the audit log, games and who may see them.
struct PlatformService::Context {
    PlatformService& s;
    std::string dummyHash; // verified against for unknown names, so timing does not reveal accounts

    explicit Context(PlatformService& service) : s(service) {}
    virtual ~Context() = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    Database& db() { return *s.m_db; }
    std::mutex& dbMutex() { return s.m_dbMutex; }
    Fleet& fleet() { return *s.m_fleet; }
    const ServiceConfig& config() const { return s.m_config; }

    // --- request plumbing ------------------------------------------------------------------
    using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;

    httplib::Server::Handler wrap(Handler handler) {
        return [this, handler](const httplib::Request& req, httplib::Response& res) {
            try {
                if (!s.m_limits.allow("ip:" + req.remote_addr, 600, 60)) {
                    fail(429, "rate_limited", "Too many requests; slow down");
                }
                handler(req, res);
            } catch (const HttpError& e) {
                sendJson(res, e.status, {{"error", {{"code", e.code}, {"message", e.message}}}});
            } catch (const std::exception& e) {
                log::error("{} {}: {}", req.method, req.path, e.what());
                sendJson(res, 500, {{"error", {{"code", "internal"}, {"message", "The platform had an internal error"}}}});
            }
        };
    }

    void limit(const std::string& key, double perWindow, double windowSeconds) {
        if (!s.m_limits.allow(key, perWindow, windowSeconds)) {
            fail(429, "rate_limited", "Too many attempts; please wait a little and try again");
        }
    }

    std::optional<User> userById(std::int64_t id) {
        auto st = db().prepare("SELECT id, username, role, banned, created FROM users WHERE id = ?1");
        st.bind(1, id);
        if (!st.step()) return std::nullopt;
        return User{st.integer(0), st.text(1), st.text(2) == "admin" ? Role::Admin : Role::User, st.integer(3) != 0,
                    st.integer(4)};
    }

    std::optional<User> userByName(std::string_view name) {
        auto st = db().prepare("SELECT id, username, role, banned, created FROM users WHERE username = ?1 COLLATE NOCASE");
        st.bind(1, name);
        if (!st.step()) return std::nullopt;
        return User{st.integer(0), st.text(1), st.text(2) == "admin" ? Role::Admin : Role::User, st.integer(3) != 0,
                    st.integer(4)};
    }

    static std::string bearer(const httplib::Request& req, std::string_view scheme) {
        const std::string header = req.get_header_value("Authorization");
        const std::string prefix = std::string(scheme) + " ";
        return header.starts_with(prefix) ? header.substr(prefix.size()) : std::string();
    }

    std::optional<User> currentUser(const httplib::Request& req) {
        const std::string token = bearer(req, "Bearer");
        if (token.empty() || token.size() > 100) return std::nullopt;
        const std::string hash = crypto::toHex(crypto::hash256(token));
        std::lock_guard lock(s.m_dbMutex);
        auto st = db().prepare("SELECT user_id, expires FROM sessions WHERE token_hash = ?1");
        st.bind(1, hash);
        if (!st.step()) return std::nullopt;
        if (st.integer(1) < now()) return std::nullopt;
        auto user = userById(st.integer(0));
        if (user && user->banned) fail(403, "banned", "This account is banned");
        if (user) {
            // Presence: "online" means seen in the last two minutes.
            auto seen = db().prepare("UPDATE users SET last_seen = ?1 WHERE id = ?2 AND last_seen < ?3");
            seen.bind(1, now()).bind(2, user->id).bind(3, now() - 30);
            seen.run();
        }
        return user;
    }

    User requireUser(const httplib::Request& req) {
        auto user = currentUser(req);
        if (!user) fail(401, "not_logged_in", "Log in first");
        return *user;
    }

    // The session of the game server making the request ("Authorization: Server <key>").
    std::string requireServer(const httplib::Request& req) {
        const std::string key = bearer(req, "Server");
        auto session = key.empty() || key.size() > 100 ? std::nullopt : s.m_fleet->sessionForKey(key);
        if (!session) fail(401, "not_a_server", "Unknown game server");
        return *session;
    }

    User requireAdmin(const httplib::Request& req) {
        User user = requireUser(req);
        if (!user.admin()) fail(403, "forbidden", "Only administrators can do this");
        return user;
    }

    std::string createSession(std::int64_t userId) {
        auto token = crypto::randomToken(32);
        if (!token) throw std::runtime_error(token.error().message);
        auto st = db().prepare("INSERT INTO sessions (token_hash, user_id, created, expires) VALUES (?1, ?2, ?3, ?4)");
        st.bind(1, crypto::toHex(crypto::hash256(token.value())))
            .bind(2, userId)
            .bind(3, now())
            .bind(4, now() + std::int64_t{s.m_config.sessionDays} * 86400);
        st.run();
        return token.value();
    }

    void audit(const User& actor, std::string_view action, std::string_view target, std::string_view detail) {
        auto st = db().prepare("INSERT INTO audit (at, actor_id, action, target, detail) VALUES (?1, ?2, ?3, ?4, ?5)");
        st.bind(1, now()).bind(2, actor.id).bind(3, action).bind(4, target).bind(5, detail);
        st.run();
        log::info("audit: {} {} {} {}", actor.username, action, target, detail);
    }

    // An account an administrator bans: logged out everywhere, their games' servers stop.
    void banUser(const User& admin, const User& target, bool banned, const std::string& note) {
        if (target.admin() && banned) fail(400, "bad_request", "Administrators cannot be banned; remove their role first");
        auto st = db().prepare("UPDATE users SET banned = ?1, ban_note = ?2 WHERE id = ?3");
        st.bind(1, banned ? 1 : 0).bind(2, note).bind(3, target.id);
        st.run();
        if (banned) {
            auto del = db().prepare("DELETE FROM sessions WHERE user_id = ?1");
            del.bind(1, target.id);
            del.run();
            auto games = db().prepare("SELECT id FROM games WHERE owner_id = ?1");
            games.bind(1, target.id);
            while (games.step()) s.m_fleet->stopGame(games.integer(0));
        }
        audit(admin, banned ? "ban_user" : "unban_user", "user " + target.username, note);
    }

    // Accounts that blocked `userId` or that it blocked (as fleet user ids). Call with the database locked.
    std::set<std::string> blockedWith(std::int64_t userId) {
        std::set<std::string> out;
        auto st = db().prepare("SELECT blocked_id FROM blocks WHERE user_id = ?1 UNION SELECT user_id FROM blocks WHERE blocked_id = ?1");
        st.bind(1, userId);
        while (st.step()) out.insert(std::to_string(st.integer(0)));
        return out;
    }

    // A seat the fleet could not give, as an answer for the player.
    [[noreturn]] static void failSeat(const Error& error) {
        const int status = error.code == ErrorCode::LimitExceeded   ? 503
                           : error.code == ErrorCode::InvalidArgument ? 409
                           : error.code == ErrorCode::NotFound        ? 409
                                                                      : 502;
        fail(status, "no_server", error.message);
    }

    // The signed ticket for a seat; counts a play of the game.
    online::JoinTicket issueTicket(const User& user, std::int64_t gameId, int version, const std::string& packageHash,
                                   const Seat& seat) {
        net::ConnectToken token;
        token.session = seat.session;
        token.userId = std::to_string(user.id);
        token.username = user.username;
        token.expires = now() + net::kConnectTokenLifetimeSeconds;
        auto nonce = crypto::randomToken(16);
        if (!nonce) throw std::runtime_error(nonce.error().message);
        token.nonce = nonce.value();
        online::JoinTicket ticket;
        ticket.token = net::signConnectToken(token, s.m_key);
        ticket.host = seat.host;
        ticket.port = seat.port;
        ticket.session = seat.session;
        ticket.expires = token.expires;
        ticket.version = version;
        ticket.packageHash = packageHash;
        ticket.webSocket = seat.webSocket;
        std::lock_guard lock(s.m_dbMutex);
        auto st = db().prepare("UPDATE games SET plays = plays + 1 WHERE id = ?1");
        st.bind(1, gameId);
        st.run();
        return ticket;
    }

    // --- games ---------------------------------------------------------------------------
    std::optional<GameRow> gameById(std::int64_t id) {
        auto st = db().prepare(std::format("SELECT {} FROM games g JOIN users u ON u.id = g.owner_id WHERE g.id = ?1",
                                           kGameColumns));
        st.bind(1, id);
        if (!st.step()) return std::nullopt;
        return readGame(st);
    }

    static bool owns(const std::optional<User>& user, const GameInfo& g) { return user && user->id == g.ownerId; }

    // Whether `user` may see (and play) the game.
    static bool mayView(const std::optional<User>& user, const GameRow& row) {
        if (user && (user->admin() || user->id == row.info.ownerId)) return true;
        if (row.info.hidden || row.ownerBanned) return false;
        return row.info.visibility != Visibility::Private;
    }

    GameRow viewableGame(std::int64_t id, const std::optional<User>& user) {
        std::lock_guard lock(s.m_dbMutex);
        auto row = gameById(id);
        if (!row || !mayView(user, *row)) fail(404, "not_found", "There is no such game");
        return *row;
    }

    json gameJson(GameRow row, const std::optional<User>& viewer) {
        row.info.playing = s.m_fleet->playing(row.info.id);
        const bool privileged = viewer && (viewer->admin() || viewer->id == row.info.ownerId);
        if (!privileged) {
            row.info.hidden = false;
            row.info.moderationNote.clear();
        }
        return row.info;
    }

};

} // namespace px::platform
