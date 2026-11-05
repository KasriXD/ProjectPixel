// People on the platform (docs/social.md): profiles, friends and friend
// requests, blocking, presence, joining friends' games, invitations and
// reports about accounts.
#include "Social.hpp"

#include "ServiceContext.hpp"

#include <algorithm>
#include <memory>

namespace px::platform {

namespace {

using online::FriendInfo;
using online::FriendRequestInfo;
using online::Invitation;
using online::JoinPolicy;
using online::PersonInfo;
using online::Presence;
using online::Profile;
using online::SocialSummary;

constexpr std::int64_t kOnlineSeconds = 120;       // "online": seen this recently
constexpr std::int64_t kInvitationSeconds = 600;   // invitations last ten minutes
constexpr std::size_t kMaxOutgoingRequests = 100;
constexpr std::size_t kMaxBlocks = 1000;

// An account as other people see it.
struct Person {
    std::int64_t id = 0;
    std::string username;
    int avatar = 0;
    std::string bio;
    JoinPolicy joinPolicy = JoinPolicy::Friends;
    std::int64_t lastSeen = 0;
    std::int64_t created = 0;
    Role role = Role::User;
    bool banned = false;

    [[nodiscard]] PersonInfo info() const { return PersonInfo{username, avatar}; }
    [[nodiscard]] UserInfo user() const { return UserInfo{id, username, role, created, false}; }
};

// The columns readPerson() reads, from the users table under the alias u.
constexpr const char* kPersonColumns =
    "u.id, u.username, u.avatar, u.bio, u.join_policy, u.last_seen, u.created, u.role, u.banned";

Person readPerson(const Statement& st) {
    Person p;
    p.id = st.integer(0);
    p.username = st.text(1);
    p.avatar = static_cast<int>(st.integer(2));
    p.bio = st.text(3);
    p.joinPolicy = online::parseJoinPolicy(st.text(4)).value_or(JoinPolicy::Friends);
    p.lastSeen = st.integer(5);
    p.created = st.integer(6);
    p.role = st.text(7) == "admin" ? Role::Admin : Role::User;
    p.banned = st.integer(8) != 0;
    return p;
}

struct Social {
    PlatformService::Context& c;

    explicit Social(PlatformService::Context& context) : c(context) {}

    // --- lookups (with the database locked) ----------------------------------------------------
    std::optional<Person> personByName(std::string_view name) {
        auto st = c.db().prepare(std::format("SELECT {} FROM users u WHERE u.username = ?1 COLLATE NOCASE", kPersonColumns));
        st.bind(1, name);
        if (!st.step()) return std::nullopt;
        return readPerson(st);
    }

    std::optional<Person> personById(std::int64_t id) {
        auto st = c.db().prepare(std::format("SELECT {} FROM users u WHERE u.id = ?1", kPersonColumns));
        st.bind(1, id);
        if (!st.step()) return std::nullopt;
        return readPerson(st);
    }

    // Someone other people may see: exists and is not banned.
    Person visiblePerson(std::string_view name) {
        auto p = personByName(name);
        if (!p || p->banned) fail(404, "not_found", std::format("There is no account named '{}'", name));
        return *p;
    }

    bool areFriends(std::int64_t a, std::int64_t b) {
        auto st = c.db().prepare("SELECT 1 FROM friends WHERE user_id = ?1 AND friend_id = ?2");
        st.bind(1, a).bind(2, b);
        return st.step();
    }

    bool hasBlocked(std::int64_t who, std::int64_t whom) {
        auto st = c.db().prepare("SELECT 1 FROM blocks WHERE user_id = ?1 AND blocked_id = ?2");
        st.bind(1, who).bind(2, whom);
        return st.step();
    }

    bool hasRequest(std::int64_t from, std::int64_t to) {
        auto st = c.db().prepare("SELECT 1 FROM friend_requests WHERE from_id = ?1 AND to_id = ?2");
        st.bind(1, from).bind(2, to);
        return st.step();
    }

    std::int64_t count(const char* sql, std::int64_t id) {
        auto st = c.db().prepare(sql);
        st.bind(1, id);
        (void)st.step();
        return st.integer(0);
    }

    std::string relation(std::int64_t viewer, std::int64_t target) {
        if (viewer == target) return "self";
        if (hasBlocked(viewer, target)) return "blocked";
        if (areFriends(viewer, target)) return "friend";
        if (hasRequest(viewer, target)) return "requested";
        if (hasRequest(target, viewer)) return "incoming";
        return "none";
    }

    // An unexpired invitation from `from` to `to` for the game server `session`.
    bool invited(std::int64_t from, std::int64_t to, const std::string& session) {
        auto st = c.db().prepare(
            "SELECT 1 FROM invitations WHERE from_id = ?1 AND to_id = ?2 AND session = ?3 AND expires > ?4");
        st.bind(1, from).bind(2, to).bind(3, session).bind(4, now());
        return st.step();
    }

    // What `viewer` (a friend, or the person themselves) sees of `p`.
    Presence presenceOf(const User& viewer, const Person& p) {
        Presence out;
        if (const auto where = c.fleet().whereIs(std::to_string(p.id))) {
            out.state = "playing";
            if (auto row = c.gameById(where->gameId); row && PlatformService::Context::mayView(viewer, *row)) {
                out.gameId = row->info.id;
                out.gameTitle = row->info.title;
                const bool allowed = viewer.id != p.id && !row->info.hidden &&
                                     (p.joinPolicy == JoinPolicy::Friends || invited(p.id, viewer.id, where->session));
                out.joinable = allowed;
            }
            return out;
        }
        out.state = now() - p.lastSeen <= kOnlineSeconds ? "online" : "offline";
        return out;
    }

    void makeFriends(std::int64_t a, std::int64_t b) {
        for (const auto& [x, y] : {std::pair{a, b}, std::pair{b, a}}) {
            auto st = c.db().prepare("INSERT OR IGNORE INTO friends (user_id, friend_id, created) VALUES (?1, ?2, ?3)");
            st.bind(1, x).bind(2, y).bind(3, now());
            st.run();
        }
        auto del = c.db().prepare(
            "DELETE FROM friend_requests WHERE (from_id = ?1 AND to_id = ?2) OR (from_id = ?2 AND to_id = ?1)");
        del.bind(1, a).bind(2, b);
        del.run();
    }

    void forgetBetween(std::int64_t a, std::int64_t b, bool friendship) {
        if (friendship) {
            auto st = c.db().prepare(
                "DELETE FROM friends WHERE (user_id = ?1 AND friend_id = ?2) OR (user_id = ?2 AND friend_id = ?1)");
            st.bind(1, a).bind(2, b);
            st.run();
        }
        auto req = c.db().prepare(
            "DELETE FROM friend_requests WHERE (from_id = ?1 AND to_id = ?2) OR (from_id = ?2 AND to_id = ?1)");
        req.bind(1, a).bind(2, b);
        req.run();
        auto inv = c.db().prepare(
            "DELETE FROM invitations WHERE (from_id = ?1 AND to_id = ?2) OR (from_id = ?2 AND to_id = ?1)");
        inv.bind(1, a).bind(2, b);
        inv.run();
    }

    void checkFriendRoom(std::int64_t id, const std::string& name) {
        if (count("SELECT COUNT(*) FROM friends WHERE user_id = ?1", id) >= static_cast<std::int64_t>(online::kMaxFriends)) {
            fail(409, "too_many_friends", std::format("{} already has {} friends, the most there can be",
                                                       name, online::kMaxFriends));
        }
    }

    // --- profiles ------------------------------------------------------------------------------
    void profile(const httplib::Request& req, httplib::Response& res) {
        const auto viewer = c.currentUser(req);
        std::lock_guard lock(c.dbMutex());
        auto p = personByName(req.matches[1].str());
        if (!p || (p->banned && !(viewer && viewer->admin()))) fail(404, "not_found", "There is no such account");
        Profile out;
        out.user = p->user();
        out.bio = p->bio;
        out.avatar = p->avatar;
        out.joinPolicy = p->joinPolicy;
        out.friends = static_cast<int>(count("SELECT COUNT(*) FROM friends WHERE user_id = ?1", p->id));
        if (viewer) {
            out.relation = relation(viewer->id, p->id);
            if (out.relation == "friend" || out.relation == "self") out.presence = presenceOf(*viewer, *p);
        }
        auto st = c.db().prepare(std::format(
            "SELECT {} FROM games g JOIN users u ON u.id = g.owner_id WHERE g.owner_id = ?1 AND g.visibility = 'public' "
            "AND g.hidden = 0 AND g.latest_version > 0 ORDER BY g.plays DESC LIMIT 50",
            kGameColumns));
        st.bind(1, p->id);
        std::vector<GameRow> rows;
        while (st.step()) rows.push_back(readGame(st));
        for (auto& row : rows) out.games.push_back(c.gameJson(row, viewer).get<GameInfo>());
        sendJson(res, 200, {{"profile", out}});
    }

    void updateProfile(const httplib::Request& req, httplib::Response& res) {
        const User me = c.requireUser(req);
        c.limit("profile:" + std::to_string(me.id), 30, 3600);
        const json body = parseBody(req);
        std::optional<std::string> bio;
        if (body.contains("bio")) {
            bio = cleanText(stringField(body, "bio", online::kMaxBioLength * 4, false, "The bio"), "The bio");
            // Count characters, not bytes (UTF-8 continuation bytes are 10xxxxxx).
            const auto characters = std::count_if(bio->begin(), bio->end(),
                                                  [](char ch) { return (static_cast<unsigned char>(ch) & 0xC0) != 0x80; });
            if (characters > static_cast<std::ptrdiff_t>(online::kMaxBioLength)) {
                fail(400, "bad_request", std::format("The bio is longer than {} characters", online::kMaxBioLength));
            }
            const auto lines = std::count(bio->begin(), bio->end(), '\n');
            if (lines > 4) fail(400, "bad_request", "The bio may have at most 5 lines");
        }
        std::optional<int> avatar;
        if (body.contains("avatar")) {
            if (!body["avatar"].is_number_integer()) fail(400, "bad_request", "avatar must be a number");
            avatar = body["avatar"].get<int>();
            if (*avatar < 0 || *avatar >= online::kAvatarCount) {
                fail(400, "bad_request", std::format("avatar must be 0 to {}", online::kAvatarCount - 1));
            }
        }
        std::optional<JoinPolicy> policy;
        if (body.contains("joinPolicy")) {
            policy = online::parseJoinPolicy(stringField(body, "joinPolicy", 20, false, "joinPolicy"));
            if (!policy) fail(400, "bad_request", "joinPolicy must be \"friends\" or \"nobody\"");
        }
        {
            std::lock_guard lock(c.dbMutex());
            if (bio) {
                auto st = c.db().prepare("UPDATE users SET bio = ?1 WHERE id = ?2");
                st.bind(1, *bio).bind(2, me.id);
                st.run();
            }
            if (avatar) {
                auto st = c.db().prepare("UPDATE users SET avatar = ?1 WHERE id = ?2");
                st.bind(1, *avatar).bind(2, me.id);
                st.run();
            }
            if (policy) {
                auto st = c.db().prepare("UPDATE users SET join_policy = ?1 WHERE id = ?2");
                st.bind(1, online::joinPolicyName(*policy)).bind(2, me.id);
                st.run();
            }
        }
        profileOf(me, res);
    }

    void profileOf(const User& me, httplib::Response& res) {
        std::lock_guard lock(c.dbMutex());
        const auto p = personById(me.id);
        Profile out;
        out.user = p->user();
        out.bio = p->bio;
        out.avatar = p->avatar;
        out.joinPolicy = p->joinPolicy;
        out.relation = "self";
        out.presence = presenceOf(me, *p);
        out.friends = static_cast<int>(count("SELECT COUNT(*) FROM friends WHERE user_id = ?1", me.id));
        sendJson(res, 200, {{"profile", out}});
    }

    void searchUsers(const httplib::Request& req, httplib::Response& res) {
        const auto viewer = c.currentUser(req);
        const std::string prefix = req.get_param_value("search").substr(0, online::kMaxUsernameLength);
        if (prefix.size() < 2) fail(400, "bad_request", "Type at least two letters of the name");
        std::string pattern;
        for (const char ch : prefix) {
            if (ch == '%' || ch == '_' || ch == '\\') pattern += '\\';
            pattern += ch;
        }
        pattern += '%';
        std::lock_guard lock(c.dbMutex());
        auto st = c.db().prepare(
            "SELECT username, avatar FROM users WHERE username LIKE ?1 ESCAPE '\\' AND banned = 0 ORDER BY username "
            "COLLATE NOCASE LIMIT 20");
        st.bind(1, pattern);
        json people = json::array();
        while (st.step()) people.push_back(PersonInfo{st.text(0), static_cast<int>(st.integer(1))});
        (void)viewer;
        sendJson(res, 200, {{"users", people}});
    }

    // --- friends -------------------------------------------------------------------------------
    void summary(const httplib::Request& req, httplib::Response& res) {
        const User me = c.requireUser(req);
        SocialSummary out;
        std::lock_guard lock(c.dbMutex());
        {
            auto st = c.db().prepare(std::format(
                "SELECT {}, f.created FROM friends f JOIN users u ON u.id = f.friend_id WHERE f.user_id = ?1 "
                "AND u.banned = 0 ORDER BY u.username COLLATE NOCASE",
                kPersonColumns));
            st.bind(1, me.id);
            while (st.step()) {
                const Person p = readPerson(st);
                FriendInfo f{p.username, p.avatar, presenceOf(me, p), st.integer(9)};
                out.friends.push_back(std::move(f));
            }
        }
        const auto requests = [&](const char* sql, std::vector<FriendRequestInfo>& into) {
            auto st = c.db().prepare(sql);
            st.bind(1, me.id);
            while (st.step()) into.push_back({st.text(0), static_cast<int>(st.integer(1)), st.integer(2)});
        };
        requests("SELECT u.username, u.avatar, r.created FROM friend_requests r JOIN users u ON u.id = r.from_id "
                 "WHERE r.to_id = ?1 AND u.banned = 0 ORDER BY r.created",
                 out.incoming);
        requests("SELECT u.username, u.avatar, r.created FROM friend_requests r JOIN users u ON u.id = r.to_id "
                 "WHERE r.from_id = ?1 AND u.banned = 0 ORDER BY r.created",
                 out.outgoing);
        {
            auto st = c.db().prepare(
                "SELECT i.id, u.username, u.avatar, i.game_id, g.title, i.expires, i.session, i.from_id "
                "FROM invitations i JOIN users u ON u.id = i.from_id JOIN games g ON g.id = i.game_id "
                "WHERE i.to_id = ?1 AND i.expires > ?2 AND u.banned = 0 ORDER BY i.created DESC LIMIT 20");
            st.bind(1, me.id).bind(2, now());
            while (st.step()) {
                // Only while the friend is still in that game.
                const auto where = c.fleet().whereIs(std::to_string(st.integer(7)));
                if (!where || where->session != st.text(6)) continue;
                out.invitations.push_back(
                    {st.integer(0), st.text(1), static_cast<int>(st.integer(2)), st.integer(3), st.text(4), st.integer(5)});
            }
        }
        sendJson(res, 200, {{"social", out}});
    }

    void requestFriend(const httplib::Request& req, httplib::Response& res) {
        const User me = c.requireUser(req);
        c.limit("friend-request:" + std::to_string(me.id), 30, 3600);
        const json body = parseBody(req);
        const std::string name = stringField(body, "username", 64, true, "The name");
        std::lock_guard lock(c.dbMutex());
        Database::Transaction tx(c.db());
        const Person other = visiblePerson(name);
        if (other.id == me.id) fail(400, "bad_request", "You cannot be your own friend");
        if (hasBlocked(me.id, other.id)) fail(409, "blocked", std::format("Unblock {} first", other.username));
        if (hasBlocked(other.id, me.id)) fail(403, "forbidden", std::format("You cannot send {} a friend request", other.username));
        std::string status = "requested";
        if (areFriends(me.id, other.id)) {
            status = "friends";
        } else if (hasRequest(other.id, me.id)) {
            checkFriendRoom(me.id, "You");
            checkFriendRoom(other.id, other.username);
            makeFriends(me.id, other.id);
            status = "friends";
        } else {
            checkFriendRoom(me.id, "You");
            if (count("SELECT COUNT(*) FROM friend_requests WHERE from_id = ?1", me.id) >=
                static_cast<std::int64_t>(kMaxOutgoingRequests)) {
                fail(409, "too_many_requests", "You have too many friend requests waiting for an answer");
            }
            auto st = c.db().prepare("INSERT OR IGNORE INTO friend_requests (from_id, to_id, created) VALUES (?1, ?2, ?3)");
            st.bind(1, me.id).bind(2, other.id).bind(3, now());
            st.run();
        }
        tx.commit();
        sendJson(res, 200, {{"status", status}, {"username", other.username}});
    }

    void acceptFriend(const httplib::Request& req, httplib::Response& res) {
        const User me = c.requireUser(req);
        std::lock_guard lock(c.dbMutex());
        Database::Transaction tx(c.db());
        const Person other = visiblePerson(req.matches[1].str());
        if (!hasRequest(other.id, me.id)) {
            fail(404, "not_found", std::format("{} has not asked to be your friend", other.username));
        }
        checkFriendRoom(me.id, "You");
        checkFriendRoom(other.id, other.username);
        makeFriends(me.id, other.id);
        tx.commit();
        sendJson(res, 200, {{"status", "friends"}, {"username", other.username}});
    }

    void removeRequest(const httplib::Request& req, httplib::Response& res) {
        const User me = c.requireUser(req);
        std::lock_guard lock(c.dbMutex());
        auto other = personByName(req.matches[1].str());
        if (other) {
            auto st = c.db().prepare(
                "DELETE FROM friend_requests WHERE (from_id = ?1 AND to_id = ?2) OR (from_id = ?2 AND to_id = ?1)");
            st.bind(1, me.id).bind(2, other->id);
            st.run();
        }
        sendJson(res, 200, json::object());
    }

    void removeFriend(const httplib::Request& req, httplib::Response& res) {
        const User me = c.requireUser(req);
        std::lock_guard lock(c.dbMutex());
        if (auto other = personByName(req.matches[1].str())) forgetBetween(me.id, other->id, true);
        sendJson(res, 200, json::object());
    }

    // --- playing together ----------------------------------------------------------------------
    void joinFriend(const httplib::Request& req, httplib::Response& res) {
        const User me = c.requireUser(req);
        c.limit("join:" + std::to_string(me.id), 30, 60);
        std::string session;
        GameRow row;
        int version = 0;
        std::string packageHash;
        {
            std::lock_guard lock(c.dbMutex());
            const Person other = visiblePerson(req.matches[1].str());
            if (!areFriends(me.id, other.id)) fail(403, "not_friends", std::format("{} is not your friend", other.username));
            const auto where = c.fleet().whereIs(std::to_string(other.id));
            if (!where) fail(409, "not_playing", std::format("{} is not in a game now", other.username));
            if (other.joinPolicy == JoinPolicy::Nobody && !invited(other.id, me.id, where->session)) {
                fail(403, "not_allowed", std::format("{} lets friends join only when invited", other.username));
            }
            auto game = c.gameById(where->gameId);
            if (!game || !PlatformService::Context::mayView(me, *game)) {
                fail(403, "not_allowed", std::format("{} is in a game you cannot play", other.username));
            }
            if (game->info.hidden && !me.admin()) fail(403, "hidden", "This game has been taken down by a moderator");
            auto st = c.db().prepare("SELECT package_hash FROM versions WHERE game_id = ?1 AND number = ?2");
            st.bind(1, where->gameId).bind(2, where->version);
            if (!st.step()) fail(409, "not_found", "That version of the game is gone");
            packageHash = st.text(0);
            session = where->session;
            version = where->version;
            row = *game;
        }
        auto seat = c.fleet().reserveIn(session, std::to_string(me.id));
        if (!seat) PlatformService::Context::failSeat(seat.error());
        sendJson(res, 200, {{"ticket", c.issueTicket(me, row.info.id, version, packageHash, *seat)}});
    }

    void invite(const httplib::Request& req, httplib::Response& res) {
        const User me = c.requireUser(req);
        c.limit("invite:" + std::to_string(me.id), 30, 600);
        const auto where = c.fleet().whereIs(std::to_string(me.id));
        if (!where) fail(409, "not_playing", "Start a game first, then invite friends to it");
        std::lock_guard lock(c.dbMutex());
        Database::Transaction tx(c.db());
        const Person other = visiblePerson(req.matches[1].str());
        if (!areFriends(me.id, other.id)) fail(403, "not_friends", std::format("{} is not your friend", other.username));
        auto del = c.db().prepare("DELETE FROM invitations WHERE from_id = ?1 AND to_id = ?2");
        del.bind(1, me.id).bind(2, other.id);
        del.run();
        auto st = c.db().prepare(
            "INSERT INTO invitations (from_id, to_id, game_id, session, created, expires) VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
        st.bind(1, me.id).bind(2, other.id).bind(3, where->gameId).bind(4, where->session).bind(5, now()).bind(
            6, now() + kInvitationSeconds);
        st.run();
        const std::int64_t id = c.db().lastInsertId();
        tx.commit();
        sendJson(res, 201, {{"invitation", id}});
    }

    void dismissInvitation(const httplib::Request& req, httplib::Response& res) {
        const User me = c.requireUser(req);
        std::lock_guard lock(c.dbMutex());
        auto st = c.db().prepare("DELETE FROM invitations WHERE id = ?1 AND to_id = ?2");
        st.bind(1, pathId(req)).bind(2, me.id);
        st.run();
        sendJson(res, 200, json::object());
    }

    // --- blocking ------------------------------------------------------------------------------
    void listBlocks(const httplib::Request& req, httplib::Response& res) {
        const User me = c.requireUser(req);
        std::lock_guard lock(c.dbMutex());
        auto st = c.db().prepare(
            "SELECT u.username, u.avatar FROM blocks b JOIN users u ON u.id = b.blocked_id WHERE b.user_id = ?1 "
            "ORDER BY u.username COLLATE NOCASE");
        st.bind(1, me.id);
        json people = json::array();
        while (st.step()) people.push_back(PersonInfo{st.text(0), static_cast<int>(st.integer(1))});
        sendJson(res, 200, {{"blocked", people}});
    }

    void block(const httplib::Request& req, httplib::Response& res) {
        const User me = c.requireUser(req);
        c.limit("block:" + std::to_string(me.id), 60, 3600);
        const json body = parseBody(req);
        const std::string name = stringField(body, "username", 64, true, "The name");
        std::lock_guard lock(c.dbMutex());
        Database::Transaction tx(c.db());
        auto other = personByName(name);
        if (!other) fail(404, "not_found", std::format("There is no account named '{}'", name));
        if (other->id == me.id) fail(400, "bad_request", "You cannot block yourself");
        if (count("SELECT COUNT(*) FROM blocks WHERE user_id = ?1", me.id) >= static_cast<std::int64_t>(kMaxBlocks)) {
            fail(409, "too_many", "You have blocked as many accounts as you can");
        }
        auto st = c.db().prepare("INSERT OR IGNORE INTO blocks (user_id, blocked_id, created) VALUES (?1, ?2, ?3)");
        st.bind(1, me.id).bind(2, other->id).bind(3, now());
        st.run();
        forgetBetween(me.id, other->id, true);
        tx.commit();
        sendJson(res, 200, {{"username", other->username}});
    }

    void unblock(const httplib::Request& req, httplib::Response& res) {
        const User me = c.requireUser(req);
        std::lock_guard lock(c.dbMutex());
        if (auto other = personByName(req.matches[1].str())) {
            auto st = c.db().prepare("DELETE FROM blocks WHERE user_id = ?1 AND blocked_id = ?2");
            st.bind(1, me.id).bind(2, other->id);
            st.run();
        }
        sendJson(res, 200, json::object());
    }

    // --- reports about accounts ----------------------------------------------------------------
    void reportUser(const httplib::Request& req, httplib::Response& res) {
        const User me = c.requireUser(req);
        c.limit("report:" + std::to_string(me.id), 10, 3600);
        const json body = parseBody(req);
        const std::string reason =
            cleanText(stringField(body, "reason", online::kMaxReportLength, true, "The reason"), "The reason");
        if (reason.empty()) fail(400, "bad_request", "Say what is wrong");
        std::lock_guard lock(c.dbMutex());
        const Person other = visiblePerson(req.matches[1].str());
        if (other.id == me.id) fail(400, "bad_request", "You cannot report yourself");
        auto st = c.db().prepare("INSERT INTO user_reports (user_id, reporter_id, reason, created) VALUES (?1, ?2, ?3, ?4)");
        st.bind(1, other.id).bind(2, me.id).bind(3, reason).bind(4, now());
        st.run();
        sendJson(res, 201, {{"report", c.db().lastInsertId()}});
    }

    void listUserReports(const httplib::Request& req, httplib::Response& res) {
        (void)c.requireAdmin(req);
        const bool openOnly = req.get_param_value("state") != "all";
        std::lock_guard lock(c.dbMutex());
        auto st = c.db().prepare(std::format(
            "SELECT r.id, u.username, rep.username, r.reason, r.state, r.resolution, r.created FROM user_reports r "
            "JOIN users u ON u.id = r.user_id JOIN users rep ON rep.id = r.reporter_id {} ORDER BY r.id DESC LIMIT 500",
            openOnly ? "WHERE r.state = 'open'" : ""));
        json reports = json::array();
        while (st.step()) {
            reports.push_back(online::UserReportInfo{st.integer(0), st.text(1), st.text(2), st.text(3), st.text(4),
                                                     st.text(5), st.integer(6)});
        }
        sendJson(res, 200, {{"reports", reports}});
    }

    void resolveUserReport(const httplib::Request& req, httplib::Response& res) {
        const User admin = c.requireAdmin(req);
        const json body = parseBody(req);
        const std::string action = stringField(body, "action", 32, true, "The action");
        const std::string note = cleanText(stringField(body, "note", 1000, false, "The note"), "The note");
        if (action != "dismiss" && action != "clear_profile" && action != "ban") {
            fail(400, "bad_request", "action must be dismiss, clear_profile or ban");
        }
        std::lock_guard lock(c.dbMutex());
        Database::Transaction tx(c.db());
        auto st = c.db().prepare("SELECT user_id, state FROM user_reports WHERE id = ?1");
        st.bind(1, pathId(req));
        if (!st.step()) fail(404, "not_found", "There is no such report");
        const std::int64_t userId = st.integer(0);
        auto target = c.userById(userId);
        if (!target) fail(404, "not_found", "The account is gone");
        if (action == "clear_profile") {
            auto clear = c.db().prepare("UPDATE users SET bio = '', avatar = 0 WHERE id = ?1");
            clear.bind(1, userId);
            clear.run();
            c.audit(admin, "clear_profile", "user " + target->username, note);
        } else if (action == "ban") {
            c.banUser(admin, *target, true, note);
        }
        auto done = c.db().prepare(
            "UPDATE user_reports SET state = 'resolved', resolution = ?1, resolved_by = ?2, resolved_at = ?3 WHERE id = ?4");
        done.bind(1, action + (note.empty() ? "" : ": " + note)).bind(2, admin.id).bind(3, now()).bind(4, pathId(req));
        done.run();
        c.audit(admin, "resolve_user_report", std::format("report {}", pathId(req)), action);
        tx.commit();
        sendJson(res, 200, json::object());
    }
};

} // namespace

void registerSocialRoutes(httplib::Server& http, PlatformService::Context& context) {
    auto social = std::make_shared<Social>(context);
    const auto bind = [&context, social](void (Social::*fn)(const httplib::Request&, httplib::Response&)) {
        return context.wrap([social, fn](const httplib::Request& req, httplib::Response& res) { ((*social).*fn)(req, res); });
    };
    constexpr const char* kName = "([A-Za-z0-9_]{1,32})";
    http.Get("/api/v1/users", bind(&Social::searchUsers));
    http.Get(std::format("/api/v1/users/{}", kName), bind(&Social::profile));
    http.Post(std::format("/api/v1/users/{}/reports", kName), bind(&Social::reportUser));
    http.Patch("/api/v1/me", bind(&Social::updateProfile));
    http.Get("/api/v1/me/social", bind(&Social::summary));
    http.Post("/api/v1/friends/requests", bind(&Social::requestFriend));
    http.Post(std::format("/api/v1/friends/requests/{}/accept", kName), bind(&Social::acceptFriend));
    http.Delete(std::format("/api/v1/friends/requests/{}", kName), bind(&Social::removeRequest));
    http.Delete(std::format("/api/v1/friends/{}", kName), bind(&Social::removeFriend));
    http.Post(std::format("/api/v1/friends/{}/join", kName), bind(&Social::joinFriend));
    http.Post(std::format("/api/v1/friends/{}/invite", kName), bind(&Social::invite));
    http.Delete(R"(/api/v1/invitations/(\d{1,18}))", bind(&Social::dismissInvitation));
    http.Get("/api/v1/blocks", bind(&Social::listBlocks));
    http.Post("/api/v1/blocks", bind(&Social::block));
    http.Delete(std::format("/api/v1/blocks/{}", kName), bind(&Social::unblock));
    http.Get("/api/v1/admin/user-reports", bind(&Social::listUserReports));
    http.Post(R"(/api/v1/admin/user-reports/(\d{1,18})/resolve)", bind(&Social::resolveUserReport));
}

} // namespace px::platform
