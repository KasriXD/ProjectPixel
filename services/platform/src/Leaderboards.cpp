// Leaderboards (docs/leaderboards.md): each player's best score on the
// boards a game declares, sent by its game servers and shown to players.
#include "Leaderboards.hpp"

#include "ServiceContext.hpp"
#include "px/online/PackageCheck.hpp"

#include <cmath>
#include <memory>
#include <sstream>

namespace px::platform {

namespace {

using online::LeaderboardEntry;
using online::LeaderboardView;
using project::LeaderboardDef;

constexpr std::size_t kMaxScoresPerRequest = 200;

struct Leaderboards {
    PlatformService::Context& c;

    explicit Leaderboards(PlatformService::Context& context) : c(context) {}

    // The boards of a version (0: the latest). With the database locked.
    std::vector<LeaderboardDef> boardsOf(std::int64_t gameId, int version) {
        auto st = c.db().prepare(
            version > 0 ? "SELECT leaderboards FROM versions WHERE game_id = ?1 AND number = ?2"
                        : "SELECT v.leaderboards FROM versions v JOIN games g ON g.id = v.game_id "
                          "WHERE v.game_id = ?1 AND v.number = g.latest_version");
        st.bind(1, gameId);
        if (version > 0) st.bind(2, version);
        if (!st.step()) return {};
        auto boards = online::leaderboardsFromJson(st.text(0));
        return boards ? boards.value() : std::vector<LeaderboardDef>{};
    }

    static const char* direction(const LeaderboardDef& b) {
        return b.order == project::LeaderboardOrder::Highest ? "DESC" : "ASC";
    }
    static const char* better(const LeaderboardDef& b) { return b.order == project::LeaderboardOrder::Highest ? ">" : "<"; }

    int rankOf(std::int64_t gameId, const LeaderboardDef& b, double value) {
        auto st = c.db().prepare(std::format(
            "SELECT COUNT(*) FROM leaderboard_scores s JOIN users u ON u.id = s.user_id "
            "WHERE s.game_id = ?1 AND s.board = ?2 AND u.banned = 0 AND s.value {} ?3",
            better(b)));
        st.bind(1, gameId).bind(2, b.id).bind(3, value);
        (void)st.step();
        return static_cast<int>(st.integer(0)) + 1;
    }

    // Rows best first, ranked (equal scores share a rank).
    std::vector<LeaderboardEntry> ranked(Statement& st, std::int64_t gameId, const LeaderboardDef& b, bool withAccount) {
        std::vector<LeaderboardEntry> out;
        while (st.step()) {
            LeaderboardEntry e;
            e.username = st.text(0);
            e.avatar = static_cast<int>(st.integer(1));
            e.value = st.real(2);
            e.updated = st.integer(3);
            if (withAccount) e.account = std::to_string(st.integer(4));
            out.push_back(std::move(e));
        }
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i].rank = i > 0 && out[i].value == out[i - 1].value ? out[i - 1].rank : rankOf(gameId, b, out[i].value);
        }
        return out;
    }

    std::vector<LeaderboardEntry> top(std::int64_t gameId, const LeaderboardDef& b, int limit, bool withAccount) {
        auto st = c.db().prepare(std::format(
            "SELECT u.username, u.avatar, s.value, s.updated, s.user_id FROM leaderboard_scores s JOIN users u ON "
            "u.id = s.user_id WHERE s.game_id = ?1 AND s.board = ?2 AND u.banned = 0 ORDER BY s.value {}, s.updated "
            "LIMIT ?3",
            direction(b)));
        st.bind(1, gameId).bind(2, b.id).bind(3, limit);
        return ranked(st, gameId, b, withAccount);
    }

    std::optional<LeaderboardEntry> entryOf(std::int64_t gameId, const LeaderboardDef& b, std::int64_t userId) {
        auto st = c.db().prepare(
            "SELECT u.username, u.avatar, s.value, s.updated, s.user_id FROM leaderboard_scores s JOIN users u ON "
            "u.id = s.user_id WHERE s.game_id = ?1 AND s.board = ?2 AND s.user_id = ?3 AND u.banned = 0");
        st.bind(1, gameId).bind(2, b.id).bind(3, userId);
        auto rows = ranked(st, gameId, b, false);
        if (rows.empty()) return std::nullopt;
        return rows.front();
    }

    std::vector<LeaderboardEntry> friendsOf(std::int64_t gameId, const LeaderboardDef& b, std::int64_t userId) {
        auto st = c.db().prepare(std::format(
            "SELECT u.username, u.avatar, s.value, s.updated, s.user_id FROM leaderboard_scores s JOIN users u ON "
            "u.id = s.user_id WHERE s.game_id = ?1 AND s.board = ?2 AND u.banned = 0 AND (s.user_id = ?3 OR "
            "s.user_id IN (SELECT friend_id FROM friends WHERE user_id = ?3)) ORDER BY s.value {}, s.updated LIMIT 100",
            direction(b)));
        st.bind(1, gameId).bind(2, b.id).bind(3, userId);
        return ranked(st, gameId, b, false);
    }

    static LeaderboardView viewOf(const LeaderboardDef& b) {
        LeaderboardView v;
        v.id = b.id;
        v.title = b.title;
        v.order = std::string(project::leaderboardOrderName(b.order));
        v.format = std::string(project::scoreFormatName(b.format));
        return v;
    }

    static int limitParam(const httplib::Request& req, int fallback, int most) {
        if (!req.has_param("limit")) return fallback;
        try {
            return std::clamp(std::stoi(req.get_param_value("limit")), 1, most);
        } catch (const std::exception&) {
            fail(400, "bad_request", "limit must be a number");
        }
    }

    // --- players -------------------------------------------------------------------------------
    void list(const httplib::Request& req, httplib::Response& res) {
        const auto viewer = c.currentUser(req);
        const std::int64_t id = pathId(req);
        const GameRow row = c.viewableGame(id, viewer);
        const int limit = limitParam(req, 10, 100);
        std::lock_guard lock(c.dbMutex());
        json boards = json::array();
        for (const LeaderboardDef& b : boardsOf(row.info.id, 0)) {
            LeaderboardView v = viewOf(b);
            v.top = top(row.info.id, b, limit, false);
            if (viewer) {
                v.you = entryOf(row.info.id, b, viewer->id);
                v.friends = friendsOf(row.info.id, b, viewer->id);
            }
            boards.push_back(v);
        }
        sendJson(res, 200, {{"leaderboards", boards}});
    }

    void clear(const httplib::Request& req, httplib::Response& res) {
        const User user = c.requireUser(req);
        const std::int64_t id = pathId(req);
        const std::string board = req.matches[2].str();
        const std::string username = req.matches.size() > 3 && req.matches[3].matched ? req.matches[3].str() : "";
        std::lock_guard lock(c.dbMutex());
        auto row = c.gameById(id);
        if (!row || !(user.admin() || row->info.ownerId == user.id)) fail(404, "not_found", "There is no such game of yours");
        if (username.empty()) {
            auto st = c.db().prepare("DELETE FROM leaderboard_scores WHERE game_id = ?1 AND board = ?2");
            st.bind(1, id).bind(2, board);
            st.run();
        } else {
            auto player = c.userByName(username);
            if (!player) fail(404, "not_found", "There is no such player");
            auto st = c.db().prepare("DELETE FROM leaderboard_scores WHERE game_id = ?1 AND board = ?2 AND user_id = ?3");
            st.bind(1, id).bind(2, board).bind(3, player->id);
            st.run();
        }
        c.audit(user, "clear_leaderboard", std::format("game {} board {}", id, board),
                username.empty() ? "every score" : "the score of " + username);
        sendJson(res, 200, {{"removed", c.db().changes()}});
    }

    // --- game servers --------------------------------------------------------------------------
    void submit(const httplib::Request& req, httplib::Response& res) {
        const std::string session = c.requireServer(req);
        c.limit("scores:" + session, 600, 60);
        const json body = parseBody(req);
        if (!body.contains("scores") || !body["scores"].is_array() || body["scores"].size() > kMaxScoresPerRequest) {
            fail(400, "bad_request", std::format("Send up to {} scores", kMaxScoresPerRequest));
        }
        const auto game = c.fleet().gameOf(session);
        if (!game) fail(404, "not_found", "Unknown session");
        std::lock_guard lock(c.dbMutex());
        const auto boards = boardsOf(*game, c.fleet().versionOf(session));
        Database::Transaction tx(c.db());
        int stored = 0;
        for (const auto& s : body["scores"]) {
            if (!s.is_object() || !s.contains("userId") || !s["userId"].is_string() || !s.contains("board") ||
                !s["board"].is_string() || !s.contains("value") || !s["value"].is_number()) {
                fail(400, "bad_request", "Each score needs userId, board and value");
            }
            const std::string userId = s["userId"].get<std::string>();
            const double value = s["value"].get<double>();
            if (!std::isfinite(value) || std::abs(value) > 1e15) continue;
            // Only players the platform sent to this server, only declared boards.
            if (!c.fleet().holdsPlayer(session, userId)) continue;
            const LeaderboardDef* board = nullptr;
            for (const auto& b : boards) {
                if (b.id == s["board"].get<std::string>()) board = &b;
            }
            if (board == nullptr) continue;
            std::int64_t user = 0;
            try {
                user = std::stoll(userId);
            } catch (const std::exception&) {
                continue;
            }
            auto st = c.db().prepare(std::format(
                "INSERT INTO leaderboard_scores (game_id, board, user_id, value, updated) VALUES (?1, ?2, ?3, ?4, ?5) "
                "ON CONFLICT (game_id, board, user_id) DO UPDATE SET value = excluded.value, updated = excluded.updated "
                "WHERE excluded.value {} leaderboard_scores.value",
                better(*board)));
            st.bind(1, *game).bind(2, board->id).bind(3, user).bind(4, value).bind(5, now());
            st.run();
            stored += c.db().changes() > 0 ? 1 : 0;
        }
        tx.commit();
        sendJson(res, 200, {{"stored", stored}});
    }

    void serverList(const httplib::Request& req, httplib::Response& res) {
        const std::string session = c.requireServer(req);
        const auto game = c.fleet().gameOf(session);
        if (!game) fail(404, "not_found", "Unknown session");
        const int limit = limitParam(req, 100, 200);
        std::vector<std::string> users;
        {
            std::stringstream list(req.get_param_value("users"));
            std::string id;
            while (std::getline(list, id, ',') && users.size() < 256) {
                if (!id.empty() && c.fleet().holdsPlayer(session, id)) users.push_back(id);
            }
        }
        std::lock_guard lock(c.dbMutex());
        json boards = json::array();
        json players = json::object();
        for (const LeaderboardDef& b : boardsOf(*game, c.fleet().versionOf(session))) {
            LeaderboardView v = viewOf(b);
            v.top = top(*game, b, limit, true);
            boards.push_back(v);
            for (const std::string& id : users) {
                std::int64_t user = 0;
                try {
                    user = std::stoll(id);
                } catch (const std::exception&) {
                    continue;
                }
                if (auto e = entryOf(*game, b, user)) players[id][b.id] = *e;
            }
        }
        sendJson(res, 200, {{"leaderboards", boards}, {"players", players}});
    }
};

} // namespace

void registerLeaderboardRoutes(httplib::Server& http, PlatformService::Context& context) {
    auto boards = std::make_shared<Leaderboards>(context);
    const auto bind = [&context, boards](void (Leaderboards::*fn)(const httplib::Request&, httplib::Response&)) {
        return context.wrap([boards, fn](const httplib::Request& req, httplib::Response& res) { ((*boards).*fn)(req, res); });
    };
    http.Get(R"(/api/v1/games/(\d+)/leaderboards)", bind(&Leaderboards::list));
    http.Delete(R"(/api/v1/games/(\d+)/leaderboards/([a-z0-9_]{1,32})(?:/([A-Za-z0-9_]{1,32}))?)",
                bind(&Leaderboards::clear));
    http.Post("/api/v1/server/leaderboards", bind(&Leaderboards::submit));
    http.Get("/api/v1/server/leaderboards", bind(&Leaderboards::serverList));
}

} // namespace px::platform
