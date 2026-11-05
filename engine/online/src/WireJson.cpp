#include "px/online/WireJson.hpp"

#include <cctype>

namespace px::online {

bool isValidUsername(std::string_view name) {
    if (name.size() < kMinUsernameLength || name.size() > kMaxUsernameLength) return false;
    for (const char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_')) return false;
    }
    return true;
}

std::string_view roleName(Role role) noexcept { return role == Role::Admin ? "admin" : "user"; }

std::string_view visibilityName(Visibility v) noexcept {
    switch (v) {
        case Visibility::Public: return "public";
        case Visibility::Unlisted: return "unlisted";
        case Visibility::Private: return "private";
    }
    return "public";
}

std::optional<Visibility> parseVisibility(std::string_view name) noexcept {
    for (const Visibility v : {Visibility::Public, Visibility::Unlisted, Visibility::Private}) {
        if (visibilityName(v) == name) return v;
    }
    return std::nullopt;
}

void to_json(nlohmann::json& j, const UserInfo& v) {
    j = {{"id", v.id}, {"username", v.username}, {"role", roleName(v.role)}, {"created", v.created}};
    if (v.banned) j["banned"] = true;
}
void from_json(const nlohmann::json& j, UserInfo& v) {
    v.id = j.at("id").get<std::int64_t>();
    v.username = j.at("username").get<std::string>();
    v.role = j.at("role").get<std::string>() == "admin" ? Role::Admin : Role::User;
    v.created = j.value("created", std::int64_t{0});
    v.banned = j.value("banned", false);
}

void to_json(nlohmann::json& j, const VersionInfo& v) {
    j = {{"number", v.number},   {"packageHash", v.packageHash},     {"bytes", v.bytes},
         {"notes", v.notes},     {"engineVersion", v.engineVersion}, {"scriptErrors", v.scriptErrors},
         {"created", v.created}};
}
void from_json(const nlohmann::json& j, VersionInfo& v) {
    v.number = j.at("number").get<int>();
    v.packageHash = j.at("packageHash").get<std::string>();
    v.bytes = j.at("bytes").get<std::uint64_t>();
    v.notes = j.value("notes", std::string());
    v.engineVersion = j.value("engineVersion", std::string());
    v.scriptErrors = j.value("scriptErrors", 0);
    v.created = j.value("created", std::int64_t{0});
}

void to_json(nlohmann::json& j, const GameInfo& v) {
    j = {{"id", v.id},
         {"title", v.title},
         {"description", v.description},
         {"owner", {{"id", v.ownerId}, {"username", v.ownerName}}},
         {"visibility", visibilityName(v.visibility)},
         {"gameType", v.gameType},
         {"maxPlayers", v.maxPlayers},
         {"latestVersion", v.latestVersion},
         {"packageHash", v.packageHash},
         {"plays", v.plays},
         {"playing", v.playing},
         {"created", v.created},
         {"updated", v.updated}};
    if (v.hidden) j["hidden"] = true;
    if (!v.moderationNote.empty()) j["moderationNote"] = v.moderationNote;
    if (!v.thumbnail.empty()) j["thumbnail"] = v.thumbnail;
}
void from_json(const nlohmann::json& j, GameInfo& v) {
    v.id = j.at("id").get<std::int64_t>();
    v.title = j.at("title").get<std::string>();
    v.description = j.value("description", std::string());
    const auto& owner = j.at("owner");
    v.ownerId = owner.at("id").get<std::int64_t>();
    v.ownerName = owner.at("username").get<std::string>();
    v.visibility = parseVisibility(j.at("visibility").get<std::string>()).value_or(Visibility::Private);
    v.gameType = j.value("gameType", std::string());
    v.maxPlayers = j.value("maxPlayers", 8);
    v.latestVersion = j.value("latestVersion", 0);
    v.packageHash = j.value("packageHash", std::string());
    v.plays = j.value("plays", std::int64_t{0});
    v.playing = j.value("playing", 0);
    v.created = j.value("created", std::int64_t{0});
    v.updated = j.value("updated", std::int64_t{0});
    v.hidden = j.value("hidden", false);
    v.moderationNote = j.value("moderationNote", std::string());
    v.thumbnail = j.value("thumbnail", std::string());
}

void to_json(nlohmann::json& j, const JoinTicket& v) {
    j = {{"token", v.token},     {"host", v.host},       {"port", v.port},
         {"session", v.session}, {"expires", v.expires}, {"version", v.version},
         {"packageHash", v.packageHash}, {"webSocket", v.webSocket}};
}
void from_json(const nlohmann::json& j, JoinTicket& v) {
    v.token = j.at("token").get<std::string>();
    v.host = j.at("host").get<std::string>();
    v.port = j.at("port").get<std::uint16_t>();
    v.session = j.at("session").get<std::string>();
    v.expires = j.at("expires").get<std::int64_t>();
    v.version = j.at("version").get<int>();
    v.packageHash = j.at("packageHash").get<std::string>();
    v.webSocket = j.value("webSocket", std::string());
}

void to_json(nlohmann::json& j, const ReportInfo& v) {
    j = {{"id", v.id},         {"gameId", v.gameId}, {"gameTitle", v.gameTitle},   {"reporter", v.reporter},
         {"reason", v.reason}, {"state", v.state},   {"resolution", v.resolution}, {"created", v.created}};
}
void from_json(const nlohmann::json& j, ReportInfo& v) {
    v.id = j.at("id").get<std::int64_t>();
    v.gameId = j.at("gameId").get<std::int64_t>();
    v.gameTitle = j.value("gameTitle", std::string());
    v.reporter = j.value("reporter", std::string());
    v.reason = j.value("reason", std::string());
    v.state = j.value("state", std::string("open"));
    v.resolution = j.value("resolution", std::string());
    v.created = j.value("created", std::int64_t{0});
}

void to_json(nlohmann::json& j, const AuditEntry& v) {
    j = {{"id", v.id},         {"at", v.at},         {"actor", v.actor},
         {"action", v.action}, {"target", v.target}, {"detail", v.detail}};
}
void from_json(const nlohmann::json& j, AuditEntry& v) {
    v.id = j.at("id").get<std::int64_t>();
    v.at = j.at("at").get<std::int64_t>();
    v.actor = j.value("actor", std::string());
    v.action = j.value("action", std::string());
    v.target = j.value("target", std::string());
    v.detail = j.value("detail", std::string());
}

void to_json(nlohmann::json& j, const PlatformInfo& v) {
    j = {{"name", v.name},
         {"engineVersion", v.engineVersion},
         {"apiVersion", v.apiVersion},
         {"publicKey", v.publicKey},
         {"webPlayer", v.webPlayer}};
}
void from_json(const nlohmann::json& j, PlatformInfo& v) {
    v.name = j.at("name").get<std::string>();
    v.engineVersion = j.at("engineVersion").get<std::string>();
    v.apiVersion = j.at("apiVersion").get<int>();
    v.publicKey = j.at("publicKey").get<std::string>();
    v.webPlayer = j.value("webPlayer", false);
}

std::string_view joinPolicyName(JoinPolicy p) noexcept { return p == JoinPolicy::Nobody ? "nobody" : "friends"; }
std::optional<JoinPolicy> parseJoinPolicy(std::string_view name) noexcept {
    if (name == "friends") return JoinPolicy::Friends;
    if (name == "nobody") return JoinPolicy::Nobody;
    return std::nullopt;
}

void to_json(nlohmann::json& j, const Presence& v) {
    j = {{"state", v.state}};
    if (v.gameId != 0) {
        j["gameId"] = v.gameId;
        j["gameTitle"] = v.gameTitle;
    }
    if (v.joinable) j["joinable"] = true;
}
void from_json(const nlohmann::json& j, Presence& v) {
    v.state = j.value("state", std::string("offline"));
    v.gameId = j.value("gameId", std::int64_t{0});
    v.gameTitle = j.value("gameTitle", std::string());
    v.joinable = j.value("joinable", false);
}

void to_json(nlohmann::json& j, const Profile& v) {
    j = {{"user", v.user}, {"bio", v.bio},         {"avatar", v.avatar},
         {"relation", v.relation}, {"games", v.games}, {"friends", v.friends}};
    if (v.relation == "self") j["joinPolicy"] = joinPolicyName(v.joinPolicy);
    if (v.presence) j["presence"] = *v.presence;
}
void from_json(const nlohmann::json& j, Profile& v) {
    v.user = j.at("user").get<UserInfo>();
    v.bio = j.value("bio", std::string());
    v.avatar = j.value("avatar", 0);
    v.relation = j.value("relation", std::string("none"));
    v.joinPolicy = parseJoinPolicy(j.value("joinPolicy", std::string("friends"))).value_or(JoinPolicy::Friends);
    if (j.contains("presence")) v.presence = j.at("presence").get<Presence>();
    v.games = j.value("games", std::vector<GameInfo>{});
    v.friends = j.value("friends", 0);
}

void to_json(nlohmann::json& j, const PersonInfo& v) { j = {{"username", v.username}, {"avatar", v.avatar}}; }
void from_json(const nlohmann::json& j, PersonInfo& v) {
    v.username = j.at("username").get<std::string>();
    v.avatar = j.value("avatar", 0);
}

void to_json(nlohmann::json& j, const FriendInfo& v) {
    j = {{"username", v.username}, {"avatar", v.avatar}, {"presence", v.presence}, {"since", v.since}};
}
void from_json(const nlohmann::json& j, FriendInfo& v) {
    v.username = j.at("username").get<std::string>();
    v.avatar = j.value("avatar", 0);
    v.presence = j.value("presence", Presence{});
    v.since = j.value("since", std::int64_t{0});
}

void to_json(nlohmann::json& j, const FriendRequestInfo& v) {
    j = {{"username", v.username}, {"avatar", v.avatar}, {"created", v.created}};
}
void from_json(const nlohmann::json& j, FriendRequestInfo& v) {
    v.username = j.at("username").get<std::string>();
    v.avatar = j.value("avatar", 0);
    v.created = j.value("created", std::int64_t{0});
}

void to_json(nlohmann::json& j, const Invitation& v) {
    j = {{"id", v.id},         {"from", v.from},           {"fromAvatar", v.fromAvatar},
         {"gameId", v.gameId}, {"gameTitle", v.gameTitle}, {"expires", v.expires}};
}
void from_json(const nlohmann::json& j, Invitation& v) {
    v.id = j.at("id").get<std::int64_t>();
    v.from = j.at("from").get<std::string>();
    v.fromAvatar = j.value("fromAvatar", 0);
    v.gameId = j.value("gameId", std::int64_t{0});
    v.gameTitle = j.value("gameTitle", std::string());
    v.expires = j.value("expires", std::int64_t{0});
}

void to_json(nlohmann::json& j, const SocialSummary& v) {
    j = {{"friends", v.friends}, {"incoming", v.incoming}, {"outgoing", v.outgoing}, {"invitations", v.invitations}};
}
void from_json(const nlohmann::json& j, SocialSummary& v) {
    v.friends = j.value("friends", std::vector<FriendInfo>{});
    v.incoming = j.value("incoming", std::vector<FriendRequestInfo>{});
    v.outgoing = j.value("outgoing", std::vector<FriendRequestInfo>{});
    v.invitations = j.value("invitations", std::vector<Invitation>{});
}

void to_json(nlohmann::json& j, const UserReportInfo& v) {
    j = {{"id", v.id},         {"username", v.username}, {"reporter", v.reporter},     {"reason", v.reason},
         {"state", v.state},   {"resolution", v.resolution}, {"created", v.created}};
}
void from_json(const nlohmann::json& j, UserReportInfo& v) {
    v.id = j.at("id").get<std::int64_t>();
    v.username = j.value("username", std::string());
    v.reporter = j.value("reporter", std::string());
    v.reason = j.value("reason", std::string());
    v.state = j.value("state", std::string("open"));
    v.resolution = j.value("resolution", std::string());
    v.created = j.value("created", std::int64_t{0});
}

void to_json(nlohmann::json& j, const LeaderboardEntry& v) {
    j = {{"rank", v.rank}, {"username", v.username}, {"avatar", v.avatar}, {"value", v.value}, {"updated", v.updated}};
    if (!v.account.empty()) j["account"] = v.account;
}
void from_json(const nlohmann::json& j, LeaderboardEntry& v) {
    v.rank = j.value("rank", 0);
    v.username = j.at("username").get<std::string>();
    v.avatar = j.value("avatar", 0);
    v.value = j.at("value").get<double>();
    v.updated = j.value("updated", std::int64_t{0});
    v.account = j.value("account", std::string());
}

void to_json(nlohmann::json& j, const LeaderboardView& v) {
    j = {{"id", v.id},       {"title", v.title}, {"order", v.order},
         {"format", v.format}, {"top", v.top},   {"friends", v.friends}};
    if (v.you) j["you"] = *v.you;
}
void from_json(const nlohmann::json& j, LeaderboardView& v) {
    v.id = j.at("id").get<std::string>();
    v.title = j.value("title", v.id);
    v.order = j.value("order", std::string("highest"));
    v.format = j.value("format", std::string("number"));
    v.top = j.value("top", std::vector<LeaderboardEntry>{});
    v.friends = j.value("friends", std::vector<LeaderboardEntry>{});
    if (j.contains("you")) v.you = j.at("you").get<LeaderboardEntry>();
}

} // namespace px::online
