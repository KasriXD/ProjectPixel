#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What the platform's HTTP API exchanges (docs/platform.md#http-api).
// Times are Unix seconds (UTC). Ids are the platform's own numbers.
namespace px::online {

inline constexpr int kPlatformApiVersion = 1;
inline constexpr std::uint16_t kDefaultPlatformPort = 8730;

// Account names: 3-20 letters, digits or '_', unique ignoring case.
inline constexpr std::size_t kMinUsernameLength = 3;
inline constexpr std::size_t kMaxUsernameLength = 20;
inline constexpr std::size_t kMinPasswordLength = 8;
inline constexpr std::size_t kMaxPasswordLength = 256;
inline constexpr std::size_t kMaxGameTitleLength = 60;
inline constexpr std::size_t kMaxGameDescriptionLength = 1000;
inline constexpr std::size_t kMaxReportLength = 1000;
[[nodiscard]] bool isValidUsername(std::string_view name);

enum class Role { User, Admin };
[[nodiscard]] std::string_view roleName(Role role) noexcept; // "user", "admin"

struct UserInfo {
    std::int64_t id = 0;
    std::string username;
    Role role = Role::User;
    std::int64_t created = 0;
    bool banned = false; // shown to admins only
};

struct AuthResult {
    UserInfo user;
    std::string token; // send as "Authorization: Bearer <token>"
};

enum class Visibility {
    Public,   // listed in the catalog
    Unlisted, // playable by anyone who has the id, not listed
    Private,  // only the owner can play it
};
[[nodiscard]] std::string_view visibilityName(Visibility v) noexcept; // "public", "unlisted", "private"
[[nodiscard]] std::optional<Visibility> parseVisibility(std::string_view name) noexcept;

struct VersionInfo {
    int number = 0;          // 1, 2, 3, ... per game
    std::string packageHash; // BLAKE2b-256 of the checked package, hex: also its download name
    std::uint64_t bytes = 0;
    std::string notes;
    std::string engineVersion; // the engine that checked it
    int scriptErrors = 0;      // runtime errors seen while the platform tried it
    std::int64_t created = 0;
};

struct GameInfo {
    std::int64_t id = 0;
    std::string title;
    std::string description;
    std::int64_t ownerId = 0;
    std::string ownerName;
    Visibility visibility = Visibility::Public;
    std::string gameType;     // from the latest version ("platformer", "topdown")
    int maxPlayers = 8;       // per game server
    int latestVersion = 0;    // 0: nothing published yet
    std::string packageHash;  // of the latest version
    std::int64_t plays = 0;   // joins, all time
    int playing = 0;          // players in game servers now
    std::int64_t created = 0;
    std::int64_t updated = 0;
    bool hidden = false;      // by a moderator (owner and admins see it)
    std::string moderationNote;
};

struct GameQuery {
    std::string search;          // words in the title or description
    std::string sort = "popular"; // "popular" (plays), "recent" (updated), "playing"
    int limit = 50;
    int offset = 0;
};

struct GameUpdate {
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<Visibility> visibility;
    std::optional<int> maxPlayers;
};

// Permission to join one game server: the token is signed by the platform
// and checked by the server (net/ConnectToken.hpp).
struct JoinTicket {
    std::string token;
    std::string host;
    std::uint16_t port = 0;
    std::string session;
    std::int64_t expires = 0;
    int version = 0;
    std::string packageHash; // the game version the server runs
};

struct ReportInfo {
    std::int64_t id = 0;
    std::int64_t gameId = 0;
    std::string gameTitle;
    std::string reporter;
    std::string reason;
    std::string state = "open"; // "open", "resolved"
    std::string resolution;     // what the moderator did
    std::int64_t created = 0;
};

struct AuditEntry {
    std::int64_t id = 0;
    std::int64_t at = 0;
    std::string actor;  // username
    std::string action; // "hide_game", "ban_user", "resolve_report", ...
    std::string target;
    std::string detail;
};

struct PlatformInfo {
    std::string name;
    std::string engineVersion;
    int apiVersion = kPlatformApiVersion;
    std::string publicKey; // hex: verifies connect tokens
};

} // namespace px::online
