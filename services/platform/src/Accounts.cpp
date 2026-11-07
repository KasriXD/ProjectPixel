// Accounts beyond logging in (docs/accounts.md): an email address checked
// by a link, password resets by email, changing the password, deleting the
// account and downloading its data.
#include "Accounts.hpp"

#include "ServiceContext.hpp"

#include <algorithm>
#include <cctype>
#include <memory>

namespace px::platform {

namespace {

constexpr std::int64_t kVerifySeconds = 24 * 3600;
constexpr std::int64_t kResetSeconds = 3600;
constexpr std::size_t kMaxEmailLength = 254;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string tokenHash(const std::string& token) { return crypto::toHex(crypto::hash256(token)); }

struct Accounts {
    PlatformService::Context& c;
    explicit Accounts(PlatformService::Context& context) : c(context) {}

    // --- helpers -----------------------------------------------------------------------------
    std::string platformName() const { return c.config().name; }

    // The stored password hash and email of an account (database locked).
    struct Secrets {
        std::string passwordHash;
        std::string email;
        bool emailVerified = false;
    };
    Secrets secrets(std::int64_t id) {
        auto st = c.db().prepare("SELECT password_hash, email, email_verified FROM users WHERE id = ?1");
        st.bind(1, id);
        Secrets s;
        if (st.step()) {
            s.passwordHash = st.text(0);
            s.email = st.text(1);
            s.emailVerified = st.integer(2) != 0;
        }
        return s;
    }

    // Checks the password of the account making a sensitive change.
    void confirmPassword(const User& user, const std::string& password) {
        std::string stored;
        {
            std::lock_guard lock(c.dbMutex());
            stored = secrets(user.id).passwordHash;
        }
        if (!crypto::verifyPassword(password, stored)) fail(403, "bad_password", "That is not your password");
    }

    // A one-time link token (database locked).
    std::string newToken(std::int64_t userId, const char* purpose, const std::string& email, std::int64_t seconds) {
        auto token = crypto::randomToken(32);
        if (!token) throw std::runtime_error(token.error().message);
        auto del = c.db().prepare("DELETE FROM account_tokens WHERE user_id = ?1 AND purpose = ?2");
        del.bind(1, userId).bind(2, purpose);
        del.run();
        auto st = c.db().prepare(
            "INSERT INTO account_tokens (token_hash, user_id, purpose, email, created, expires) VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
        st.bind(1, tokenHash(token.value())).bind(2, userId).bind(3, purpose).bind(4, email).bind(5, now()).bind(6, now() + seconds);
        st.run();
        return token.value();
    }

    // The account a link token belongs to (database locked), or a 400.
    struct Redeemed {
        std::int64_t userId = 0;
        std::string email;
    };
    Redeemed redeem(const std::string& token, const char* purpose) {
        if (token.empty() || token.size() > 100) fail(400, "bad_link", "This link is not valid");
        auto st = c.db().prepare("SELECT user_id, email, expires FROM account_tokens WHERE token_hash = ?1 AND purpose = ?2");
        st.bind(1, tokenHash(token)).bind(2, purpose);
        if (!st.step()) fail(400, "bad_link", "This link is not valid, or was used already");
        Redeemed r{st.integer(0), st.text(1)};
        const std::int64_t expires = st.integer(2);
        auto del = c.db().prepare("DELETE FROM account_tokens WHERE token_hash = ?1");
        del.bind(1, tokenHash(token));
        del.run();
        if (expires < now()) fail(400, "expired_link", "This link has expired; ask for a new one");
        return r;
    }

    void sendVerification(const User& user, const std::string& email, const std::string& token) {
        Mail mail;
        mail.to = email;
        mail.subject = std::format("Confirm your email address for {}", platformName());
        mail.text = std::format(
            "Hello {},\n\n"
            "Open this link to confirm that {} is your email address:\n\n"
            "{}/play#verify={}\n\n"
            "The link works for 24 hours. If you did not ask for this, ignore this message.\n\n"
            "{}\n",
            user.username, email, c.s.publicUrl(), token, platformName());
        c.mailer().send(std::move(mail));
    }

    void sendReset(const std::string& username, const std::string& email, const std::string& token) {
        Mail mail;
        mail.to = email;
        mail.subject = std::format("Reset your password on {}", platformName());
        mail.text = std::format(
            "Hello {},\n\n"
            "Someone, hopefully you, asked to reset the password of the account {}. "
            "Open this link within an hour to choose a new one:\n\n"
            "{}/play#reset={}\n\n"
            "If it was not you, ignore this message: your password stays as it is.\n\n"
            "{}\n",
            username, username, c.s.publicUrl(), token, platformName());
        c.mailer().send(std::move(mail));
    }

    void requireMail() {
        if (!c.mailer().enabled()) fail(503, "no_mail", "This platform cannot send email (ask its operator)");
    }

    // --- handlers ----------------------------------------------------------------------------

    // GET /me/account: the account's private settings.
    void account(const httplib::Request& req, httplib::Response& res) {
        const User user = c.requireUser(req);
        std::lock_guard lock(c.dbMutex());
        const Secrets s = secrets(user.id);
        sendJson(res, 200, {{"email", s.email},
                            {"emailVerified", s.emailVerified},
                            {"mail", c.mailer().enabled()},
                            {"emailRequiredToPublish", c.config().requireVerifiedEmail}});
    }

    // PUT /me/email {email, password}: set (or with "" remove) the address; a link confirms it.
    void setEmail(const httplib::Request& req, httplib::Response& res) {
        const User user = c.requireUser(req);
        c.limit("email:" + std::to_string(user.id), 5, 3600);
        const json body = parseBody(req);
        const std::string email = lower(cleanLine(stringField(body, "email", kMaxEmailLength, true, "The email address"),
                                                  "The email address"));
        const std::string password = stringField(body, "password", online::kMaxPasswordLength, true, "Your password");
        if (!email.empty() && !isValidEmail(email)) fail(400, "bad_email", "That does not look like an email address");
        if (!email.empty()) requireMail();
        confirmPassword(user, password);
        std::string token;
        {
            std::lock_guard lock(c.dbMutex());
            Database::Transaction tx(c.db());
            auto st = c.db().prepare("UPDATE users SET email = ?1, email_verified = 0 WHERE id = ?2");
            st.bind(1, email).bind(2, user.id);
            st.run();
            auto del = c.db().prepare("DELETE FROM account_tokens WHERE user_id = ?1");
            del.bind(1, user.id);
            del.run();
            if (!email.empty()) token = newToken(user.id, "verify", email, kVerifySeconds);
            tx.commit();
        }
        if (!email.empty()) sendVerification(user, email, token);
        log::info("Account {} {} its email address", user.username, email.empty() ? "removed" : "changed");
        sendJson(res, 200, {{"email", email}, {"emailVerified", false}});
    }

    // POST /me/email/verification: the link again.
    void resendVerification(const httplib::Request& req, httplib::Response& res) {
        const User user = c.requireUser(req);
        c.limit("email:" + std::to_string(user.id), 5, 3600);
        requireMail();
        std::string email;
        std::string token;
        {
            std::lock_guard lock(c.dbMutex());
            const Secrets s = secrets(user.id);
            if (s.email.empty()) fail(400, "no_email", "Add an email address first");
            if (s.emailVerified) fail(409, "verified", "Your email address is confirmed already");
            email = s.email;
            token = newToken(user.id, "verify", email, kVerifySeconds);
        }
        sendVerification(user, email, token);
        sendJson(res, 202, {{"status", "sent"}});
    }

    // POST /email-verifications {token}: from the link in the email (no login needed).
    void verifyEmail(const httplib::Request& req, httplib::Response& res) {
        c.limit("link:" + c.clientAddress(req), 30, 3600);
        const json body = parseBody(req);
        const std::string token = stringField(body, "token", 100, true, "The link");
        std::lock_guard lock(c.dbMutex());
        Database::Transaction tx(c.db());
        const Redeemed r = redeem(token, "verify");
        const Secrets s = secrets(r.userId);
        if (s.email != r.email) fail(400, "bad_link", "This link is for an address the account no longer has");
        auto st = c.db().prepare("UPDATE users SET email_verified = 1 WHERE id = ?1");
        st.bind(1, r.userId);
        st.run();
        tx.commit();
        auto user = c.userById(r.userId);
        log::info("Account {} confirmed its email address", user ? user->username : "?");
        sendJson(res, 200, {{"username", user ? user->username : std::string()}, {"email", r.email}});
    }

    // POST /password-resets {name}: a name or an email address. The answer is
    // the same whether or not an account matches.
    void requestReset(const httplib::Request& req, httplib::Response& res) {
        c.limit("reset-ip:" + c.clientAddress(req), 10, 3600);
        const json body = parseBody(req);
        const std::string name = lower(cleanLine(stringField(body, "name", kMaxEmailLength, true, "The name or email address"),
                                                 "The name or email address"));
        if (name.empty()) fail(400, "bad_request", "Give your name or email address");
        c.limit("reset:" + name, 3, 3600);
        requireMail();
        struct Target {
            std::string username;
            std::string email;
            std::string token;
        };
        std::vector<Target> targets;
        {
            std::lock_guard lock(c.dbMutex());
            auto st = c.db().prepare(
                name.find('@') != std::string::npos
                    ? "SELECT id, username, email FROM users WHERE email = ?1 AND email_verified = 1 AND banned = 0 AND deleted = 0 LIMIT 5"
                    : "SELECT id, username, email FROM users WHERE username = ?1 COLLATE NOCASE AND email_verified = 1 "
                      "AND banned = 0 AND deleted = 0");
            st.bind(1, name);
            std::vector<std::tuple<std::int64_t, std::string, std::string>> rows;
            while (st.step()) rows.emplace_back(st.integer(0), st.text(1), st.text(2));
            for (const auto& [id, username, email] : rows) {
                targets.push_back({username, email, newToken(id, "reset", email, kResetSeconds)});
            }
        }
        for (const auto& t : targets) sendReset(t.username, t.email, t.token);
        if (!targets.empty()) log::info("Password reset link sent for {} account(s)", targets.size());
        sendJson(res, 202, {{"status", "If an account with a confirmed email address matches, a link is on its way."}});
    }

    // POST /password-resets/complete {token, password}: from the link.
    void completeReset(const httplib::Request& req, httplib::Response& res) {
        c.limit("link:" + c.clientAddress(req), 30, 3600);
        const json body = parseBody(req);
        const std::string token = stringField(body, "token", 100, true, "The link");
        const std::string password = stringField(body, "password", online::kMaxPasswordLength, true, "The new password");
        if (password.size() < online::kMinPasswordLength) {
            fail(400, "weak_password", std::format("Passwords need at least {} characters", online::kMinPasswordLength));
        }
        auto hash = crypto::hashPassword(password, c.config().passwordHashing);
        if (!hash) throw std::runtime_error(hash.error().message);
        std::lock_guard lock(c.dbMutex());
        Database::Transaction tx(c.db());
        const Redeemed r = redeem(token, "reset");
        auto st = c.db().prepare("UPDATE users SET password_hash = ?1 WHERE id = ?2 AND deleted = 0");
        st.bind(1, hash.value()).bind(2, r.userId);
        st.run();
        // Logged out everywhere: whoever knew the old password is out too.
        auto del = c.db().prepare("DELETE FROM sessions WHERE user_id = ?1");
        del.bind(1, r.userId);
        del.run();
        auto user = c.userById(r.userId);
        if (!user) fail(400, "bad_link", "This link is not valid");
        c.audit(*user, "password_reset", "user " + user->username, "by email link");
        tx.commit();
        sendJson(res, 200, {{"username", user->username}});
    }

    // PUT /me/password {current, password}: other sessions end.
    void changePassword(const httplib::Request& req, httplib::Response& res) {
        const User user = c.requireUser(req);
        c.limit("password:" + std::to_string(user.id), 10, 3600);
        const json body = parseBody(req);
        const std::string current = stringField(body, "current", online::kMaxPasswordLength, true, "Your password");
        const std::string password = stringField(body, "password", online::kMaxPasswordLength, true, "The new password");
        if (password.size() < online::kMinPasswordLength) {
            fail(400, "weak_password", std::format("Passwords need at least {} characters", online::kMinPasswordLength));
        }
        confirmPassword(user, current);
        auto hash = crypto::hashPassword(password, c.config().passwordHashing);
        if (!hash) throw std::runtime_error(hash.error().message);
        std::lock_guard lock(c.dbMutex());
        Database::Transaction tx(c.db());
        auto st = c.db().prepare("UPDATE users SET password_hash = ?1 WHERE id = ?2");
        st.bind(1, hash.value()).bind(2, user.id);
        st.run();
        auto del = c.db().prepare("DELETE FROM sessions WHERE user_id = ?1 AND token_hash <> ?2");
        del.bind(1, user.id).bind(2, tokenHash(PlatformService::Context::bearer(req, "Bearer")));
        del.run();
        tx.commit();
        log::info("Account {} changed its password", user.username);
        sendJson(res, 200, {{"status", "changed"}});
    }

    // DELETE /me {password}: personal data goes; the account id stays so the
    // audit log and moderation history still make sense.
    void deleteAccount(const httplib::Request& req, httplib::Response& res) {
        const User user = c.requireUser(req);
        c.limit("delete:" + std::to_string(user.id), 5, 3600);
        const json body = parseBody(req);
        const std::string password = stringField(body, "password", online::kMaxPasswordLength, true, "Your password");
        confirmPassword(user, password);
        if (user.admin()) fail(400, "admin", "Administrators cannot delete their account; remove the role first");
        std::vector<std::int64_t> games;
        std::vector<std::string> packages;
        std::vector<std::string> pictures;
        {
            std::lock_guard lock(c.dbMutex());
            Database::Transaction tx(c.db());
            const auto run = [&](const char* sql, std::int64_t id) {
                auto st = c.db().prepare(sql);
                st.bind(1, id);
                st.run();
            };
            auto owned = c.db().prepare("SELECT id, thumbnail FROM games WHERE owner_id = ?1");
            owned.bind(1, user.id);
            while (owned.step()) {
                games.push_back(owned.integer(0));
                if (!owned.text(1).empty()) pictures.push_back(owned.text(1));
            }
            for (const std::int64_t g : games) {
                auto hashes = c.db().prepare("SELECT DISTINCT package_hash FROM versions WHERE game_id = ?1");
                hashes.bind(1, g);
                while (hashes.step()) packages.push_back(hashes.text(0));
                for (const char* sql : {"DELETE FROM player_data WHERE game_id = ?1",
                                        "DELETE FROM leaderboard_scores WHERE game_id = ?1",
                                        "DELETE FROM invitations WHERE game_id = ?1", "DELETE FROM reports WHERE game_id = ?1",
                                        "DELETE FROM versions WHERE game_id = ?1", "DELETE FROM games WHERE id = ?1"}) {
                    run(sql, g);
                }
            }
            for (const char* sql : {"DELETE FROM sessions WHERE user_id = ?1",
                                    "DELETE FROM account_tokens WHERE user_id = ?1",
                                    "DELETE FROM friends WHERE user_id = ?1 OR friend_id = ?1",
                                    "DELETE FROM friend_requests WHERE from_id = ?1 OR to_id = ?1",
                                    "DELETE FROM blocks WHERE user_id = ?1 OR blocked_id = ?1",
                                    "DELETE FROM invitations WHERE from_id = ?1 OR to_id = ?1",
                                    "DELETE FROM player_data WHERE user_id = ?1",
                                    "DELETE FROM leaderboard_scores WHERE user_id = ?1",
                                    "UPDATE users SET username = 'deleted-' || id, password_hash = '', email = '', "
                                    "email_verified = 0, bio = '', avatar = 0, join_policy = 'friends', deleted = 1 "
                                    "WHERE id = ?1"}) {
                run(sql, user.id);
            }
            c.audit(user, "delete_account", "user " + std::to_string(user.id), std::format("{} games", games.size()));
            tx.commit();
        }
        for (const std::int64_t g : games) c.fleet().stopGame(g);
        // Files nothing refers to any more (not while a backup collects them).
        std::error_code ec;
        std::lock_guard files(c.fileRemovalMutex());
        for (const auto& hash : packages) {
            std::lock_guard lock(c.dbMutex());
            auto used = c.db().prepare("SELECT COUNT(*) FROM versions WHERE package_hash = ?1");
            used.bind(1, hash);
            if (used.step() && used.integer(0) == 0) std::filesystem::remove(c.packagePath(hash), ec);
        }
        for (const auto& hash : pictures) {
            std::lock_guard lock(c.dbMutex());
            auto used = c.db().prepare("SELECT COUNT(*) FROM games WHERE thumbnail = ?1");
            used.bind(1, hash);
            if (used.step() && used.integer(0) == 0) std::filesystem::remove(c.thumbnailPath(hash), ec);
        }
        log::info("Account {} ({}) was deleted with {} games", user.username, user.id, games.size());
        sendJson(res, 200, {{"status", "deleted"}, {"games", games.size()}});
    }

    // GET /me/export: everything the platform keeps about the account, as JSON.
    void exportData(const httplib::Request& req, httplib::Response& res) {
        const User user = c.requireUser(req);
        c.limit("export:" + std::to_string(user.id), 5, 3600);
        std::lock_guard lock(c.dbMutex());
        const auto rows = [&](const char* sql, auto&& each) {
            auto st = c.db().prepare(sql);
            st.bind(1, user.id);
            json out = json::array();
            while (st.step()) out.push_back(each(st));
            return out;
        };
        json account;
        {
            auto st = c.db().prepare(
                "SELECT username, role, created, email, email_verified, bio, avatar, join_policy, last_seen FROM users WHERE id = ?1");
            st.bind(1, user.id);
            if (st.step()) {
                account = {{"id", user.id},        {"username", st.text(0)},     {"role", st.text(1)},
                           {"created", st.integer(2)}, {"email", st.text(3)},   {"emailVerified", st.integer(4) != 0},
                           {"bio", st.text(5)},     {"avatar", st.integer(6)},     {"joinPolicy", st.text(7)},
                           {"lastSeen", st.integer(8)}};
            }
        }
        json out = {
            {"exported", now()},
            {"platform", c.config().name},
            {"account", account},
            {"sessions", rows("SELECT created, expires FROM sessions WHERE user_id = ?1",
                              [](const Statement& s) { return json{{"created", s.integer(0)}, {"expires", s.integer(1)}}; })},
            {"friends", rows("SELECT u.username, f.created FROM friends f JOIN users u ON u.id = f.friend_id WHERE f.user_id = ?1",
                             [](const Statement& s) { return json{{"username", s.text(0)}, {"since", s.integer(1)}}; })},
            {"friendRequestsSent",
             rows("SELECT u.username, r.created FROM friend_requests r JOIN users u ON u.id = r.to_id WHERE r.from_id = ?1",
                  [](const Statement& s) { return json{{"username", s.text(0)}, {"created", s.integer(1)}}; })},
            {"friendRequestsReceived",
             rows("SELECT u.username, r.created FROM friend_requests r JOIN users u ON u.id = r.from_id WHERE r.to_id = ?1",
                  [](const Statement& s) { return json{{"username", s.text(0)}, {"created", s.integer(1)}}; })},
            {"blocked", rows("SELECT u.username, b.created FROM blocks b JOIN users u ON u.id = b.blocked_id WHERE b.user_id = ?1",
                             [](const Statement& s) { return json{{"username", s.text(0)}, {"created", s.integer(1)}}; })},
            {"games", rows("SELECT id, title, description, visibility, created, updated, latest_version, plays FROM games "
                           "WHERE owner_id = ?1",
                           [](const Statement& s) {
                               return json{{"id", s.integer(0)},      {"title", s.text(1)},   {"description", s.text(2)},
                                           {"visibility", s.text(3)}, {"created", s.integer(4)}, {"updated", s.integer(5)},
                                           {"latestVersion", s.integer(6)}, {"plays", s.integer(7)}};
                           })},
            {"savedData", rows("SELECT p.game_id, g.title, p.data, p.updated FROM player_data p JOIN games g ON g.id = p.game_id "
                               "WHERE p.user_id = ?1",
                               [](const Statement& s) {
                                   json data = json::parse(s.text(2), nullptr, false);
                                   return json{{"gameId", s.integer(0)}, {"game", s.text(1)},
                                               {"data", data.is_discarded() ? json(s.text(2)) : data},
                                               {"updated", s.integer(3)}};
                               })},
            {"scores", rows("SELECT l.game_id, g.title, l.board, l.value, l.updated FROM leaderboard_scores l JOIN games g "
                            "ON g.id = l.game_id WHERE l.user_id = ?1",
                            [](const Statement& s) {
                                return json{{"gameId", s.integer(0)}, {"game", s.text(1)}, {"board", s.text(2)},
                                            {"value", s.real(3)},     {"updated", s.integer(4)}};
                            })},
            {"gameReportsFiled", rows("SELECT game_id, reason, state, created FROM reports WHERE reporter_id = ?1",
                                      [](const Statement& s) {
                                          return json{{"gameId", s.integer(0)}, {"reason", s.text(1)},
                                                      {"state", s.text(2)},     {"created", s.integer(3)}};
                                      })},
            {"accountReportsFiled",
             rows("SELECT u.username, r.reason, r.state, r.created FROM user_reports r JOIN users u ON u.id = r.user_id "
                  "WHERE r.reporter_id = ?1",
                  [](const Statement& s) {
                      return json{{"about", s.text(0)}, {"reason", s.text(1)}, {"state", s.text(2)}, {"created", s.integer(3)}};
                  })},
        };
        res.set_header("Content-Disposition", std::format("attachment; filename=\"{}-account.json\"", user.username));
        sendJson(res, 200, out);
    }
};

} // namespace

void registerAccountRoutes(httplib::Server& http, PlatformService::Context& context) {
    auto accounts = std::make_shared<Accounts>(context);
    const auto bind = [&context, accounts](void (Accounts::*fn)(const httplib::Request&, httplib::Response&)) {
        return context.wrap([accounts, fn](const httplib::Request& req, httplib::Response& res) { ((*accounts).*fn)(req, res); });
    };
    http.Get("/api/v1/me/account", bind(&Accounts::account));
    http.Put("/api/v1/me/email", bind(&Accounts::setEmail));
    http.Post("/api/v1/me/email/verification", bind(&Accounts::resendVerification));
    http.Post("/api/v1/email-verifications", bind(&Accounts::verifyEmail));
    http.Post("/api/v1/password-resets", bind(&Accounts::requestReset));
    http.Post("/api/v1/password-resets/complete", bind(&Accounts::completeReset));
    http.Put("/api/v1/me/password", bind(&Accounts::changePassword));
    http.Delete("/api/v1/me", bind(&Accounts::deleteAccount));
    http.Get("/api/v1/me/export", bind(&Accounts::exportData));
}

Result<std::string> PlatformService::passwordResetLink(std::string_view username) {
    auto token = crypto::randomToken(32);
    if (!token) return token.error();
    std::lock_guard lock(m_dbMutex);
    auto st = m_db->prepare("SELECT id FROM users WHERE username = ?1 COLLATE NOCASE AND deleted = 0");
    st.bind(1, username);
    if (!st.step()) return makeError(ErrorCode::NotFound, "There is no account named '{}'", username);
    const std::int64_t id = st.integer(0);
    auto ins = m_db->prepare(
        "INSERT INTO account_tokens (token_hash, user_id, purpose, email, created, expires) VALUES (?1, ?2, 'reset', '', ?3, ?4)");
    ins.bind(1, tokenHash(token.value())).bind(2, id).bind(3, now()).bind(4, now() + kResetSeconds);
    ins.run();
    return std::format("{}/play#reset={}", publicUrl(), token.value());
}

} // namespace px::platform
