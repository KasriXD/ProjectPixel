#pragma once

#include "px/core/Result.hpp"
#include "px/online/PlatformTypes.hpp"
#include "px/runtime/PlayerData.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// The platform's HTTP API from a program's side (docs/platform.md#http-api).
// Every call blocks until the platform answers; call from a worker thread in
// interactive programs. Errors carry the platform's message, written for
// the person using the program.
namespace px::online {

class PlatformClient {
public:
    // `baseUrl`: "https://host[:port]" or "http://host:port" (docs/platform.md#https).
    // HTTPS checks the certificate against the system's trusted ones, plus the
    // file in PX_PLATFORM_CA if set (a self-signed development certificate).
    explicit PlatformClient(std::string baseUrl);
    // Also trust the certificates in this PEM file (empty: only the system's).
    void setCaFile(std::string file) { m_caFile = std::move(file); }

    [[nodiscard]] const std::string& baseUrl() const { return m_baseUrl; }
    // A player's session token ("Bearer"), from register or login.
    void setToken(std::string token) { m_token = std::move(token); }
    [[nodiscard]] const std::string& token() const { return m_token; }
    // A game server's key ("Server"), given to it when the platform starts it.
    void setServerKey(std::string key) { m_serverKey = std::move(key); }
    void setTimeoutSeconds(int seconds) { m_timeoutSeconds = seconds; }

    [[nodiscard]] Result<PlatformInfo> info();

    // --- Accounts ------------------------------------------------------------------------
    [[nodiscard]] Result<AuthResult> registerAccount(const std::string& username, const std::string& password);
    [[nodiscard]] Result<AuthResult> login(const std::string& username, const std::string& password);
    [[nodiscard]] Status logout();
    [[nodiscard]] Result<UserInfo> me();

    // --- Games -----------------------------------------------------------------------------
    [[nodiscard]] Result<std::vector<GameInfo>> games(const GameQuery& query = {});
    [[nodiscard]] Result<std::vector<GameInfo>> myGames();
    [[nodiscard]] Result<GameInfo> game(std::int64_t id);
    [[nodiscard]] Result<GameInfo> createGame(const std::string& title, const std::string& description,
                                              Visibility visibility);
    [[nodiscard]] Result<GameInfo> updateGame(std::int64_t id, const GameUpdate& update);
    // Uploads a package (project::encodePackage) as the game's next version.
    // The platform checks it before accepting it; that can take a few seconds.
    [[nodiscard]] Result<VersionInfo> publish(std::int64_t gameId, const std::vector<std::uint8_t>& package,
                                              const std::string& notes = {});
    [[nodiscard]] Result<std::vector<VersionInfo>> versions(std::int64_t gameId);
    // Downloads a package and checks it has the hash it was asked for.
    [[nodiscard]] Result<std::vector<std::uint8_t>> downloadPackage(const std::string& hash);
    // A ticket to a game server running the game's latest version.
    [[nodiscard]] Result<JoinTicket> join(std::int64_t gameId);
    [[nodiscard]] Status report(std::int64_t gameId, const std::string& reason);
    // A player's saved data in one of your games (owners and admins), as JSON.
    [[nodiscard]] Result<std::string> playerData(std::int64_t gameId, const std::string& username);

    // --- Your account (docs/accounts.md) ---------------------------------------------------
    [[nodiscard]] Result<AccountSettings> account();
    // Sets the email address (empty removes it); the platform mails a link to confirm it.
    [[nodiscard]] Status setEmail(const std::string& email, const std::string& password);
    [[nodiscard]] Status resendVerification();
    // The token from a confirmation link (".../play#verify=TOKEN"); returns the account name.
    [[nodiscard]] Result<std::string> verifyEmail(const std::string& token);
    // A name or an email address; the answer does not say whether one matched.
    [[nodiscard]] Status requestPasswordReset(const std::string& nameOrEmail);
    // The token from a reset link (".../play#reset=TOKEN"); returns the account
    // name. Every session of the account ends.
    [[nodiscard]] Result<std::string> completePasswordReset(const std::string& token, const std::string& password);
    // Your other sessions end; this one stays.
    [[nodiscard]] Status changePassword(const std::string& current, const std::string& password);
    // Deletes the account and its games for good; logs out.
    [[nodiscard]] Status deleteAccount(const std::string& password);
    // Everything the platform keeps about you, as JSON.
    [[nodiscard]] Result<std::string> exportData();

    // --- People (docs/social.md) -----------------------------------------------------------
    [[nodiscard]] Result<Profile> profile(const std::string& username);
    [[nodiscard]] Result<Profile> updateProfile(const ProfileUpdate& update);
    // Accounts whose names start with `prefix` (two letters or more).
    [[nodiscard]] Result<std::vector<PersonInfo>> searchUsers(const std::string& prefix);
    // Friends with presence, friend requests both ways, invitations to you.
    [[nodiscard]] Result<SocialSummary> social();
    // Sends a friend request, or accepts theirs if they asked first.
    // Returns "requested" or "friends".
    [[nodiscard]] Result<std::string> requestFriend(const std::string& username);
    [[nodiscard]] Status acceptFriend(const std::string& username);
    // Declines their request or takes back yours.
    [[nodiscard]] Status removeFriendRequest(const std::string& username);
    [[nodiscard]] Status removeFriend(const std::string& username);
    // A ticket to the game server a friend is playing on.
    [[nodiscard]] Result<JoinTicket> joinFriend(const std::string& username);
    // Invites a friend to the game server you are playing on.
    [[nodiscard]] Status invite(const std::string& username);
    [[nodiscard]] Status dismissInvitation(std::int64_t id);
    [[nodiscard]] Result<std::vector<PersonInfo>> blocked();
    [[nodiscard]] Status block(const std::string& username);
    [[nodiscard]] Status unblock(const std::string& username);
    [[nodiscard]] Status reportUser(const std::string& username, const std::string& reason);

    // --- Leaderboards (docs/leaderboards.md) ----------------------------------------------
    // The game's leaderboards: the top `limit` of each, and you and your friends with a login.
    [[nodiscard]] Result<std::vector<LeaderboardView>> leaderboards(std::int64_t gameId, int limit = 10);
    // Owners: remove one player's score, or every score (username empty).
    [[nodiscard]] Status clearLeaderboard(std::int64_t gameId, const std::string& board, const std::string& username = {});

    // --- Thumbnails -------------------------------------------------------------------------
    // Owners: a PNG or JPEG picture of the game; the platform checks it and
    // stores it as a 320x180 PNG.
    [[nodiscard]] Status uploadThumbnail(std::int64_t gameId, const std::vector<std::uint8_t>& image);
    // The PNG (empty when the game has none).
    [[nodiscard]] Result<std::vector<std::uint8_t>> thumbnail(std::int64_t gameId);

    // --- Moderation (admins) ----------------------------------------------------------------
    [[nodiscard]] Result<std::vector<ReportInfo>> reports(bool openOnly = true);
    // action: "dismiss", "hide_game" or "ban_owner".
    [[nodiscard]] Status resolveReport(std::int64_t reportId, const std::string& action, const std::string& note);
    [[nodiscard]] Status setGameHidden(std::int64_t gameId, bool hidden, const std::string& note);
    [[nodiscard]] Status setUserBanned(const std::string& username, bool banned, const std::string& note);
    [[nodiscard]] Result<std::vector<AuditEntry>> audit(int limit = 100);
    [[nodiscard]] Result<std::vector<UserReportInfo>> userReports(bool openOnly = true);
    // action: "dismiss", "clear_profile" or "ban".
    [[nodiscard]] Status resolveUserReport(std::int64_t reportId, const std::string& action, const std::string& note);

    // --- Game servers (server key) ---------------------------------------------------------
    [[nodiscard]] Status serverReady(std::uint16_t port);
    // Reports who is playing; true means the platform wants the server to stop.
    [[nodiscard]] Result<bool> serverHeartbeat(const std::vector<std::string>& userIds);
    [[nodiscard]] Result<runtime::PlayerData> serverLoadData(const std::string& userId);
    // `release`: the player left; the platform may let them join elsewhere.
    [[nodiscard]] Status serverSaveData(const std::string& userId, const runtime::PlayerData& data, bool release);
    // The player left before their data was loaded.
    [[nodiscard]] Status serverRelease(const std::string& userId);
    [[nodiscard]] Status serverEnded();
    struct ScoreSubmission {
        std::string userId;
        std::string board;
        double value = 0.0;
    };
    // Players' scores; the platform keeps each player's best.
    [[nodiscard]] Status serverSubmitScores(const std::vector<ScoreSubmission>& scores);
    struct ServerLeaderboards {
        std::vector<LeaderboardView> boards; // top entries carry their account ids
        // account id -> board id -> that player's best and rank
        std::map<std::string, std::map<std::string, LeaderboardEntry>> players;
    };
    // The game's leaderboards (top `limit` each) and the bests of these players.
    [[nodiscard]] Result<ServerLeaderboards> serverLeaderboards(int limit, const std::vector<std::string>& userIds);

private:
    struct Response {
        int status = 0;
        std::string body;
    };
    enum class Auth { None, User, Server };
    [[nodiscard]] Result<Response> request(const char* method, const std::string& path, const std::string& body,
                                           const char* contentType, Auth auth);
    [[nodiscard]] Result<std::string> call(const char* method, const std::string& path, const std::string& body = {},
                                           Auth auth = Auth::User, const char* contentType = "application/json");

    std::string m_baseUrl;
    std::string m_token;
    std::string m_serverKey;
    std::string m_caFile;
    int m_timeoutSeconds = 30;
};

// Saved player data as JSON ({"coins": 3, "spawn": {"x": 1, "y": 2}}).
[[nodiscard]] std::string playerDataToJson(const runtime::PlayerData& data);
[[nodiscard]] Result<runtime::PlayerData> playerDataFromJson(std::string_view json);

// Downloaded packages, kept by hash so a game is downloaded once per version.
class PackageCache {
public:
    explicit PackageCache(std::filesystem::path dir) : m_dir(std::move(dir)) {}
    // The package's bytes, from the cache or downloaded through `client`.
    [[nodiscard]] Result<std::vector<std::uint8_t>> get(PlatformClient& client, const std::string& hash);
    [[nodiscard]] std::filesystem::path pathFor(const std::string& hash) const;

private:
    std::filesystem::path m_dir;
};

// Where programs keep platform state for the person using them (login
// token, package cache): <user config dir>/ProjectPixel/platform.
[[nodiscard]] std::filesystem::path userPlatformDir();

// The login remembered on this computer for a platform, shared by Studio
// and the player (file mode 0600 where the OS has modes).
struct SavedLogin {
    std::string platformUrl;
    std::string username;
    std::string token;
};
[[nodiscard]] std::optional<SavedLogin> loadSavedLogin(const std::filesystem::path& dir);
[[nodiscard]] Status saveLogin(const std::filesystem::path& dir, const SavedLogin& login);
void forgetLogin(const std::filesystem::path& dir);

} // namespace px::online
