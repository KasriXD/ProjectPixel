#include "PlatformService.hpp"
#include "ServiceContext.hpp"
#include "Leaderboards.hpp"
#include "Social.hpp"

#include "Database.hpp"
#include "WebFiles.hpp"
#include "px/core/Log.hpp"
#include "px/core/Version.hpp"
#include "px/crypto/Encoding.hpp"
#include "px/net/ConnectToken.hpp"
#include "px/online/PackageCheck.hpp"
#include "px/online/Thumbnail.hpp"
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

using namespace service;

// =============================================================================================

struct PlatformService::Impl : PlatformService::Context {
    explicit Impl(PlatformService& service) : Context(service) {}

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
        if (s.m_config.sandbox) options.args.push_back("--sandbox");
        options.memoryLimitMiB = s.m_config.checkMemoryMiB;
        options.cpuLimitSeconds = static_cast<int>(s.m_config.checkTimeoutSeconds) + 5;
        options.cleanEnvironment = true;
        options.singleProcess = true;
        options.maxOpenFiles = 64;
        options.maxFileSizeMiB = 512;
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
            "created, leaderboards) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)");
        st.bind(1, id)
            .bind(2, number)
            .bind(3, hash)
            .bind(4, static_cast<std::int64_t>(bytes.size()))
            .bind(5, notes)
            .bind(6, std::string(version::string))
            .bind(7, report->scriptErrors)
            .bind(8, run->output)
            .bind(9, now())
            .bind(10, online::leaderboardsToJson(report->leaderboards));
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

    // --- thumbnails -------------------------------------------------------------------------
    std::filesystem::path thumbnailPath(const std::string& hash) const {
        return s.m_config.dataDir / "thumbnails" / (hash + ".png");
    }

    void uploadThumbnail(const httplib::Request& req, httplib::Response& res) {
        const User user = requireUser(req);
        const std::int64_t id = pathId(req);
        {
            std::lock_guard lock(s.m_dbMutex);
            auto row = gameById(id);
            if (!row || !(row->info.ownerId == user.id || user.admin())) fail(404, "not_found", "There is no such game of yours");
        }
        limit("thumbnail:" + std::to_string(user.id), 30, 3600);
        if (req.body.size() > online::kMaxThumbnailUploadBytes) fail(413, "too_large", "The picture is larger than 4 MiB");
        if (req.body.size() < 8) fail(400, "bad_request", "Send a PNG or JPEG picture as the request body");
        // Decoded in its own limited process, like uploaded games; only its PNG is kept.
        const auto tmpDir = s.m_config.dataDir / "tmp";
        std::error_code ec;
        std::filesystem::create_directories(tmpDir, ec);
        auto name = crypto::randomToken(12);
        if (!name) throw std::runtime_error(name.error().message);
        const auto upload = tmpDir / (name.value() + ".picture");
        const auto made = tmpDir / (name.value() + ".thumb.png");
        struct Cleanup {
            std::filesystem::path a, b;
            ~Cleanup() {
                std::error_code e;
                std::filesystem::remove(a, e);
                std::filesystem::remove(b, e);
            }
        } cleanup{upload, made};
        {
            std::ofstream out(upload, std::ios::binary | std::ios::trunc);
            out.write(req.body.data(), static_cast<std::streamsize>(req.body.size()));
            if (!out) throw std::runtime_error("cannot write the upload");
        }
        ProcessOptions options;
        options.args = {"--check-thumbnail", upload.string(), "--output", made.string()};
        if (s.m_config.sandbox) options.args.push_back("--sandbox");
        options.memoryLimitMiB = 512;
        options.cpuLimitSeconds = 25;
        options.cleanEnvironment = true;
        options.singleProcess = true;
        options.maxOpenFiles = 64;
        options.maxFileSizeMiB = 16;
        auto run = runProcess(s.m_config.headlessExe, options, 20.0);
        if (!run) throw std::runtime_error(run.error().message);
        if (run->timedOut || run->exitCode != 0) {
            std::string why = run->errors;
            while (!why.empty() && (why.back() == '\n' || why.back() == '\r')) why.pop_back();
            if (why.empty() || why.size() > 300) why = "it could not be read";
            fail(422, "check_failed", "The picture was not accepted: " + why);
        }
        auto png = readLocalFile(made);
        if (!png || png->empty()) throw std::runtime_error("the checker wrote no thumbnail");
        const std::string hash = crypto::toHex(crypto::hash256(png.value())).substr(0, 32);
        const auto stored = thumbnailPath(hash);
        std::filesystem::create_directories(stored.parent_path(), ec);
        std::filesystem::rename(made, stored, ec);
        if (ec) {
            std::filesystem::copy_file(made, stored, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) throw std::runtime_error("cannot store the thumbnail: " + ec.message());
        }
        std::lock_guard lock(s.m_dbMutex);
        auto old = db().prepare("SELECT thumbnail FROM games WHERE id = ?1");
        old.bind(1, id);
        const std::string previous = old.step() ? old.text(0) : std::string();
        auto st = db().prepare("UPDATE games SET thumbnail = ?1 WHERE id = ?2");
        st.bind(1, hash).bind(2, id);
        st.run();
        if (!previous.empty() && previous != hash) {
            auto used = db().prepare("SELECT COUNT(*) FROM games WHERE thumbnail = ?1");
            used.bind(1, previous);
            if (used.step() && used.integer(0) == 0) std::filesystem::remove(thumbnailPath(previous), ec);
        }
        sendJson(res, 200, {{"thumbnail", hash}});
    }

    void getThumbnail(const httplib::Request& req, httplib::Response& res) {
        const auto viewer = currentUser(req);
        const GameRow row = viewableGame(pathId(req), viewer);
        if (row.info.thumbnail.empty()) fail(404, "not_found", "This game has no picture");
        const auto file = thumbnailPath(row.info.thumbnail);
        auto png = readLocalFile(file);
        if (!png) fail(404, "not_found", "This game has no picture");
        const std::string etag = "\"" + row.info.thumbnail + "\"";
        res.set_header("ETag", etag);
        res.set_header("Cache-Control", "no-cache");
        if (req.get_header_value("If-None-Match") == etag) {
            res.status = 304;
            return;
        }
        res.set_content(std::string(png->begin(), png->end()), "image/png");
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
        std::set<std::string> avoid;
        {
            std::lock_guard lock(s.m_dbMutex);
            avoid = blockedWith(user.id); // never in one server with someone blocked either way
        }
        auto seat = s.m_fleet->reserve(ref, std::to_string(user.id), avoid);
        if (!seat) failSeat(seat.error());
        sendJson(res, 200, {{"ticket", issueTicket(user, row.info.id, row.info.latestVersion, row.info.packageHash, *seat)}});
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
    // --- handlers: the browser page and player (docs/platform.md#browsers) ------------------
    [[nodiscard]] bool hasWebPlayer() const {
        std::error_code ec;
        return !s.m_config.webPlayerDir.empty() &&
               std::filesystem::is_regular_file(s.m_config.webPlayerDir / "px-player.js", ec);
    }

    static void pageHeaders(httplib::Response& res) {
        // Scripts only from here (WebAssembly needs 'wasm-unsafe-eval'); game
        // servers are reached over ws: / wss: on their own ports.
        res.set_header("Content-Security-Policy",
                       "default-src 'none'; script-src 'self' 'wasm-unsafe-eval'; style-src 'self'; "
                       "connect-src 'self' ws: wss:; img-src 'self' data: blob:; base-uri 'none'; "
                       "form-action 'self'; frame-ancestors 'none'");
        res.set_header("X-Frame-Options", "DENY");
        res.set_header("Referrer-Policy", "no-referrer");
        res.set_header("Cache-Control", "no-cache");
    }

    void playPage(const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.matches.size() > 1 && req.matches[1].matched ? req.matches[1].str() : "play.html";
        const auto file = findWebFile(name);
        if (!file) fail(404, "not_found", "There is no such page");
        pageHeaders(res);
        res.set_content(std::string(file->content), file->contentType);
    }

    // px-player.js / .wasm / .data from the WebAssembly build, revalidated
    // with an ETag so browsers keep their copy until it changes.
    void playerFile(const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.matches[1].str();
        if (!hasWebPlayer()) fail(404, "not_found", "This platform has no browser player");
        const auto file = s.m_config.webPlayerDir / name;
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        const auto written = ec ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(file, ec);
        if (ec) fail(404, "not_found", "There is no such file");
        const std::string etag =
            std::format("\"{:x}-{:x}\"", size, static_cast<std::uint64_t>(written.time_since_epoch().count()));
        res.set_header("ETag", etag);
        res.set_header("Cache-Control", "no-cache");
        if (req.get_header_value("If-None-Match") == etag) {
            res.status = 304;
            return;
        }
        const std::string type = name.ends_with(".wasm") ? "application/wasm"
                                 : name.ends_with(".js") ? "text/javascript; charset=utf-8"
                                                         : "application/octet-stream";
        auto stream = std::make_shared<std::ifstream>(file, std::ios::binary);
        res.set_content_provider(static_cast<std::size_t>(size), type,
                                 [stream](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
                                     std::vector<char> buffer(std::min<std::size_t>(length, 256 * 1024));
                                     stream->seekg(static_cast<std::streamoff>(offset));
                                     stream->read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                                     const auto n = stream->gcount();
                                     if (n <= 0) return false;
                                     return sink.write(buffer.data(), static_cast<std::size_t>(n));
                                 });
    }

    void home(const httplib::Request&, httplib::Response& res) {
        const bool webPlayer = hasWebPlayer();
        std::string rows;
        {
            std::lock_guard lock(s.m_dbMutex);
            auto st = db().prepare(std::format(
                "SELECT {} FROM games g JOIN users u ON u.id = g.owner_id WHERE g.visibility = 'public' AND g.hidden = 0 "
                "AND u.banned = 0 AND g.latest_version > 0 ORDER BY g.plays DESC LIMIT 50",
                kGameColumns));
            while (st.step()) {
                const GameRow row = readGame(st);
                const std::string browser =
                    webPlayer ? std::format("<a href=\"/play#game={}\">Play in the browser</a> or ", row.info.id) : "";
                rows += std::format("<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}<code>px-player --platform {} "
                                    "--play {}</code></td></tr>\n",
                                    row.info.id, htmlEscape(row.info.title), htmlEscape(row.info.ownerName),
                                    row.info.plays, browser, htmlEscape(s.url()), row.info.id);
            }
        }
        const std::string page = std::format(
            "<!doctype html><html><head><meta charset=\"utf-8\"><title>{0}</title>"
            "<style>body{{font-family:sans-serif;margin:2em;max-width:60em}}td,th{{padding:.3em .8em;text-align:left}}"
            "</style></head><body><h1>{0}</h1><p>Platform for Project Pixel {1}. Play from the player app, Studio{3}.</p>"
            "<table><tr><th>Id</th><th>Game</th><th>By</th><th>Plays</th><th>Play</th></tr>{2}</table></body></html>",
            htmlEscape(s.m_config.name), version::string, rows,
            webPlayer ? " or <a href=\"/play\">your browser</a>" : "");
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
    if (version < 1) migrateTo1();
    if (version < 2) migrateTo2();
}

void PlatformService::migrateTo1() {
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

// Version 2 (0.8.0): profiles, friends, blocks, invitations, reports about
// accounts, leaderboards, thumbnails.
void PlatformService::migrateTo2() {
    m_db->exec(R"SQL(
BEGIN;
ALTER TABLE users ADD COLUMN bio TEXT NOT NULL DEFAULT '';
ALTER TABLE users ADD COLUMN avatar INTEGER NOT NULL DEFAULT 0;
ALTER TABLE users ADD COLUMN join_policy TEXT NOT NULL DEFAULT 'friends';
ALTER TABLE users ADD COLUMN last_seen INTEGER NOT NULL DEFAULT 0;
CREATE TABLE friends (
    user_id INTEGER NOT NULL REFERENCES users(id),
    friend_id INTEGER NOT NULL REFERENCES users(id),
    created INTEGER NOT NULL,
    PRIMARY KEY (user_id, friend_id));
CREATE TABLE friend_requests (
    from_id INTEGER NOT NULL REFERENCES users(id),
    to_id INTEGER NOT NULL REFERENCES users(id),
    created INTEGER NOT NULL,
    PRIMARY KEY (from_id, to_id));
CREATE INDEX friend_requests_to ON friend_requests(to_id);
CREATE TABLE blocks (
    user_id INTEGER NOT NULL REFERENCES users(id),
    blocked_id INTEGER NOT NULL REFERENCES users(id),
    created INTEGER NOT NULL,
    PRIMARY KEY (user_id, blocked_id));
CREATE INDEX blocks_blocked ON blocks(blocked_id);
CREATE TABLE invitations (
    id INTEGER PRIMARY KEY,
    from_id INTEGER NOT NULL REFERENCES users(id),
    to_id INTEGER NOT NULL REFERENCES users(id),
    game_id INTEGER NOT NULL REFERENCES games(id),
    session TEXT NOT NULL,
    created INTEGER NOT NULL,
    expires INTEGER NOT NULL);
CREATE INDEX invitations_to ON invitations(to_id);
CREATE TABLE user_reports (
    id INTEGER PRIMARY KEY,
    user_id INTEGER NOT NULL REFERENCES users(id),
    reporter_id INTEGER NOT NULL REFERENCES users(id),
    reason TEXT NOT NULL,
    state TEXT NOT NULL DEFAULT 'open',
    resolution TEXT NOT NULL DEFAULT '',
    resolved_by INTEGER,
    resolved_at INTEGER,
    created INTEGER NOT NULL);
CREATE TABLE leaderboard_scores (
    game_id INTEGER NOT NULL REFERENCES games(id),
    board TEXT NOT NULL,
    user_id INTEGER NOT NULL REFERENCES users(id),
    value REAL NOT NULL,
    updated INTEGER NOT NULL,
    PRIMARY KEY (game_id, board, user_id));
CREATE INDEX leaderboard_values ON leaderboard_scores(game_id, board, value);
ALTER TABLE versions ADD COLUMN leaderboards TEXT NOT NULL DEFAULT '[]';
ALTER TABLE games ADD COLUMN thumbnail TEXT NOT NULL DEFAULT '';
PRAGMA user_version = 2;
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
    // Every answer: no guessing of content types by browsers.
    http.set_post_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("X-Content-Type-Options", "nosniff");
    });
    http.Get("/", bind(&Impl::home));
    registerSocialRoutes(http, i);
    registerLeaderboardRoutes(http, i);
    http.Get("/play", bind(&Impl::playPage));
    http.Get("/play/", i.wrap([](const httplib::Request&, httplib::Response& res) { res.set_redirect("/play"); }));
    http.Get(R"(/play/(play\.(?:js|css)|avatars\.png))", bind(&Impl::playPage));
    http.Get(R"(/play/player/(px-player\.(?:js|wasm|data)))", bind(&Impl::playerFile));
    http.Get("/api/v1/platform", i.wrap([this](const httplib::Request&, httplib::Response& res) {
        online::PlatformInfo info{m_config.name, std::string(version::string), online::kPlatformApiVersion,
                                  crypto::toHex(m_key.publicKey), m_impl->hasWebPlayer()};
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
    http.Put(R"(/api/v1/games/(\d+)/thumbnail)", bind(&Impl::uploadThumbnail));
    http.Get(R"(/api/v1/games/(\d+)/thumbnail)", bind(&Impl::getThumbnail));
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
    fleet.cpuLimitPercent = m_config.serverCpuPercent;
    fleet.sandbox = m_config.sandbox;
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
