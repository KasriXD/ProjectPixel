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
    std::string thumbnail;    // hash of the picture at /games/{id}/thumbnail; empty: none
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
    std::string webSocket;   // "wss://host:port/play" (or ws://): the same server, for browsers
    std::string serverKey;   // the identity the server must prove (net::decodeServerKey; empty: not checked)
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

// --- People (docs/social.md) ------------------------------------------------------------------

inline constexpr std::size_t kMaxBioLength = 200;
inline constexpr int kAvatarCount = 16; // pixel portraits 0-15, the same everywhere
inline constexpr std::size_t kMaxFriends = 200;

// Who may join the game you are in without an invitation.
enum class JoinPolicy { Friends, Nobody };
[[nodiscard]] std::string_view joinPolicyName(JoinPolicy p) noexcept; // "friends", "nobody"
[[nodiscard]] std::optional<JoinPolicy> parseJoinPolicy(std::string_view name) noexcept;

// What friends see of each other. Only friends get presence at all.
struct Presence {
    std::string state = "offline"; // "offline", "online" (seen in the last two minutes), "playing"
    std::int64_t gameId = 0;       // when playing a game the viewer may see
    std::string gameTitle;
    bool joinable = false;         // the viewer may join them now
};

struct Profile {
    UserInfo user;
    std::string bio;
    int avatar = 0;
    JoinPolicy joinPolicy = JoinPolicy::Friends; // only in your own profile
    // "self", "friend", "requested" (you asked them), "incoming" (they asked
    // you), "blocked" (you blocked them), "none".
    std::string relation = "none";
    std::optional<Presence> presence; // friends (and yourself) only
    std::vector<GameInfo> games;      // their public games
    int friends = 0;
};

struct ProfileUpdate {
    std::optional<std::string> bio;
    std::optional<int> avatar;
    std::optional<JoinPolicy> joinPolicy;
};

struct PersonInfo {
    std::string username;
    int avatar = 0;
};

struct FriendInfo {
    std::string username;
    int avatar = 0;
    Presence presence;
    std::int64_t since = 0;
};

struct FriendRequestInfo {
    std::string username;
    int avatar = 0;
    std::int64_t created = 0;
};

// "Come and play with me": a friend in a game asks you to join them there.
struct Invitation {
    std::int64_t id = 0;
    std::string from;
    int fromAvatar = 0;
    std::int64_t gameId = 0;
    std::string gameTitle;
    std::int64_t expires = 0;
};

// Everything a launcher shows about your friends, in one call.
struct SocialSummary {
    std::vector<FriendInfo> friends;
    std::vector<FriendRequestInfo> incoming;
    std::vector<FriendRequestInfo> outgoing;
    std::vector<Invitation> invitations;
};

struct UserReportInfo {
    std::int64_t id = 0;
    std::string username; // the account reported
    std::string reporter;
    std::string reason;
    std::string state = "open";
    std::string resolution;
    std::int64_t created = 0;
};

// --- Leaderboards (docs/leaderboards.md) -------------------------------------------------------

struct LeaderboardEntry {
    int rank = 0; // 1 = best; equal scores share a rank
    std::string username;
    int avatar = 0;
    double value = 0.0;
    std::int64_t updated = 0;
    std::string account; // the account id: only in answers to game servers
};

struct LeaderboardView {
    std::string id;
    std::string title;
    std::string order = "highest"; // "highest" or "lowest" is best
    std::string format = "number"; // "number" or "time" (seconds)
    std::vector<LeaderboardEntry> top;
    std::optional<LeaderboardEntry> you;  // with a login
    std::vector<LeaderboardEntry> friends; // with a login: you and your friends, best first
};

struct PlatformInfo {
    std::string name;
    std::string engineVersion;
    int apiVersion = kPlatformApiVersion;
    std::string publicKey; // hex: verifies connect tokens
    bool webPlayer = false; // games can be played in a browser at /play
};

} // namespace px::online
