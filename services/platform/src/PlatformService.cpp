#include "PlatformService.hpp"

#include "Database.hpp"
#include "px/core/Log.hpp"
#include "px/core/Version.hpp"
#include "px/crypto/Encoding.hpp"
#include "px/net/ConnectToken.hpp"
#include "px/online/PackageCheck.hpp"
#include "px/online/PlatformClient.hpp"
#include "px/online/WireJson.hpp"
#include "px/project/Package.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <format>
#include <fstream>
#include <functional>

namespace px::platform {

using nlohmann::json;
using online::GameInfo;
using online::Role;
using online::UserInfo;
using online::Visibility;

namespace {

// An answer other than success: thrown by handlers, turned into
// {"error": {"code": ..., "message": ...}} with the HTTP status.
struct HttpError {
    int status;
    std::string code;
    std::string message;
};

[[noreturn]] void fail(int status, std::string code, std::string message) {
    throw HttpError{status, std::move(code), std::move(message)};
}

std::int64_t now() { return net::unixTimeNow(); }

constexpr std::size_t kMaxJsonBody = 64 * 1024;

json parseBody(const httplib::Request& req, std::size_t limit = kMaxJsonBody) {
    if (req.body.size() > limit) fail(413, "too_large", "The request is too large");
    try {
        json j = req.body.empty() ? json::object() : json::parse(req.body);
        if (!j.is_object()) fail(400, "bad_request", "Expected a JSON object");
        return j;
    } catch (const json::exception&) {
        fail(400, "bad_request", "The request is not valid JSON");
    }
}

std::string stringField(const json& j, const char* name, std::size_t maxLength, bool required, const char* label) {
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
std::string cleanLine(std::string text, const char* label) {
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

std::string cleanText(std::string text, const char* label) {
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) < 0x20 && c != '\n') || c == 0x7F) {
            fail(400, "bad_request", std::format("{} cannot contain control characters", label));
        }
    }
    return text;
}

std::string likePattern(const std::string& words) {
    std::string out = "%";
    for (const char c : words) {
        if (c == '%' || c == '_' || c == '\\') out += '\\';
        out += c;
    }
    return out + "%";
}

std::string htmlEscape(std::string_view s) {
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

std::int64_t pathId(const httplib::Request& req, std::size_t index = 1) {
    try {
        return std::stoll(req.matches[static_cast<int>(index)].str());
    } catch (const std::exception&) {
        fail(404, "not_found", "Not found");
    }
}

constexpr const char* kGameColumns = R"(g.id, g.title, g.description, g.owner_id, u.username, g.visibility,
    g.game_type, g.max_players, g.latest_version,
    COALESCE((SELECT package_hash FROM versions v WHERE v.game_id = g.id AND v.number = g.latest_version), ''),
    g.plays, g.created, g.updated, g.hidden, g.hidden_note, u.banned)";

struct GameRow {
    GameInfo info;
    bool ownerBanned = false;
};

GameRow readGame(const Statement& s) {
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

void sendJson(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

} // namespace

// =============================================================================================

struct PlatformService::Impl {
    PlatformService& s;
    std::string dummyHash; // verified against for unknown names, so timing does not reveal accounts

    explicit Impl(PlatformService& service) : s(service) {}

    Database& db() { return *s.m_db; }

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
        return user;
    }

    User requireUser(const httplib::Request& req) {
        auto user = currentUser(req);
        if (!user) fail(401, "not_logged_in", "Log in first");
        return *user;
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

    // --- handlers: accounts -------------------------------------------------------------
    void registerAccount(const httplib::Request& req, httplib::Response& res) {
        limit("register:" + req.remote_addr, 10, 3600);
        const json body = parseBody(req);
        const std::string username = stringField(body, "username", 64, true, "The name");
        const std::string password = stringField(body, "password", online::kMaxPasswordLength, true, "The password");
        if (!online::isValidUsername(username)) {
            fail(400, "bad_username", "Names are 3 to 20 letters, digits or '_'");
        }
        if (password.size() < online::kMinPasswordLength) {
            fail(400, "weak_password", std::format("Passwords need at least {} characters", online::kMinPasswordLength));
        }
        {
            std::lock_guard lock(s.m_dbMutex);
            if (userByName(username)) fail(409, "name_taken", "That name is taken");
        }
        auto hash = crypto::hashPassword(password, s.m_config.passwordHashing); // slow: outside the lock
        if (!hash) throw std::runtime_error(hash.error().message);
        std::lock_guard lock(s.m_dbMutex);
        Database::Transaction tx(db());
        if (userByName(username)) fail(409, "name_taken", "That name is taken");
        auto st = db().prepare("INSERT INTO users (username, password_hash, role, created) VALUES (?1, ?2, 'user', ?3)");
        st.bind(1, username).bind(2, hash.value()).bind(3, now());
        st.run();
        const std::int64_t id = db().lastInsertId();
        const std::string token = createSession(id);
        tx.commit();
        auto user = userById(id);
        log::info("New account '{}' ({})", username, id);
        sendJson(res, 201, {{"user", user->info()}, {"token", token}});
    }

    void login(const httplib::Request& req, httplib::Response& res) {
        limit("login:" + req.remote_addr, 20, 60);
        const json body = parseBody(req);
        const std::string username = stringField(body, "username", 64, true, "The name");
        const std::string password = stringField(body, "password", online::kMaxPasswordLength, true, "The password");
        limit("login-name:" + username, 10, 60);
        std::optional<User> user;
        std::string stored;
        {
            std::lock_guard lock(s.m_dbMutex);
            user = userByName(username);
            if (user) {
                auto st = db().prepare("SELECT password_hash FROM users WHERE id = ?1");
                st.bind(1, user->id);
                if (st.step()) stored = st.text(0);
            }
        }
        const bool ok = crypto::verifyPassword(password, user ? stored : dummyHash);
        if (!user || !ok) fail(401, "bad_login", "Wrong name or password");
        if (user->banned) fail(403, "banned", "This account is banned");
        std::lock_guard lock(s.m_dbMutex);
        const std::string token = createSession(user->id);
        sendJson(res, 200, {{"user", user->info()}, {"token", token}});
    }

    void logout(const httplib::Request& req, httplib::Response& res) {
        const std::string token = bearer(req, "Bearer");
        if (!token.empty()) {
            std::lock_guard lock(s.m_dbMutex);
            auto st = db().prepare("DELETE FROM sessions WHERE token_hash = ?1");
            st.bind(1, crypto::toHex(crypto::hash256(token)));
            st.run();
        }
        sendJson(res, 200, json::object());
    }

    void me(const httplib::Request& req, httplib::Response& res) {
        const User user = requireUser(req);
        sendJson(res, 200, {{"user", user.info()}});
    }

    // --- handlers: games --------------------------------------------------------------------
    void listGames(const httplib::Request& req, httplib::Response& res) {
        const auto viewer = currentUser(req);
        const std::string search = req.get_param_value("search").substr(0, 100);
        const std::string sort = req.get_param_value("sort");
        int limitCount = 50;
        int offset = 0;
        try {
            if (req.has_param("limit")) limitCount = std::clamp(std::stoi(req.get_param_value("limit")), 1, 100);
            if (req.has_param("offset")) offset = std::clamp(std::stoi(req.get_param_value("offset")), 0, 100000);
        } catch (const std::exception&) {
            fail(400, "bad_request", "limit and offset must be numbers");
        }
        std::vector<GameRow> rows;
        {
            std::lock_guard lock(s.m_dbMutex);
            const char* order = sort == "recent" ? "g.updated DESC" : "g.plays DESC, g.updated DESC";
            auto st = db().prepare(std::format(
                "SELECT {} FROM games g JOIN users u ON u.id = g.owner_id WHERE g.visibility = 'public' AND "
                "g.hidden = 0 AND u.banned = 0 AND g.latest_version > 0 AND (?1 = '' OR g.title LIKE ?2 ESCAPE '\\' "
                "OR g.description LIKE ?2 ESCAPE '\\') ORDER BY {} LIMIT ?3 OFFSET ?4",
                kGameColumns, order));
            st.bind(1, search).bind(2, likePattern(search)).bind(3, limitCount).bind(4, offset);
            while (st.step()) rows.push_back(readGame(st));
        }
        json games = json::array();
        for (auto& r : rows) games.push_back(gameJson(r, viewer));
        if (sort == "playing") {
            std::stable_sort(games.begin(), games.end(),
                             [](const json& a, const json& b) { return a["playing"].get<int>() > b["playing"].get<int>(); });
        }
        sendJson(res, 200, {{"games", games}});
    }

    void myGames(const httplib::Request& req, httplib::Response& res) {
        const User user = requireUser(req);
        std::vector<GameRow> rows;
        {
            std::lock_guard lock(s.m_dbMutex);
            auto st = db().prepare(std::format(
                "SELECT {} FROM games g JOIN users u ON u.id = g.owner_id WHERE g.owner_id = ?1 ORDER BY g.updated DESC",
                kGameColumns));
            st.bind(1, user.id);
            while (st.step()) rows.push_back(readGame(st));
        }
        json games = json::array();
        for (auto& r : rows) games.push_back(gameJson(r, user));
        sendJson(res, 200, {{"games", games}});
    }

    void getGame(const httplib::Request& req, httplib::Response& res) {
        const auto viewer = currentUser(req);
        const GameRow row = viewableGame(pathId(req), viewer);
        sendJson(res, 200, {{"game", gameJson(row, viewer)}});
    }

    void createGame(const httplib::Request& req, httplib::Response& res) {
        const User user = requireUser(req);
        limit("create-game:" + std::to_string(user.id), 20, 3600);
        const json body = parseBody(req);
        const std::string title =
            cleanLine(stringField(body, "title", online::kMaxGameTitleLength, true, "The title"), "The title");
        if (title.empty()) fail(400, "bad_request", "The title cannot be empty");
        const std::string description = cleanText(
            stringField(body, "description", online::kMaxGameDescriptionLength, false, "The description"),
            "The description");
        Visibility visibility = Visibility::Public;
        if (body.contains("visibility")) {
            const auto v = online::parseVisibility(stringField(body, "visibility", 20, true, "visibility"));
            if (!v) fail(400, "bad_request", "visibility is \"public\", \"unlisted\" or \"private\"");
            visibility = *v;
        }
        std::lock_guard lock(s.m_dbMutex);
        auto count = db().prepare("SELECT COUNT(*) FROM games WHERE owner_id = ?1");
        count.bind(1, user.id);
        count.step();
        if (static_cast<std::size_t>(count.integer(0)) >= s.m_config.maxGamesPerUser) {
            fail(429, "too_many_games", std::format("You can have at most {} games", s.m_config.maxGamesPerUser));
        }
        auto st = db().prepare(
            "INSERT INTO games (owner_id, title, description, visibility, created, updated) VALUES (?1, ?2, ?3, ?4, ?5, ?5)");
        st.bind(1, user.id).bind(2, title).bind(3, description).bind(4, online::visibilityName(visibility)).bind(5, now());
        st.run();
        auto row = gameById(db().lastInsertId());
        sendJson(res, 201, {{"game", gameJson(*row, user)}});
    }

    void updateGame(const httplib::Request& req, httplib::Response& res) {
        const User user = requireUser(req);
        const std::int64_t id = pathId(req);
        const json body = parseBody(req);
        std::lock_guard lock(s.m_dbMutex);
        auto row = gameById(id);
        if (!row || !(user.admin() || row->info.ownerId == user.id)) fail(404, "not_found", "There is no such game");
        if (body.contains("title")) {
            const std::string title =
                cleanLine(stringField(body, "title", online::kMaxGameTitleLength, true, "The title"), "The title");
            if (title.empty()) fail(400, "bad_request", "The title cannot be empty");
            auto st = db().prepare("UPDATE games SET title = ?1 WHERE id = ?2");
            st.bind(1, title).bind(2, id);
            st.run();
        }
        if (body.contains("description")) {
            auto st = db().prepare("UPDATE games SET description = ?1 WHERE id = ?2");
            st.bind(1, cleanText(stringField(body, "description", online::kMaxGameDescriptionLength, true, "The description"),
                                 "The description"))
                .bind(2, id);
            st.run();
        }
        if (body.contains("visibility")) {
            const auto v = online::parseVisibility(stringField(body, "visibility", 20, true, "visibility"));
            if (!v) fail(400, "bad_request", "visibility is \"public\", \"unlisted\" or \"private\"");
            auto st = db().prepare("UPDATE games SET visibility = ?1 WHERE id = ?2");
            st.bind(1, online::visibilityName(*v)).bind(2, id);
            st.run();
        }
        if (body.contains("maxPlayers")) {
            if (!body["maxPlayers"].is_number_integer()) fail(400, "bad_request", "maxPlayers must be a whole number");
            const int n = body["maxPlayers"].get<int>();
            if (n < 1 || n > 64) fail(400, "bad_request", "maxPlayers must be 1 to 64");
            auto st = db().prepare("UPDATE games SET max_players = ?1 WHERE id = ?2");
            st.bind(1, n).bind(2, id);
            st.run();
        }
        auto touch = db().prepare("UPDATE games SET updated = ?1 WHERE id = ?2");
        touch.bind(1, now()).bind(2, id);
        touch.run();
        sendJson(res, 200, {{"game", gameJson(*gameById(id), user)}});
    }

    std::filesystem::path packagePath(const std::string& hash) const {
        return s.m_config.dataDir / "packages" / hash.substr(0, 2) / (hash + ".pxpkg");
    }

    void publish(const httplib::Request& req, httplib::Response& res) {
        const User user = requireUser(req);
        const std::int64_t id = pathId(req);
        {
            std::lock_guard lock(s.m_dbMutex);
            auto row = gameById(id);
            if (!row || row->info.ownerId != user.id) fail(404, "not_found", "There is no such game of yours");
        }
        limit("publish:" + std::to_string(user.id), 30, 3600);
        if (req.body.size() > project::kMaxPackageBytes) fail(413, "too_large", "The game is too large");
        if (req.body.size() < 12) fail(400, "bad_request", "Send the game package as the request body");
        const std::string notes = cleanText(req.get_param_value("notes").substr(0, 500), "The notes");

        // The upload goes to a file and is checked by px-headless in its own,
        // limited process; only its re-encoded output is kept.
        const auto tmpDir = s.m_config.dataDir / "tmp";
        std::error_code ec;
        std::filesystem::create_directories(tmpDir, ec);
        auto name = crypto::randomToken(12);
        if (!name) throw std::runtime_error(name.error().message);
        const auto upload = tmpDir / (name.value() + ".upload.pxpkg");
        const auto checked = tmpDir / (name.value() + ".checked.pxpkg");
        struct Cleanup {
            std::filesystem::path a, b;
            ~Cleanup() {
                std::error_code e;
                std::filesystem::remove(a, e);
                std::filesystem::remove(b, e);
            }
        } cleanup{upload, checked};
        {
            std::ofstream out(upload, std::ios::binary | std::ios::trunc);
            out.write(req.body.data(), static_cast<std::streamsize>(req.body.size()));
            if (!out) throw std::runtime_error("cannot write the upload");
        }
        ProcessOptions options;
        options.args = {"--check-package", upload.string(), "--output", checked.string(), "--ticks",
                        std::to_string(s.m_config.checkTicks)};
        options.memoryLimitMiB = s.m_config.checkMemoryMiB;
        options.cpuLimitSeconds = static_cast<int>(s.m_config.checkTimeoutSeconds) + 5;
        auto run = runProcess(s.m_config.headlessExe, options, s.m_config.checkTimeoutSeconds);
        if (!run) throw std::runtime_error(run.error().message);
        if (run->timedOut) fail(422, "check_failed", "The game took too long to load and was not accepted");
        if (run->exitCode != 0) {
            std::string why = run->errors;
            while (!why.empty() && (why.back() == '\n' || why.back() == '\r')) why.pop_back();
            if (why.size() > 800) why = why.substr(0, 800) + "...";
            if (run->exitCode != 1 || why.empty()) {
                log::warn("Package check for game {} stopped with code {}: {}", id, run->exitCode, why);
                why = "the checker stopped unexpectedly (the game may use too much memory)";
            }
            fail(422, "check_failed", "The game was not accepted: " + why);
        }
        auto report = online::reportFromJson(run->output);
        if (!report) throw std::runtime_error(report.error().message);
        auto read = readLocalFile(checked);
        if (!read || read->empty()) throw std::runtime_error("the checker wrote no package");
        const std::vector<std::uint8_t>& bytes = read.value();
        const std::string hash = crypto::toHex(crypto::hash256(bytes));
        const auto stored = packagePath(hash);
        if (!std::filesystem::exists(stored, ec)) {
            std::filesystem::create_directories(stored.parent_path(), ec);
            std::filesystem::rename(checked, stored, ec);
            if (ec) {
                std::filesystem::copy_file(checked, stored, std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) throw std::runtime_error("cannot store the package: " + ec.message());
            }
        }

        std::lock_guard lock(s.m_dbMutex);
        Database::Transaction tx(db());
        auto next = db().prepare("SELECT COALESCE(MAX(number), 0) + 1 FROM versions WHERE game_id = ?1");
        next.bind(1, id);
        next.step();
        const int number = static_cast<int>(next.integer(0));
        auto st = db().prepare(
            "INSERT INTO versions (game_id, number, package_hash, bytes, notes, engine_version, script_errors, report, "
            "created) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
        st.bind(1, id)
            .bind(2, number)
            .bind(3, hash)
            .bind(4, static_cast<std::int64_t>(bytes.size()))
            .bind(5, notes)
            .bind(6, std::string(version::string))
            .bind(7, report->scriptErrors)
            .bind(8, run->output)
            .bind(9, now());
        st.run();
        auto upd = db().prepare("UPDATE games SET latest_version = ?1, game_type = ?2, updated = ?3 WHERE id = ?4");
        upd.bind(1, number).bind(2, report->gameType).bind(3, now()).bind(4, id);
        upd.run();
        tx.commit();
        log::info("'{}' published version {} of game {} ({} bytes, {})", user.username, number, id, bytes.size(), hash);
        online::VersionInfo v{number, hash, bytes.size(), notes, std::string(version::string), report->scriptErrors, now()};
        json out = {{"version", v}, {"report", json::parse(run->output)}};
        sendJson(res, 201, out);
    }

    void listVersions(const httplib::Request& req, httplib::Response& res) {
        const User user = requireUser(req);
        const std::int64_t id = pathId(req);
        std::lock_guard lock(s.m_dbMutex);
        auto row = gameById(id);
        if (!row || !(user.admin() || row->info.ownerId == user.id)) fail(404, "not_found", "There is no such game");
        auto st = db().prepare(
            "SELECT number, package_hash, bytes, notes, engine_version, script_errors, created FROM versions WHERE "
            "game_id = ?1 ORDER BY number DESC");
        st.bind(1, id);
        json versions = json::array();
        while (st.step()) {
            online::VersionInfo v{static_cast<int>(st.integer(0)), st.text(1), static_cast<std::uint64_t>(st.integer(2)),
                                  st.text(3), st.text(4), static_cast<int>(st.integer(5)), st.integer(6)};
            versions.push_back(v);
        }
        sendJson(res, 200, {{"versions", versions}});
    }

    void downloadPackage(const httplib::Request& req, httplib::Response& res) {
        const std::string hash = req.matches[1].str();
        const auto viewer = currentUser(req);
        bool allowed = false;
        {
            std::lock_guard lock(s.m_dbMutex);
            auto st = db().prepare("SELECT DISTINCT game_id FROM versions WHERE package_hash = ?1");
            st.bind(1, hash);
            std::vector<std::int64_t> games;
            while (st.step()) games.push_back(st.integer(0));
            for (const std::int64_t g : games) {
                auto row = gameById(g);
                if (row && mayView(viewer, *row)) allowed = true;
            }
        }
        const auto file = packagePath(hash);
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        if (!allowed || ec) fail(404, "not_found", "There is no such package");
        auto stream = std::make_shared<std::ifstream>(file, std::ios::binary);
        res.set_content_provider(static_cast<std::size_t>(size), "application/octet-stream",
                                 [stream](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
                                     std::vector<char> buffer(std::min<std::size_t>(length, 64 * 1024));
                                     stream->seekg(static_cast<std::streamoff>(offset));
                                     stream->read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                                     const auto n = stream->gcount();
                                     if (n <= 0) return false;
                                     return sink.write(buffer.data(), static_cast<std::size_t>(n));
                                 });
    }

    void join(const httplib::Request& req, httplib::Response& res) {
        const User user = requireUser(req);
        limit("join:" + std::to_string(user.id), 30, 60);
        const std::int64_t id = pathId(req);
        const GameRow row = viewableGame(id, user);
        if (row.info.hidden && !user.admin()) fail(403, "hidden", "This game has been taken down by a moderator");
        if (row.info.latestVersion == 0) fail(409, "not_published", "This game has not been published yet");
        Fleet::GameRef ref{id, row.info.latestVersion, packagePath(row.info.packageHash), row.info.maxPlayers,
                           row.info.title};
        auto seat = s.m_fleet->reserve(ref, std::to_string(user.id));
        if (!seat) {
            const int status = seat.error().code == ErrorCode::LimitExceeded ? 503
                               : seat.error().code == ErrorCode::InvalidArgument ? 409
                                                                                 : 502;
            fail(status, "no_server", seat.error().message);
        }
        net::ConnectToken token;
        token.session = seat->session;
        token.userId = std::to_string(user.id);
        token.username = user.username;
        token.expires = now() + net::kConnectTokenLifetimeSeconds;
        auto nonce = crypto::randomToken(16);
        if (!nonce) throw std::runtime_error(nonce.error().message);
        token.nonce = nonce.value();
        online::JoinTicket ticket;
        ticket.token = net::signConnectToken(token, s.m_key);
        ticket.host = seat->host;
        ticket.port = seat->port;
        ticket.session = seat->session;
        ticket.expires = token.expires;
        ticket.version = row.info.latestVersion;
        ticket.packageHash = row.info.packageHash;
        ticket.webSocket = seat->webSocket;
        {
            std::lock_guard lock(s.m_dbMutex);
            auto st = db().prepare("UPDATE games SET plays = plays + 1 WHERE id = ?1");
            st.bind(1, id);
            st.run();
        }
        sendJson(res, 200, {{"ticket", ticket}});
    }

    void reportGame(const httplib::Request& req, httplib::Response& res) {
        const User user = requireUser(req);
        limit("report:" + std::to_string(user.id), 10, 3600);
        const std::int64_t id = pathId(req);
        (void)viewableGame(id, user);
        const json body = parseBody(req);
        const std::string reason = cleanText(stringField(body, "reason", online::kMaxReportLength, true, "The reason"), "The reason");
        if (reason.empty()) fail(400, "bad_request", "Say what is wrong");
        std::lock_guard lock(s.m_dbMutex);
        auto st = db().prepare("INSERT INTO reports (game_id, reporter_id, reason, created) VALUES (?1, ?2, ?3, ?4)");
        st.bind(1, id).bind(2, user.id).bind(3, reason).bind(4, now());
        st.run();
        sendJson(res, 201, {{"report", db().lastInsertId()}});
    }

    std::string storedData(std::int64_t gameId, std::int64_t userId) {
        auto st = db().prepare("SELECT data FROM player_data WHERE game_id = ?1 AND user_id = ?2");
        st.bind(1, gameId).bind(2, userId);
        return st.step() ? st.text(0) : std::string("{}");
    }

    void ownerPlayerData(const httplib::Request& req, httplib::Response& res) {
        const User user = requireUser(req);
        const std::int64_t id = pathId(req);
        std::lock_guard lock(s.m_dbMutex);
        auto row = gameById(id);
        if (!row || !(user.admin() || row->info.ownerId == user.id)) fail(404, "not_found", "There is no such game");
        auto player = userByName(req.matches[2].str());
        if (!player) fail(404, "not_found", "There is no such player");
        sendJson(res, 200, {{"data", json::parse(storedData(id, player->id))}});
    }

    // --- handlers: moderation ------------------------------------------------------------
    void listReports(const httplib::Request& req, httplib::Response& res) {
        (void)requireAdmin(req);
        const bool openOnly = req.get_param_value("state") != "all";
        std::lock_guard lock(s.m_dbMutex);
        auto st = db().prepare(std::format(
            "SELECT r.id, r.game_id, g.title, u.username, r.reason, r.state, r.resolution, r.created FROM reports r "
            "JOIN games g ON g.id = r.game_id JOIN users u ON u.id = r.reporter_id {} ORDER BY r.id DESC LIMIT 500",
            openOnly ? "WHERE r.state = 'open'" : ""));
        json reports = json::array();
        while (st.step()) {
            online::ReportInfo r{st.integer(0), st.integer(1), st.text(2), st.text(3),
                                 st.text(4),    st.text(5),    st.text(6), st.integer(7)};
            reports.push_back(r);
        }
        sendJson(res, 200, {{"reports", reports}});
    }

    void hideGame(const User& admin, std::int64_t id, bool hidden, const std::string& note) {
        auto row = gameById(id);
        if (!row) fail(404, "not_found", "There is no such game");
        auto st = db().prepare("UPDATE games SET hidden = ?1, hidden_note = ?2 WHERE id = ?3");
        st.bind(1, hidden ? 1 : 0).bind(2, hidden ? note : std::string()).bind(3, id);
        st.run();
        audit(admin, hidden ? "hide_game" : "unhide_game", std::format("game {} '{}'", id, row->info.title), note);
        if (hidden) s.m_fleet->stopGame(id);
    }

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

    void resolveReport(const httplib::Request& req, httplib::Response& res) {
        const User admin = requireAdmin(req);
        const std::int64_t id = pathId(req);
        const json body = parseBody(req);
        const std::string action = stringField(body, "action", 20, true, "action");
        const std::string note = cleanText(stringField(body, "note", 500, false, "The note"), "The note");
        std::lock_guard lock(s.m_dbMutex);
        auto st = db().prepare("SELECT game_id FROM reports WHERE id = ?1");
        st.bind(1, id);
        if (!st.step()) fail(404, "not_found", "There is no such report");
        const std::int64_t gameId = st.integer(0);
        Database::Transaction tx(db());
        if (action == "hide_game") {
            hideGame(admin, gameId, true, note);
        } else if (action == "ban_owner") {
            auto row = gameById(gameId);
            auto owner = userById(row->info.ownerId);
            banUser(admin, *owner, true, note);
        } else if (action != "dismiss") {
            fail(400, "bad_request", "action is \"dismiss\", \"hide_game\" or \"ban_owner\"");
        }
        auto upd = db().prepare(
            "UPDATE reports SET state = 'resolved', resolution = ?1, resolved_by = ?2, resolved_at = ?3 WHERE id = ?4");
        upd.bind(1, note.empty() ? action : action + ": " + note).bind(2, admin.id).bind(3, now()).bind(4, id);
        upd.run();
        audit(admin, "resolve_report", std::format("report {}", id), action);
        tx.commit();
        sendJson(res, 200, json::object());
    }

    void adminHide(const httplib::Request& req, httplib::Response& res) {
        const User admin = requireAdmin(req);
        const json body = parseBody(req);
        if (!body.contains("hidden") || !body["hidden"].is_boolean()) fail(400, "bad_request", "hidden must be true or false");
        const std::string note = cleanText(stringField(body, "note", 500, false, "The note"), "The note");
        std::lock_guard lock(s.m_dbMutex);
        hideGame(admin, pathId(req), body["hidden"].get<bool>(), note);
        sendJson(res, 200, json::object());
    }

    void adminBan(const httplib::Request& req, httplib::Response& res) {
        const User admin = requireAdmin(req);
        const json body = parseBody(req);
        if (!body.contains("banned") || !body["banned"].is_boolean()) fail(400, "bad_request", "banned must be true or false");
        const std::string note = cleanText(stringField(body, "note", 500, false, "The note"), "The note");
        std::lock_guard lock(s.m_dbMutex);
        auto target = userByName(req.matches[1].str());
        if (!target) fail(404, "not_found", "There is no such account");
        if (target->id == admin.id) fail(400, "bad_request", "You cannot ban yourself");
        banUser(admin, *target, body["banned"].get<bool>(), note);
        sendJson(res, 200, json::object());
    }

    void auditLog(const httplib::Request& req, httplib::Response& res) {
        (void)requireAdmin(req);
        int n = 100;
        try {
            if (req.has_param("limit")) n = std::clamp(std::stoi(req.get_param_value("limit")), 1, 1000);
        } catch (const std::exception&) {
        }
        std::lock_guard lock(s.m_dbMutex);
        auto st = db().prepare(
            "SELECT a.id, a.at, u.username, a.action, a.target, a.detail FROM audit a JOIN users u ON u.id = a.actor_id "
            "ORDER BY a.id DESC LIMIT ?1");
        st.bind(1, n);
        json entries = json::array();
        while (st.step()) {
            online::AuditEntry e{st.integer(0), st.integer(1), st.text(2), st.text(3), st.text(4), st.text(5)};
            entries.push_back(e);
        }
        sendJson(res, 200, {{"entries", entries}});
    }

    // --- handlers: game servers ------------------------------------------------------------
    std::string requireServer(const httplib::Request& req) {
        const std::string key = bearer(req, "Server");
        auto session = key.empty() || key.size() > 100 ? std::nullopt : s.m_fleet->sessionForKey(key);
        if (!session) fail(401, "not_a_server", "Unknown game server");
        return *session;
    }

    void serverReady(const httplib::Request& req, httplib::Response& res) {
        const std::string session = requireServer(req);
        const json body = parseBody(req);
        const int port = body.value("port", 0);
        s.m_fleet->ready(session, static_cast<std::uint16_t>(port));
        sendJson(res, 200, json::object());
    }

    void serverHeartbeat(const httplib::Request& req, httplib::Response& res) {
        const std::string session = requireServer(req);
        const json body = parseBody(req);
        std::vector<std::string> players;
        if (body.contains("players") && body["players"].is_array() && body["players"].size() <= 256) {
            for (const auto& p : body["players"]) {
                if (p.is_string()) players.push_back(p.get<std::string>());
            }
        }
        const bool stop = s.m_fleet->heartbeat(session, players);
        sendJson(res, 200, {{"stop", stop}});
    }

    std::int64_t serverPlayer(const httplib::Request& req, const std::string& session) {
        const std::string userId = req.matches[1].str();
        if (!s.m_fleet->holdsPlayer(session, userId)) {
            fail(403, "not_here", "That player was not sent to this server");
        }
        try {
            return std::stoll(userId);
        } catch (const std::exception&) {
            fail(404, "not_found", "No such player");
        }
    }

    void serverLoadData(const httplib::Request& req, httplib::Response& res) {
        const std::string session = requireServer(req);
        const std::int64_t user = serverPlayer(req, session);
        const auto game = s.m_fleet->gameOf(session);
        if (!game) fail(404, "not_found", "Unknown session");
        std::lock_guard lock(s.m_dbMutex);
        sendJson(res, 200, {{"data", json::parse(storedData(*game, user))}});
    }

    void serverSaveData(const httplib::Request& req, httplib::Response& res) {
        const std::string session = requireServer(req);
        const std::int64_t user = serverPlayer(req, session);
        const auto game = s.m_fleet->gameOf(session);
        if (!game) fail(404, "not_found", "Unknown session");
        const json body = parseBody(req, 2 * runtime::kMaxPlayerDataBytes + 1024);
        if (!body.contains("data")) fail(400, "bad_request", "data is required");
        auto data = online::playerDataFromJson(body["data"].dump());
        if (!data) fail(400, "bad_data", data.error().message);
        const std::string normalized = online::playerDataToJson(data.value());
        {
            std::lock_guard lock(s.m_dbMutex);
            auto st = db().prepare(
                "INSERT INTO player_data (game_id, user_id, data, updated) VALUES (?1, ?2, ?3, ?4) "
                "ON CONFLICT(game_id, user_id) DO UPDATE SET data = excluded.data, updated = excluded.updated");
            st.bind(1, *game).bind(2, user).bind(3, normalized).bind(4, now());
            st.run();
        }
        if (body.value("release", false)) s.m_fleet->release(session, std::to_string(user));
        sendJson(res, 200, json::object());
    }

    void serverRelease(const httplib::Request& req, httplib::Response& res) {
        const std::string session = requireServer(req);
        const std::int64_t user = serverPlayer(req, session);
        s.m_fleet->release(session, std::to_string(user));
        sendJson(res, 200, json::object());
    }

    void serverEnded(const httplib::Request& req, httplib::Response& res) {
        const std::string session = requireServer(req);
        sendJson(res, 200, json::object());
        // Ending waits for the process; do it after answering.
        std::thread([fleet = s.m_fleet.get(), session] { fleet->ended(session); }).detach();
    }

    // --- a page for browsers -------------------------------------------------------------------
    void home(const httplib::Request&, httplib::Response& res) {
        std::string rows;
        {
            std::lock_guard lock(s.m_dbMutex);
            auto st = db().prepare(std::format(
                "SELECT {} FROM games g JOIN users u ON u.id = g.owner_id WHERE g.visibility = 'public' AND g.hidden = 0 "
                "AND u.banned = 0 AND g.latest_version > 0 ORDER BY g.plays DESC LIMIT 50",
                kGameColumns));
            while (st.step()) {
                const GameRow row = readGame(st);
                rows += std::format("<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td><code>px-player --platform {} "
                                    "--play {}</code></td></tr>\n",
                                    row.info.id, htmlEscape(row.info.title), htmlEscape(row.info.ownerName),
                                    row.info.plays, htmlEscape(s.url()), row.info.id);
            }
        }
        const std::string page = std::format(
            "<!doctype html><html><head><meta charset=\"utf-8\"><title>{0}</title>"
            "<style>body{{font-family:sans-serif;margin:2em;max-width:60em}}td,th{{padding:.3em .8em;text-align:left}}"
            "</style></head><body><h1>{0}</h1><p>Platform for Project Pixel {1}. Play from the player app or Studio.</p>"
            "<table><tr><th>Id</th><th>Game</th><th>By</th><th>Plays</th><th>Play</th></tr>{2}</table></body></html>",
            htmlEscape(s.m_config.name), version::string, rows);
        res.set_content(page, "text/html; charset=utf-8");
    }
};

// =============================================================================================

PlatformService::PlatformService(ServiceConfig config) : m_config(std::move(config)) {}

PlatformService::~PlatformService() { stop(); }

Result<std::unique_ptr<PlatformService>> PlatformService::create(ServiceConfig config) {
    auto service = std::unique_ptr<PlatformService>(new PlatformService(std::move(config)));
    if (auto s = service->initialize(); !s) return s.error();
    return service;
}

Status PlatformService::initialize() {
    std::error_code ec;
    std::filesystem::create_directories(m_config.dataDir / "packages", ec);
    std::filesystem::create_directories(m_config.dataDir / "logs", ec);
    if (ec) return makeError(ErrorCode::IoError, "Cannot create {}: {}", m_config.dataDir.string(), ec.message());

    // The signing key: created once, kept private to this account.
    const auto keyFile = m_config.dataDir / "platform.key";
    std::ifstream in(keyFile);
    std::string seedHex;
    if (in >> seedHex) {
        const auto seed = crypto::fromHex(seedHex);
        if (!seed || seed->size() != 32) return makeError(ErrorCode::InvalidData, "{} is damaged", keyFile.string());
        m_key = crypto::SigningKey::fromSeed(std::span<const std::uint8_t, 32>(seed->data(), 32));
    } else {
        auto key = crypto::SigningKey::generate();
        if (!key) return key.error();
        m_key = key.value();
        {
            std::ofstream create(keyFile, std::ios::trunc);
        }
        std::filesystem::permissions(keyFile, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
        std::ofstream out(keyFile, std::ios::trunc);
        out << crypto::toHex(std::span<const std::uint8_t>(m_key.secret.data(), 32)) << "\n";
        if (!out) return makeError(ErrorCode::IoError, "Cannot write {}", keyFile.string());
        log::info("Created the platform's signing key in {}", keyFile.string());
    }

    auto db = Database::open(m_config.dataDir / "platform.db");
    if (!db) return db.error();
    m_db = std::move(db.value());
    try {
        migrate();
    } catch (const DatabaseError& e) {
        return makeError(ErrorCode::IoError, "Cannot set up the database: {}", e.what());
    }
    m_limits.setEnabled(m_config.rateLimits);
    m_impl = std::make_unique<Impl>(*this);
    auto dummy = crypto::hashPassword("not a real password", m_config.passwordHashing);
    if (!dummy) return dummy.error();
    m_impl->dummyHash = dummy.value();
    return {};
}

void PlatformService::migrate() {
    auto st = m_db->prepare("PRAGMA user_version");
    st.step();
    const auto version = st.integer(0);
    if (version >= 1) return;
    m_db->exec(R"SQL(
BEGIN;
CREATE TABLE users (
    id INTEGER PRIMARY KEY,
    username TEXT NOT NULL UNIQUE COLLATE NOCASE,
    password_hash TEXT NOT NULL,
    role TEXT NOT NULL DEFAULT 'user',
    banned INTEGER NOT NULL DEFAULT 0,
    ban_note TEXT NOT NULL DEFAULT '',
    created INTEGER NOT NULL);
CREATE TABLE sessions (
    token_hash TEXT PRIMARY KEY,
    user_id INTEGER NOT NULL REFERENCES users(id),
    created INTEGER NOT NULL,
    expires INTEGER NOT NULL);
CREATE INDEX sessions_user ON sessions(user_id);
CREATE TABLE games (
    id INTEGER PRIMARY KEY,
    owner_id INTEGER NOT NULL REFERENCES users(id),
    title TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    visibility TEXT NOT NULL DEFAULT 'public',
    game_type TEXT NOT NULL DEFAULT '',
    max_players INTEGER NOT NULL DEFAULT 8,
    latest_version INTEGER NOT NULL DEFAULT 0,
    plays INTEGER NOT NULL DEFAULT 0,
    hidden INTEGER NOT NULL DEFAULT 0,
    hidden_note TEXT NOT NULL DEFAULT '',
    created INTEGER NOT NULL,
    updated INTEGER NOT NULL);
CREATE INDEX games_owner ON games(owner_id);
CREATE TABLE versions (
    game_id INTEGER NOT NULL REFERENCES games(id),
    number INTEGER NOT NULL,
    package_hash TEXT NOT NULL,
    bytes INTEGER NOT NULL,
    notes TEXT NOT NULL DEFAULT '',
    engine_version TEXT NOT NULL,
    script_errors INTEGER NOT NULL DEFAULT 0,
    report TEXT NOT NULL DEFAULT '{}',
    created INTEGER NOT NULL,
    PRIMARY KEY (game_id, number));
CREATE INDEX versions_hash ON versions(package_hash);
CREATE TABLE player_data (
    game_id INTEGER NOT NULL REFERENCES games(id),
    user_id INTEGER NOT NULL REFERENCES users(id),
    data TEXT NOT NULL,
    updated INTEGER NOT NULL,
    PRIMARY KEY (game_id, user_id));
CREATE TABLE reports (
    id INTEGER PRIMARY KEY,
    game_id INTEGER NOT NULL REFERENCES games(id),
    reporter_id INTEGER NOT NULL REFERENCES users(id),
    reason TEXT NOT NULL,
    state TEXT NOT NULL DEFAULT 'open',
    resolution TEXT NOT NULL DEFAULT '',
    resolved_by INTEGER,
    resolved_at INTEGER,
    created INTEGER NOT NULL);
CREATE TABLE audit (
    id INTEGER PRIMARY KEY,
    at INTEGER NOT NULL,
    actor_id INTEGER NOT NULL REFERENCES users(id),
    action TEXT NOT NULL,
    target TEXT NOT NULL,
    detail TEXT NOT NULL DEFAULT '');
PRAGMA user_version = 1;
COMMIT;
)SQL");
}

std::string PlatformService::url() const {
    const std::string host = m_config.bindAddress == "0.0.0.0" ? "127.0.0.1" : m_config.bindAddress;
    return std::format("http://{}:{}", host, m_port);
}

Status PlatformService::setRole(std::string_view username, Role role) {
    std::lock_guard lock(m_dbMutex);
    auto st = m_db->prepare("UPDATE users SET role = ?1 WHERE username = ?2 COLLATE NOCASE");
    st.bind(1, online::roleName(role)).bind(2, username);
    st.run();
    if (m_db->changes() == 0) return makeError(ErrorCode::NotFound, "There is no account named '{}'", username);
    return {};
}

void PlatformService::registerRoutes(httplib::Server& http) {
    Impl& i = *m_impl;
    const auto bind = [&i](void (Impl::*fn)(const httplib::Request&, httplib::Response&)) {
        return i.wrap([&i, fn](const httplib::Request& req, httplib::Response& res) { (i.*fn)(req, res); });
    };
    http.Get("/", bind(&Impl::home));
    http.Get("/api/v1/platform", i.wrap([this](const httplib::Request&, httplib::Response& res) {
        online::PlatformInfo info{m_config.name, std::string(version::string), online::kPlatformApiVersion,
                                  crypto::toHex(m_key.publicKey)};
        sendJson(res, 200, info);
    }));
    http.Post("/api/v1/accounts", bind(&Impl::registerAccount));
    http.Post("/api/v1/sessions", bind(&Impl::login));
    http.Delete("/api/v1/sessions/current", bind(&Impl::logout));
    http.Get("/api/v1/me", bind(&Impl::me));
    http.Get("/api/v1/games", bind(&Impl::listGames));
    http.Get("/api/v1/games/mine", bind(&Impl::myGames));
    http.Post("/api/v1/games", bind(&Impl::createGame));
    http.Get(R"(/api/v1/games/(\d+))", bind(&Impl::getGame));
    http.Patch(R"(/api/v1/games/(\d+))", bind(&Impl::updateGame));
    http.Post(R"(/api/v1/games/(\d+)/versions)", bind(&Impl::publish));
    http.Get(R"(/api/v1/games/(\d+)/versions)", bind(&Impl::listVersions));
    http.Post(R"(/api/v1/games/(\d+)/join)", bind(&Impl::join));
    http.Post(R"(/api/v1/games/(\d+)/reports)", bind(&Impl::reportGame));
    http.Get(R"(/api/v1/games/(\d+)/data/([A-Za-z0-9_]{1,32}))", bind(&Impl::ownerPlayerData));
    http.Get(R"(/api/v1/packages/([0-9a-f]{64}))", bind(&Impl::downloadPackage));
    http.Get("/api/v1/admin/reports", bind(&Impl::listReports));
    http.Post(R"(/api/v1/admin/reports/(\d+)/resolve)", bind(&Impl::resolveReport));
    http.Post(R"(/api/v1/admin/games/(\d+)/hide)", bind(&Impl::adminHide));
    http.Post(R"(/api/v1/admin/users/([A-Za-z0-9_]{1,32})/ban)", bind(&Impl::adminBan));
    http.Get("/api/v1/admin/audit", bind(&Impl::auditLog));
    http.Post("/api/v1/server/ready", bind(&Impl::serverReady));
    http.Post("/api/v1/server/heartbeat", bind(&Impl::serverHeartbeat));
    http.Get(R"(/api/v1/server/data/(\d{1,19}))", bind(&Impl::serverLoadData));
    http.Put(R"(/api/v1/server/data/(\d{1,19}))", bind(&Impl::serverSaveData));
    http.Post(R"(/api/v1/server/release/(\d{1,19}))", bind(&Impl::serverRelease));
    http.Post("/api/v1/server/ended", bind(&Impl::serverEnded));
    http.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.body.empty()) {
            res.set_content(json{{"error", {{"code", "not_found"}, {"message", "Not found"}}}}.dump(), "application/json");
        }
    });
}

std::string PlatformService::httpsUrl() const {
    if (m_httpsPort == 0) return {};
    const std::string bind = m_config.httpsBindAddress.empty() ? m_config.bindAddress : m_config.httpsBindAddress;
    const std::string host = bind == "0.0.0.0" ? "127.0.0.1" : bind;
    return std::format("https://{}:{}", host, m_httpsPort);
}

Status PlatformService::start() {
    const std::size_t threads = m_config.threads;
    const auto configure = [this, threads](httplib::Server& server) {
        server.new_task_queue = [threads] { return new httplib::ThreadPool(threads); };
        server.set_payload_max_length(project::kMaxPackageBytes + (1u << 20));
        server.set_read_timeout(120, 0);
        server.set_write_timeout(120, 0);
        server.set_keep_alive_max_count(100);
        registerRoutes(server);
    };
    m_http = std::make_unique<httplib::Server>();
    configure(*m_http);
    if (m_config.httpsPort != 0) {
#if defined(PX_HAVE_TLS)
        const std::string cert = m_config.tlsCert.string();
        const std::string key = m_config.tlsKey.string();
        auto https = std::make_unique<httplib::SSLServer>(cert.c_str(), key.c_str());
        if (!https->is_valid()) {
            return makeError(ErrorCode::InvalidData, "Cannot use the TLS certificate {} and key {}", cert, key);
        }
        m_https = std::move(https);
        configure(*m_https);
        const std::string bind = m_config.httpsBindAddress.empty() ? m_config.bindAddress : m_config.httpsBindAddress;
        if (m_config.httpsPort < 0) {
            m_httpsPort = m_https->bind_to_any_port(bind);
        } else if (m_https->bind_to_port(bind, m_config.httpsPort)) {
            m_httpsPort = m_config.httpsPort;
        }
        if (m_httpsPort <= 0) return makeError(ErrorCode::IoError, "Cannot listen for HTTPS on {}:{}", bind, m_config.httpsPort);
#else
        return makeError(ErrorCode::Unsupported, "This build has no TLS support (PX_ENABLE_TLS)");
#endif
    }
    if (m_config.port == 0) {
        m_port = m_http->bind_to_any_port(m_config.bindAddress);
        if (m_port <= 0) return makeError(ErrorCode::IoError, "Cannot listen on {}", m_config.bindAddress);
    } else {
        if (!m_http->bind_to_port(m_config.bindAddress, m_config.port)) {
            return makeError(ErrorCode::IoError, "Cannot listen on {}:{} (is another platform running?)",
                             m_config.bindAddress, m_config.port);
        }
        m_port = m_config.port;
    }
    FleetConfig fleet;
    fleet.serverExe = m_config.serverExe;
    fleet.logDir = m_config.dataDir / "logs";
    fleet.platformUrl = m_config.platformUrl.empty() ? url() : m_config.platformUrl;
    fleet.publicHost = m_config.publicHost;
    fleet.firstPort = m_config.firstServerPort;
    fleet.portCount = m_config.serverPortCount;
    fleet.maxServers = m_config.maxServers;
    fleet.memoryLimitMiB = m_config.serverMemoryMiB;
    fleet.platformKeyHex = crypto::toHex(m_key.publicKey);
    if (m_https) {
        fleet.tlsCert = m_config.tlsCert.string();
        fleet.tlsKey = m_config.tlsKey.string();
    }
    m_fleet = std::make_unique<Fleet>(fleet);

    m_running = true;
    m_listener = std::thread([this] { m_http->listen_after_bind(); });
    if (m_https) m_httpsListener = std::thread([this] { m_https->listen_after_bind(); });
    m_maintenance = std::thread([this] {
        int ticks = 0;
        while (m_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (++ticks % 4 != 0) continue;
            m_fleet->maintain();
            if (ticks % (4 * 3600) == 0) {
                try {
                    std::lock_guard lock(m_dbMutex);
                    auto st = m_db->prepare("DELETE FROM sessions WHERE expires < ?1");
                    st.bind(1, now());
                    st.run();
                } catch (const std::exception& e) {
                    log::error("Cleaning up sessions: {}", e.what());
                }
            }
        }
    });
    m_http->wait_until_ready();
    if (m_https) m_https->wait_until_ready();
    log::info("{} listening on {}{} (data in {})", m_config.name, url(),
              m_https ? std::format(" and {}", httpsUrl()) : std::string(), m_config.dataDir.string());
    return {};
}

void PlatformService::stop() {
    if (!m_running.exchange(false)) return;
    if (m_http) m_http->stop();
    if (m_https) m_https->stop();
    if (m_listener.joinable()) m_listener.join();
    if (m_httpsListener.joinable()) m_httpsListener.join();
    if (m_maintenance.joinable()) m_maintenance.join();
    if (m_fleet) m_fleet->stopAll();
}

} // namespace px::platform
