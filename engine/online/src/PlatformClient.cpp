#include "px/online/PlatformClient.hpp"

#include "px/crypto/Crypto.hpp"
#include "px/crypto/Encoding.hpp"
#include "px/online/WireJson.hpp"

#include <httplib.h>

#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>

namespace px::online {

using nlohmann::json;

namespace {
std::string encodeQuery(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            out += std::format("%{:02X}", u);
        }
    }
    return out;
}

ErrorCode codeForStatus(int status) {
    switch (status) {
        case 400: return ErrorCode::InvalidArgument;
        case 401:
        case 403: return ErrorCode::AccessDenied;
        case 404: return ErrorCode::NotFound;
        case 409: return ErrorCode::InvalidArgument;
        case 413:
        case 429: return ErrorCode::LimitExceeded;
        case 422: return ErrorCode::InvalidData;
        default: return ErrorCode::Unknown;
    }
}

template <class T>
Result<T> parseAs(const std::string& body, const char* key = nullptr) {
    try {
        const json j = json::parse(body);
        return key != nullptr ? j.at(key).get<T>() : j.get<T>();
    } catch (const std::exception& e) {
        return makeError(ErrorCode::InvalidData, "The platform sent an answer this program does not understand ({})",
                         e.what());
    }
}
} // namespace

PlatformClient::PlatformClient(std::string baseUrl) : m_baseUrl(std::move(baseUrl)) {
    while (m_baseUrl.ends_with('/')) m_baseUrl.pop_back();
}

Result<PlatformClient::Response> PlatformClient::request(const char* method, const std::string& path,
                                                         const std::string& body, const char* contentType, Auth auth) {
    if (!m_baseUrl.starts_with("http://")) {
        return makeError(ErrorCode::Unsupported, "'{}' is not an http:// address (this build has no HTTPS)", m_baseUrl);
    }
    httplib::Client client(m_baseUrl);
    client.set_connection_timeout(5);
    client.set_read_timeout(m_timeoutSeconds);
    client.set_write_timeout(m_timeoutSeconds);
    httplib::Headers headers;
    if (auth == Auth::User && !m_token.empty()) headers.emplace("Authorization", "Bearer " + m_token);
    if (auth == Auth::Server && !m_serverKey.empty()) headers.emplace("Authorization", "Server " + m_serverKey);
    const std::string full = "/api/v1" + path;
    httplib::Result res;
    const std::string m(method);
    if (m == "GET") {
        res = client.Get(full, headers);
    } else if (m == "POST") {
        res = client.Post(full, headers, body, contentType);
    } else if (m == "PUT") {
        res = client.Put(full, headers, body, contentType);
    } else if (m == "PATCH") {
        res = client.Patch(full, headers, body, contentType);
    } else {
        res = client.Delete(full, headers, body, contentType);
    }
    if (!res) {
        return makeError(ErrorCode::IoError, "Cannot reach the platform at {} ({})", m_baseUrl,
                         httplib::to_string(res.error()));
    }
    return Response{res->status, std::move(res->body)};
}

Result<std::string> PlatformClient::call(const char* method, const std::string& path, const std::string& body,
                                         Auth auth, const char* contentType) {
    auto response = request(method, path, body, contentType, auth);
    if (!response) return response.error();
    if (response->status >= 200 && response->status < 300) return std::move(response->body);
    std::string message = std::format("The platform answered {}", response->status);
    try {
        const json j = json::parse(response->body);
        message = j.at("error").at("message").get<std::string>();
    } catch (const std::exception&) {
    }
    return Error{codeForStatus(response->status), std::move(message)};
}

Result<PlatformInfo> PlatformClient::info() {
    auto body = call("GET", "/platform", {}, Auth::None);
    if (!body) return body.error();
    return parseAs<PlatformInfo>(body.value());
}

Result<AuthResult> PlatformClient::registerAccount(const std::string& username, const std::string& password) {
    auto body = call("POST", "/accounts", json{{"username", username}, {"password", password}}.dump(), Auth::None);
    if (!body) return body.error();
    auto user = parseAs<UserInfo>(body.value(), "user");
    auto token = parseAs<std::string>(body.value(), "token");
    if (!user) return user.error();
    if (!token) return token.error();
    m_token = token.value();
    return AuthResult{std::move(user.value()), std::move(token.value())};
}

Result<AuthResult> PlatformClient::login(const std::string& username, const std::string& password) {
    auto body = call("POST", "/sessions", json{{"username", username}, {"password", password}}.dump(), Auth::None);
    if (!body) return body.error();
    auto user = parseAs<UserInfo>(body.value(), "user");
    auto token = parseAs<std::string>(body.value(), "token");
    if (!user) return user.error();
    if (!token) return token.error();
    m_token = token.value();
    return AuthResult{std::move(user.value()), std::move(token.value())};
}

Status PlatformClient::logout() {
    auto body = call("DELETE", "/sessions/current");
    m_token.clear();
    if (!body) return body.error();
    return {};
}

Result<UserInfo> PlatformClient::me() {
    auto body = call("GET", "/me");
    if (!body) return body.error();
    return parseAs<UserInfo>(body.value(), "user");
}

Result<std::vector<GameInfo>> PlatformClient::games(const GameQuery& q) {
    auto body = call("GET", std::format("/games?search={}&sort={}&limit={}&offset={}", encodeQuery(q.search),
                                        encodeQuery(q.sort), q.limit, q.offset),
                     {}, m_token.empty() ? Auth::None : Auth::User);
    if (!body) return body.error();
    return parseAs<std::vector<GameInfo>>(body.value(), "games");
}

Result<std::vector<GameInfo>> PlatformClient::myGames() {
    auto body = call("GET", "/games/mine");
    if (!body) return body.error();
    return parseAs<std::vector<GameInfo>>(body.value(), "games");
}

Result<GameInfo> PlatformClient::game(std::int64_t id) {
    auto body = call("GET", std::format("/games/{}", id), {}, m_token.empty() ? Auth::None : Auth::User);
    if (!body) return body.error();
    return parseAs<GameInfo>(body.value(), "game");
}

Result<GameInfo> PlatformClient::createGame(const std::string& title, const std::string& description,
                                            Visibility visibility) {
    auto body = call("POST", "/games",
                     json{{"title", title}, {"description", description}, {"visibility", visibilityName(visibility)}}
                         .dump());
    if (!body) return body.error();
    return parseAs<GameInfo>(body.value(), "game");
}

Result<GameInfo> PlatformClient::updateGame(std::int64_t id, const GameUpdate& update) {
    json j = json::object();
    if (update.title) j["title"] = *update.title;
    if (update.description) j["description"] = *update.description;
    if (update.visibility) j["visibility"] = visibilityName(*update.visibility);
    if (update.maxPlayers) j["maxPlayers"] = *update.maxPlayers;
    auto body = call("PATCH", std::format("/games/{}", id), j.dump());
    if (!body) return body.error();
    return parseAs<GameInfo>(body.value(), "game");
}

Result<VersionInfo> PlatformClient::publish(std::int64_t gameId, const std::vector<std::uint8_t>& package,
                                            const std::string& notes) {
    const std::string bytes(package.begin(), package.end());
    const int saved = m_timeoutSeconds;
    m_timeoutSeconds = std::max(m_timeoutSeconds, 180); // the platform checks the game first
    auto body = call("POST", std::format("/games/{}/versions?notes={}", gameId, encodeQuery(notes)), bytes,
                     Auth::User, "application/octet-stream");
    m_timeoutSeconds = saved;
    if (!body) return body.error();
    return parseAs<VersionInfo>(body.value(), "version");
}

Result<std::vector<VersionInfo>> PlatformClient::versions(std::int64_t gameId) {
    auto body = call("GET", std::format("/games/{}/versions", gameId));
    if (!body) return body.error();
    return parseAs<std::vector<VersionInfo>>(body.value(), "versions");
}

Result<std::vector<std::uint8_t>> PlatformClient::downloadPackage(const std::string& hash) {
    auto expected = crypto::fromHex(hash);
    if (!expected || expected->size() != 32) return makeError(ErrorCode::InvalidArgument, "'{}' is not a package hash", hash);
    const int saved = m_timeoutSeconds;
    m_timeoutSeconds = std::max(m_timeoutSeconds, 300);
    auto body = call("GET", "/packages/" + hash, {}, m_token.empty() ? Auth::None : Auth::User);
    m_timeoutSeconds = saved;
    if (!body) return body.error();
    std::vector<std::uint8_t> bytes(body->begin(), body->end());
    const auto actual = crypto::hash256(bytes);
    if (!crypto::constantTimeEqual(actual, *expected)) {
        return makeError(ErrorCode::InvalidData, "The downloaded game is damaged (its hash does not match)");
    }
    return bytes;
}

Result<JoinTicket> PlatformClient::join(std::int64_t gameId) {
    auto body = call("POST", std::format("/games/{}/join", gameId), "{}");
    if (!body) return body.error();
    return parseAs<JoinTicket>(body.value(), "ticket");
}

Status PlatformClient::report(std::int64_t gameId, const std::string& reason) {
    auto body = call("POST", std::format("/games/{}/reports", gameId), json{{"reason", reason}}.dump());
    if (!body) return body.error();
    return {};
}

Result<std::string> PlatformClient::playerData(std::int64_t gameId, const std::string& username) {
    auto body = call("GET", std::format("/games/{}/data/{}", gameId, encodeQuery(username)));
    if (!body) return body.error();
    try {
        return json::parse(body.value()).at("data").dump();
    } catch (const std::exception& e) {
        return makeError(ErrorCode::InvalidData, "Unexpected answer: {}", e.what());
    }
}

Result<std::vector<ReportInfo>> PlatformClient::reports(bool openOnly) {
    auto body = call("GET", std::format("/admin/reports?state={}", openOnly ? "open" : "all"));
    if (!body) return body.error();
    return parseAs<std::vector<ReportInfo>>(body.value(), "reports");
}

Status PlatformClient::resolveReport(std::int64_t reportId, const std::string& action, const std::string& note) {
    auto body = call("POST", std::format("/admin/reports/{}/resolve", reportId),
                     json{{"action", action}, {"note", note}}.dump());
    if (!body) return body.error();
    return {};
}

Status PlatformClient::setGameHidden(std::int64_t gameId, bool hidden, const std::string& note) {
    auto body = call("POST", std::format("/admin/games/{}/hide", gameId), json{{"hidden", hidden}, {"note", note}}.dump());
    if (!body) return body.error();
    return {};
}

Status PlatformClient::setUserBanned(const std::string& username, bool banned, const std::string& note) {
    auto body = call("POST", std::format("/admin/users/{}/ban", encodeQuery(username)),
                     json{{"banned", banned}, {"note", note}}.dump());
    if (!body) return body.error();
    return {};
}

Result<std::vector<AuditEntry>> PlatformClient::audit(int limit) {
    auto body = call("GET", std::format("/admin/audit?limit={}", limit));
    if (!body) return body.error();
    return parseAs<std::vector<AuditEntry>>(body.value(), "entries");
}

Status PlatformClient::serverReady(std::uint16_t port) {
    auto body = call("POST", "/server/ready", json{{"port", port}}.dump(), Auth::Server);
    if (!body) return body.error();
    return {};
}

Result<bool> PlatformClient::serverHeartbeat(const std::vector<std::string>& userIds) {
    auto body = call("POST", "/server/heartbeat", json{{"players", userIds}}.dump(), Auth::Server);
    if (!body) return body.error();
    return parseAs<bool>(body.value(), "stop");
}

Result<runtime::PlayerData> PlatformClient::serverLoadData(const std::string& userId) {
    auto body = call("GET", "/server/data/" + encodeQuery(userId), {}, Auth::Server);
    if (!body) return body.error();
    try {
        return playerDataFromJson(json::parse(body.value()).at("data").dump());
    } catch (const std::exception& e) {
        return makeError(ErrorCode::InvalidData, "Unexpected answer: {}", e.what());
    }
}

Status PlatformClient::serverSaveData(const std::string& userId, const runtime::PlayerData& data, bool release) {
    const std::string payload = std::format(R"({{"data":{},"release":{}}})", playerDataToJson(data), release);
    auto body = call("PUT", "/server/data/" + encodeQuery(userId), payload, Auth::Server);
    if (!body) return body.error();
    return {};
}

Status PlatformClient::serverRelease(const std::string& userId) {
    auto body = call("POST", "/server/release/" + encodeQuery(userId), "{}", Auth::Server);
    if (!body) return body.error();
    return {};
}

Status PlatformClient::serverEnded() {
    auto body = call("POST", "/server/ended", "{}", Auth::Server);
    if (!body) return body.error();
    return {};
}

// --- Player data JSON ---------------------------------------------------------------------

std::string playerDataToJson(const runtime::PlayerData& data) {
    json j = json::object();
    for (const auto& entry : data.values) {
        const std::string& key = entry.first; // (Apple Clang cannot capture structured bindings)
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, Vec2>) {
                    j[key] = {{"x", v.x}, {"y", v.y}};
                } else {
                    j[key] = v;
                }
            },
            entry.second);
    }
    return j.dump();
}

Result<runtime::PlayerData> playerDataFromJson(std::string_view text) {
    try {
        const json j = json::parse(text);
        if (!j.is_object()) return makeError(ErrorCode::InvalidData, "saved data must be an object");
        runtime::PlayerData data;
        for (const auto& [key, value] : j.items()) {
            std::optional<scene::ScriptValue> v;
            if (value.is_number()) {
                v = value.get<double>();
            } else if (value.is_boolean()) {
                v = value.get<bool>();
            } else if (value.is_string()) {
                v = value.get<std::string>();
            } else if (value.is_object() && value.size() == 2 && value.contains("x") && value.contains("y")) {
                v = Vec2{value.at("x").get<float>(), value.at("y").get<float>()};
            } else {
                return makeError(ErrorCode::InvalidData, "saved value '{}' has an unsupported type", key);
            }
            if (auto s = data.set(key, std::move(v)); !s) return s.error();
        }
        return data;
    } catch (const std::exception& e) {
        return makeError(ErrorCode::InvalidData, "saved data is not valid JSON: {}", e.what());
    }
}

// --- Package cache ------------------------------------------------------------------------

std::filesystem::path PackageCache::pathFor(const std::string& hash) const { return m_dir / (hash + ".pxpkg"); }

Result<std::vector<std::uint8_t>> PackageCache::get(PlatformClient& client, const std::string& hash) {
    auto expected = crypto::fromHex(hash);
    if (!expected || expected->size() != 32) return makeError(ErrorCode::InvalidArgument, "'{}' is not a package hash", hash);
    const auto file = pathFor(hash);
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        std::ifstream in(file, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (crypto::constantTimeEqual(crypto::hash256(bytes), *expected)) return bytes;
        std::filesystem::remove(file, ec); // damaged: download again
    }
    auto bytes = client.downloadPackage(hash);
    if (!bytes) return bytes.error();
    std::filesystem::create_directories(m_dir, ec);
    const auto temp = file.string() + ".part";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
    }
    std::filesystem::rename(temp, file, ec);
    return bytes;
}

// --- Per-user state -------------------------------------------------------------------------

std::filesystem::path userPlatformDir() {
    if (const char* overridden = std::getenv("PX_PLATFORM_DIR"); overridden != nullptr && *overridden != 0) {
        return overridden;
    }
#if defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA")) return std::filesystem::path(appData) / "ProjectPixel" / "platform";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / "Library" / "Application Support" / "ProjectPixel" / "platform";
    }
#else
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config != nullptr && *config != 0) {
        return std::filesystem::path(config) / "projectpixel" / "platform";
    }
    if (const char* home = std::getenv("HOME")) return std::filesystem::path(home) / ".config" / "projectpixel" / "platform";
#endif
    return std::filesystem::temp_directory_path() / "projectpixel-platform";
}

std::optional<SavedLogin> loadSavedLogin(const std::filesystem::path& dir) {
    std::ifstream in(dir / "login.json");
    if (!in) return std::nullopt;
    try {
        json j;
        in >> j;
        SavedLogin login;
        login.platformUrl = j.at("platform").get<std::string>();
        login.username = j.at("username").get<std::string>();
        login.token = j.at("token").get<std::string>();
        return login;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

Status saveLogin(const std::filesystem::path& dir, const SavedLogin& login) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / "login.json";
    {
        std::ofstream out(file, std::ios::trunc);
        if (!out) return makeError(ErrorCode::IoError, "Cannot write {}", file.string());
    }
    // Owner-only before the token is written.
    std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    std::ofstream out(file, std::ios::trunc);
    out << json{{"platform", login.platformUrl}, {"username", login.username}, {"token", login.token}}.dump(2) << "\n";
    if (!out) return makeError(ErrorCode::IoError, "Cannot write {}", file.string());
    return {};
}

void forgetLogin(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove(dir / "login.json", ec);
}

} // namespace px::online
