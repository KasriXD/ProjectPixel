#pragma once

#include "px/core/Result.hpp"
#include "px/online/PlatformTypes.hpp"
#include "px/runtime/PlayerData.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The platform's HTTP API from a program's side (docs/platform.md#http-api).
// Every call blocks until the platform answers; call from a worker thread in
// interactive programs. Errors carry the platform's message, written for
// the person using the program.
namespace px::online {

class PlatformClient {
public:
    // `baseUrl`: "http://host:port" (plain HTTP; see docs/platform.md#security).
    explicit PlatformClient(std::string baseUrl);

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

    // --- Moderation (admins) ----------------------------------------------------------------
    [[nodiscard]] Result<std::vector<ReportInfo>> reports(bool openOnly = true);
    // action: "dismiss", "hide_game" or "ban_owner".
    [[nodiscard]] Status resolveReport(std::int64_t reportId, const std::string& action, const std::string& note);
    [[nodiscard]] Status setGameHidden(std::int64_t gameId, bool hidden, const std::string& note);
    [[nodiscard]] Status setUserBanned(const std::string& username, bool banned, const std::string& note);
    [[nodiscard]] Result<std::vector<AuditEntry>> audit(int limit = 100);

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
