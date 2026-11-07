// Running the platform on the internet (docs/deployment.md): client
// addresses behind a reverse proxy, /healthz and /metrics, backups and
// restoring them, and reloading the TLS certificate when it is renewed.
#include "Operations.hpp"

#include "ServiceContext.hpp"
#include "px/core/Version.hpp"

#if defined(PX_HAVE_TLS)
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#endif

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace px::platform {

namespace {

using service::json;

std::string trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return std::string(s);
}

// An address as 16 bytes (IPv4 mapped into IPv6), or nothing.
std::optional<std::array<std::uint8_t, 16>> addressBytes(std::string_view text) {
    std::string s(text);
    if (s.size() > 2 && s.front() == '[' && s.back() == ']') s = s.substr(1, s.size() - 2);
    if (const auto percent = s.find('%'); percent != std::string::npos) s.resize(percent); // zone
    std::array<std::uint8_t, 16> out{};
    in_addr v4{};
    if (s.find(':') == std::string::npos) {
        if (inet_pton(AF_INET, s.c_str(), &v4) != 1) return std::nullopt;
        out[10] = 0xff;
        out[11] = 0xff;
        std::memcpy(out.data() + 12, &v4, 4);
        return out;
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, s.c_str(), &v6) != 1) return std::nullopt;
    std::memcpy(out.data(), &v6, 16);
    return out;
}

bool isMappedV4(const std::array<std::uint8_t, 16>& a) {
    for (int i = 0; i < 10; ++i) {
        if (a[static_cast<std::size_t>(i)] != 0) return false;
    }
    return a[10] == 0xff && a[11] == 0xff;
}

// Days from 1970-01-01 to a date (proleptic Gregorian).
std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::string backupName(std::int64_t unixTime) {
    const std::time_t t = static_cast<std::time_t>(unixTime);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return std::format("{:04}{:02}{:02}-{:02}{:02}{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                       tm.tm_sec);
}

std::optional<std::string> readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// A file into a backup: a hard link when they share a disk (packages never
// change once written), else a copy.
bool linkOrCopy(const std::filesystem::path& from, const std::filesystem::path& to, std::uint64_t& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(to.parent_path(), ec);
    const auto size = std::filesystem::file_size(from, ec);
    if (ec) return false;
    std::filesystem::create_hard_link(from, to, ec);
    if (ec) {
        ec.clear();
        std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return false;
    }
    bytes += size;
    return true;
}

// Files a database refers to, relative to the data folder.
std::vector<std::filesystem::path> referencedFiles(Database& db) {
    std::vector<std::filesystem::path> out;
    auto packages = db.prepare("SELECT DISTINCT package_hash FROM versions");
    while (packages.step()) {
        const std::string hash = packages.text(0);
        if (hash.size() < 2) continue;
        out.push_back(std::filesystem::path("packages") / hash.substr(0, 2) / (hash + ".pxpkg"));
    }
    auto pictures = db.prepare("SELECT DISTINCT thumbnail FROM games WHERE thumbnail <> ''");
    while (pictures.step()) out.push_back(std::filesystem::path("thumbnails") / (pictures.text(0) + ".png"));
    return out;
}

void metric(std::string& out, std::string_view name, std::string_view type, std::string_view help) {
    out += std::format("# HELP {} {}\n# TYPE {} {}\n", name, help, name, type);
}

template <typename T>
void sample(std::string& out, std::string_view name, T value, std::string_view labels = {}) {
    if (labels.empty()) {
        out += std::format("{} {}\n", name, value);
    } else {
        out += std::format("{}{{{}}} {}\n", name, labels, value);
    }
}

} // namespace

// --- Addresses --------------------------------------------------------------------------------------

Result<AddressRanges> AddressRanges::parse(const std::vector<std::string>& items) {
    AddressRanges out;
    for (const auto& raw : items) {
        const std::string item = trimmed(raw);
        if (item.empty()) continue;
        const auto slash = item.find('/');
        const auto bytes = addressBytes(item.substr(0, slash));
        if (!bytes) return makeError(ErrorCode::InvalidArgument, "'{}' is not an address or a range like 10.0.0.0/8", item);
        Range r;
        r.bytes = *bytes;
        const bool v4 = isMappedV4(*bytes) && item.find(':') == std::string::npos;
        if (slash != std::string::npos) {
            int bits = -1;
            try {
                bits = std::stoi(item.substr(slash + 1));
            } catch (const std::exception&) {
            }
            if (bits < 0 || bits > (v4 ? 32 : 128)) return makeError(ErrorCode::InvalidArgument, "'{}': a bad prefix length", item);
            r.bits = v4 ? bits + 96 : bits;
        }
        out.m_ranges.push_back(r);
    }
    return out;
}

bool AddressRanges::contains(std::string_view address) const {
    const auto bytes = addressBytes(address);
    if (!bytes) return false;
    for (const Range& r : m_ranges) {
        bool match = true;
        for (int bit = 0; bit < r.bits && match; bit += 8) {
            const int left = std::min(8, r.bits - bit);
            const auto mask = static_cast<std::uint8_t>(0xff << (8 - left));
            const auto i = static_cast<std::size_t>(bit / 8);
            match = ((*bytes)[i] & mask) == (r.bytes[i] & mask);
        }
        if (match) return true;
    }
    return false;
}

bool isLoopback(std::string_view address) {
    static const AddressRanges loopback = AddressRanges::parse({"127.0.0.0/8", "::1"}).value();
    return loopback.contains(address);
}

std::string clientAddress(const AddressRanges& trustedProxies, std::string_view peer, std::string_view forwardedFor) {
    if (trustedProxies.empty() || forwardedFor.empty() || !trustedProxies.contains(peer)) return std::string(peer);
    std::string candidate(peer);
    std::size_t end = forwardedFor.size();
    while (true) {
        const auto comma = forwardedFor.rfind(',', end == 0 ? 0 : end - 1);
        const std::size_t start = comma == std::string_view::npos ? 0 : comma + 1;
        const std::string address = trimmed(forwardedFor.substr(start, end - start));
        if (!addressBytes(address)) return candidate; // garbage: stop at the last good one
        candidate = address;
        if (!trustedProxies.contains(address)) return candidate;
        if (comma == std::string_view::npos) return candidate;
        end = comma;
    }
}

// --- Backups ----------------------------------------------------------------------------------------

std::vector<BackupInfo> listBackups(const std::filesystem::path& dir) {
    std::vector<BackupInfo> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_directory(ec) || entry.path().filename().string().starts_with('.')) continue;
        const auto text = readFile(entry.path() / "backup.json");
        if (!text) continue;
        const json j = json::parse(*text, nullptr, false);
        if (j.is_discarded() || j.value("format", "") != "px.backup") continue;
        BackupInfo b;
        b.name = entry.path().filename().string();
        b.folder = entry.path();
        b.created = j.value("created", std::int64_t{0});
        b.schemaVersion = j.value("schemaVersion", 0);
        b.engineVersion = j.value("engineVersion", "");
        b.files = j.value("files", std::size_t{0});
        b.bytes = j.value("bytes", std::uint64_t{0});
        b.seconds = j.value("seconds", 0.0);
        b.missing = j.value("missing", std::vector<std::string>{});
        out.push_back(std::move(b));
    }
    std::sort(out.begin(), out.end(), [](const BackupInfo& a, const BackupInfo& b) { return a.name > b.name; });
    return out;
}

Result<RestoreReport> restoreBackup(const std::filesystem::path& backup, const std::filesystem::path& dataDir) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(backup / "backup.json", ec) || !std::filesystem::is_regular_file(backup / "platform.db", ec)) {
        return makeError(ErrorCode::InvalidArgument, "{} is not a platform backup (no backup.json and platform.db)", backup.string());
    }
    if (std::filesystem::exists(dataDir / "platform.db", ec)) {
        return makeError(ErrorCode::InvalidArgument, "{} has a platform already: restore into a new folder", dataDir.string());
    }
    std::filesystem::create_directories(dataDir, ec);
    const auto copy = [&](const std::filesystem::path& from, const std::filesystem::path& to) -> Status {
        std::filesystem::copy(from, to, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                              ec);
        if (ec) return makeError(ErrorCode::IoError, "Cannot copy {} to {}: {}", from.string(), to.string(), ec.message());
        return {};
    };
    if (auto s = copy(backup / "platform.db", dataDir / "platform.db"); !s) return s.error();
    if (std::filesystem::exists(backup / "platform.key", ec)) {
        if (auto s = copy(backup / "platform.key", dataDir / "platform.key"); !s) return s.error();
        std::filesystem::permissions(dataDir / "platform.key", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     ec);
    }
    for (const char* folder : {"packages", "thumbnails"}) {
        if (!std::filesystem::exists(backup / folder, ec)) continue;
        if (auto s = copy(backup / folder, dataDir / folder); !s) return s.error();
    }
    auto db = Database::open(dataDir / "platform.db");
    if (!db) return db.error();
    RestoreReport report;
    try {
        const std::string check = (*db)->integrityCheck();
        if (check != "ok") return makeError(ErrorCode::InvalidData, "The restored database is damaged: {}", check);
        auto accounts = (*db)->prepare("SELECT COUNT(*) FROM users");
        if (accounts.step()) report.accounts = accounts.integer(0);
        auto games = (*db)->prepare("SELECT COUNT(*) FROM games");
        if (games.step()) report.games = games.integer(0);
        for (const auto& file : referencedFiles(**db)) {
            if (std::filesystem::is_regular_file(dataDir / file, ec)) {
                ++report.files;
            } else {
                report.missing.push_back(file.generic_string());
            }
        }
    } catch (const DatabaseError& e) {
        return makeError(ErrorCode::InvalidData, "The restored database cannot be read: {}", e.what());
    }
    return report;
}

// --- Monitoring -------------------------------------------------------------------------------------

std::string processMetrics() {
    std::string out;
#if defined(__linux__)
    const double ticks = static_cast<double>(sysconf(_SC_CLK_TCK));
    const double page = static_cast<double>(sysconf(_SC_PAGESIZE));
    if (const auto stat = readFile("/proc/self/stat")) {
        // Fields after the command name (which may hold spaces) in parentheses.
        const auto close = stat->rfind(')');
        std::istringstream fields(close == std::string::npos ? std::string() : stat->substr(close + 2));
        std::vector<std::string> f{std::istream_iterator<std::string>(fields), std::istream_iterator<std::string>()};
        if (f.size() > 20) {
            const double cpu = (std::stod(f[11]) + std::stod(f[12])) / ticks; // utime, stime
            metric(out, "process_cpu_seconds_total", "counter", "Processor time used by the platform (not its game servers).");
            sample(out, "process_cpu_seconds_total", cpu);
            double boot = 0.0;
            if (const auto procStat = readFile("/proc/stat")) {
                const auto at = procStat->find("btime ");
                if (at != std::string::npos) boot = std::stod(procStat->substr(at + 6));
            }
            metric(out, "process_start_time_seconds", "gauge", "When the platform started (Unix time).");
            sample(out, "process_start_time_seconds", boot + std::stod(f[19]) / ticks);
        }
    }
    if (const auto statm = readFile("/proc/self/statm")) {
        std::istringstream fields(*statm);
        double size = 0.0;
        double resident = 0.0;
        fields >> size >> resident;
        metric(out, "process_resident_memory_bytes", "gauge", "Memory the platform has in RAM.");
        sample(out, "process_resident_memory_bytes", static_cast<std::uint64_t>(resident * page));
        metric(out, "process_virtual_memory_bytes", "gauge", "The platform's address space.");
        sample(out, "process_virtual_memory_bytes", static_cast<std::uint64_t>(size * page));
    }
    std::error_code ec;
    std::size_t fds = 0;
    for (auto it = std::filesystem::directory_iterator("/proc/self/fd", ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
        ++fds;
    }
    metric(out, "process_open_fds", "gauge", "Open files and sockets.");
    sample(out, "process_open_fds", fds);
#endif
    return out;
}

Result<std::int64_t> certificateExpiry(std::string_view certPem, std::string_view keyPem) {
#if defined(PX_HAVE_TLS)
    static std::once_flag psa;
    std::call_once(psa, [] { (void)psa_crypto_init(); });
    mbedtls_x509_crt cert;
    mbedtls_x509_crt_init(&cert);
    const std::string certText(certPem);
    int rc = mbedtls_x509_crt_parse(&cert, reinterpret_cast<const unsigned char*>(certText.c_str()), certText.size() + 1);
    if (rc < 0 || cert.version == 0) {
        mbedtls_x509_crt_free(&cert);
        return makeError(ErrorCode::InvalidData, "Not a PEM certificate");
    }
    const mbedtls_x509_time& t = cert.valid_to;
    const std::int64_t expires = daysFromCivil(t.year, static_cast<unsigned>(t.mon), static_cast<unsigned>(t.day)) * 86400 +
                                 t.hour * 3600 + t.min * 60 + t.sec;
    Status pair;
    if (!keyPem.empty()) {
        mbedtls_pk_context key;
        mbedtls_pk_init(&key);
        mbedtls_entropy_context entropy;
        mbedtls_ctr_drbg_context drbg;
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        const std::string keyText(keyPem);
        rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0);
        if (rc == 0) {
            rc = mbedtls_pk_parse_key(&key, reinterpret_cast<const unsigned char*>(keyText.c_str()), keyText.size() + 1,
                                      nullptr, 0, mbedtls_ctr_drbg_random, &drbg);
        }
        if (rc != 0) {
            pair = makeError(ErrorCode::InvalidData, "Not a PEM private key");
        } else if (mbedtls_pk_check_pair(&cert.pk, &key, mbedtls_ctr_drbg_random, &drbg) != 0) {
            pair = makeError(ErrorCode::InvalidData, "The private key does not belong to the certificate");
        }
        mbedtls_pk_free(&key);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
    mbedtls_x509_crt_free(&cert);
    if (!pair) return pair.error();
    return expires;
#else
    (void)certPem;
    (void)keyPem;
    return makeError(ErrorCode::Unsupported, "This build has no TLS");
#endif
}

Status checkHealth(const std::string& url) {
    httplib::Client client(url);
    client.set_connection_timeout(3);
    client.set_read_timeout(5);
#if defined(PX_HAVE_TLS)
    client.enable_server_certificate_verification(false); // this computer asking itself
#endif
    auto res = client.Get("/healthz");
    if (!res) return makeError(ErrorCode::IoError, "Cannot reach {} ({})", url, httplib::to_string(res.error()));
    if (res->status != 200) return makeError(ErrorCode::IoError, "{}/healthz answered {}: {}", url, res->status, res->body);
    return {};
}

// --- The service's side -------------------------------------------------------------------------------

namespace {

struct Operations {
    PlatformService::Context& c;
    explicit Operations(PlatformService::Context& context) : c(context) {}

    // GET /healthz: 200 when the database answers (load balancers, containers).
    void health(const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        try {
            std::lock_guard lock(c.dbMutex());
            auto st = c.db().prepare("SELECT 1");
            if (!st.step()) throw DatabaseError("no answer");
        } catch (const std::exception& e) {
            log::error("Health check: {}", e.what());
            service::sendJson(res, 503, {{"status", "unavailable"}});
            return;
        }
        std::size_t servers = 0;
        for (const auto& s : c.fleet().sessions()) servers += s.state != "ended" ? 1 : 0;
        const auto& m = c.s.metrics();
        service::sendJson(res, 200, {{"status", "ok"},
                                     {"version", std::string(version::string)},
                                     {"uptimeSeconds", now() - m.started},
                                     {"gameServers", servers},
                                     {"lastBackup", m.lastBackup.load()}});
    }

    // GET /metrics: Prometheus' text format. With --metrics-token only with
    // that bearer token; without, only from this computer (not through a proxy).
    void metrics(const httplib::Request& req, httplib::Response& res) {
        const std::string& token = c.config().metricsToken;
        if (!token.empty()) {
            const std::string given = PlatformService::Context::bearer(req, "Bearer");
            if (!crypto::constantTimeEqual(crypto::bytesOf(given), crypto::bytesOf(token))) {
                service::fail(401, "not_allowed", "/metrics needs the metrics token");
            }
        } else if (!isLoopback(req.remote_addr) || req.has_header("X-Forwarded-For") || req.has_header("Forwarded")) {
            service::fail(403, "not_allowed", "/metrics is only for this computer (or set PX_METRICS_TOKEN)");
        }
        std::string out;
        const ServiceMetrics& m = c.s.metrics();
        metric(out, "px_build_info", "gauge", "The platform's version.");
        sample(out, "px_build_info", 1, std::format("version=\"{}\"", version::string));
        metric(out, "px_start_time_seconds", "gauge", "When the platform started (Unix time).");
        sample(out, "px_start_time_seconds", m.started);

        metric(out, "px_http_requests_total", "counter", "HTTP requests answered, by status class.");
        for (int i = 1; i <= 5; ++i) {
            sample(out, "px_http_requests_total", m.requests[static_cast<std::size_t>(i)].load(), std::format("code=\"{}xx\"", i));
        }
        metric(out, "px_http_request_seconds_total", "counter", "Time spent answering HTTP requests.");
        sample(out, "px_http_request_seconds_total", static_cast<double>(m.requestMicros.load()) / 1e6);
        metric(out, "px_rate_limited_total", "counter", "Requests refused by a rate limit.");
        sample(out, "px_rate_limited_total", m.rateLimited.load());

        {
            std::lock_guard lock(c.dbMutex());
            const auto count = [&](const char* sql) {
                auto st = c.db().prepare(sql);
                return st.step() ? st.integer(0) : std::int64_t{0};
            };
            metric(out, "px_accounts", "gauge", "Accounts (not deleted).");
            sample(out, "px_accounts", count("SELECT COUNT(*) FROM users WHERE deleted = 0"));
            metric(out, "px_accounts_confirmed_email", "gauge", "Accounts with a confirmed email address.");
            sample(out, "px_accounts_confirmed_email", count("SELECT COUNT(*) FROM users WHERE email_verified = 1 AND deleted = 0"));
            metric(out, "px_sessions", "gauge", "Login sessions that have not expired.");
            sample(out, "px_sessions", count(std::format("SELECT COUNT(*) FROM sessions WHERE expires >= {}", now()).c_str()));
            metric(out, "px_games", "gauge", "Games with at least one published version.");
            sample(out, "px_games", count("SELECT COUNT(*) FROM games WHERE latest_version > 0"));
            metric(out, "px_game_versions", "gauge", "Published versions of games.");
            sample(out, "px_game_versions", count("SELECT COUNT(*) FROM versions"));
            metric(out, "px_reports_open", "gauge", "Reports waiting for a moderator (games and accounts).");
            sample(out, "px_reports_open", count("SELECT (SELECT COUNT(*) FROM reports WHERE state = 'open') + "
                                                 "(SELECT COUNT(*) FROM user_reports WHERE state = 'open')"));
        }
        std::error_code ec;
        const auto dbBytes = std::filesystem::file_size(c.config().dataDir / "platform.db", ec);
        metric(out, "px_database_bytes", "gauge", "Size of the database file (without its write-ahead log).");
        sample(out, "px_database_bytes", ec ? 0 : dbBytes);

        std::size_t running = 0;
        std::size_t starting = 0;
        std::size_t players = 0;
        for (const auto& s : c.fleet().sessions()) {
            if (s.state == "ready") ++running;
            if (s.state == "starting") ++starting;
            if (s.state != "ended") players += s.players;
        }
        metric(out, "px_game_servers", "gauge", "Game servers, by state.");
        sample(out, "px_game_servers", running, "state=\"ready\"");
        sample(out, "px_game_servers", starting, "state=\"starting\"");
        metric(out, "px_players", "gauge", "Players on game servers.");
        sample(out, "px_players", players);
        const Fleet::Counters fleet = c.fleet().counters();
        metric(out, "px_game_servers_started_total", "counter", "Game servers started.");
        sample(out, "px_game_servers_started_total", fleet.started);
        metric(out, "px_game_servers_ended_total", "counter", "Game servers that ended, by how.");
        for (const char* how : {"finished", "stopped", "failed", "exited", "cpu_limit"}) {
            const auto it = fleet.ended.find(how);
            sample(out, "px_game_servers_ended_total", it == fleet.ended.end() ? 0 : it->second, std::format("how=\"{}\"", how));
        }

        const Mailer::Stats mail = c.mailer().stats();
        metric(out, "px_mail_sent_total", "counter", "Email sent.");
        sample(out, "px_mail_sent_total", mail.sent);
        metric(out, "px_mail_failed_total", "counter", "Email given up on after every attempt.");
        sample(out, "px_mail_failed_total", mail.failed);
        metric(out, "px_mail_queued", "gauge", "Email waiting to be sent.");
        sample(out, "px_mail_queued", mail.queued);

        metric(out, "px_backups_total", "counter", "Backups made.");
        sample(out, "px_backups_total", m.backups.load());
        metric(out, "px_backup_failures_total", "counter", "Backups that failed.");
        sample(out, "px_backup_failures_total", m.backupFailures.load());
        metric(out, "px_backup_last_success_timestamp_seconds", "gauge", "When the last good backup was made (Unix time; 0: none).");
        sample(out, "px_backup_last_success_timestamp_seconds", m.lastBackup.load());
        metric(out, "px_backup_last_seconds", "gauge", "How long the last good backup took.");
        sample(out, "px_backup_last_seconds", static_cast<double>(m.lastBackupMillis.load()) / 1000.0);
        metric(out, "px_backup_last_bytes", "gauge", "Size of the last good backup.");
        sample(out, "px_backup_last_bytes", m.lastBackupBytes.load());

        if (m.certificateExpires.load() != 0) {
            metric(out, "px_tls_certificate_expiry_timestamp_seconds", "gauge", "When the HTTPS certificate expires (Unix time).");
            sample(out, "px_tls_certificate_expiry_timestamp_seconds", m.certificateExpires.load());
            metric(out, "px_tls_certificate_reloads_total", "counter", "Renewed certificates taken into use.");
            sample(out, "px_tls_certificate_reloads_total", m.certificateReloads.load());
            metric(out, "px_tls_certificate_reload_failures_total", "counter", "Renewed certificates that could not be used.");
            sample(out, "px_tls_certificate_reload_failures_total", m.certificateReloadFailures.load());
        }
        out += processMetrics();
        res.set_header("Cache-Control", "no-store");
        res.set_content(out, "text/plain; version=0.0.4; charset=utf-8");
    }

    static json backupJson(const BackupInfo& b) {
        return {{"name", b.name},   {"created", b.created}, {"schemaVersion", b.schemaVersion}, {"files", b.files},
                {"bytes", b.bytes}, {"seconds", b.seconds}, {"missing", b.missing}};
    }

    // GET /admin/backups
    void backups(const httplib::Request& req, httplib::Response& res) {
        (void)c.requireAdmin(req);
        json list = json::array();
        for (const auto& b : listBackups(c.s.backupDir())) list.push_back(backupJson(b));
        service::sendJson(res, 200, {{"backups", list}});
    }

    // POST /admin/backups: one now.
    void backupNow(const httplib::Request& req, httplib::Response& res) {
        const User admin = c.requireAdmin(req);
        auto made = c.s.backup();
        if (!made) service::fail(500, "backup_failed", made.error().message);
        {
            std::lock_guard lock(c.dbMutex());
            c.audit(admin, "backup", made->name, std::format("{} files, {} bytes", made->files, made->bytes));
        }
        service::sendJson(res, 201, {{"backup", backupJson(*made)}});
    }
};

} // namespace

void registerOperationRoutes(httplib::Server& http, PlatformService::Context& context) {
    auto ops = std::make_shared<Operations>(context);
    const auto bind = [&context, ops](void (Operations::*fn)(const httplib::Request&, httplib::Response&)) {
        return context.wrap([ops, fn](const httplib::Request& req, httplib::Response& res) { ((*ops).*fn)(req, res); });
    };
    http.Get("/healthz", bind(&Operations::health));
    http.Get("/metrics", bind(&Operations::metrics));
    http.Get("/api/v1/admin/backups", bind(&Operations::backups));
    http.Post("/api/v1/admin/backups", bind(&Operations::backupNow));
}

// --- PlatformService: backups and certificates ------------------------------------------------------

std::filesystem::path PlatformService::backupDir() const {
    return m_config.backupDir.empty() ? m_config.dataDir / "backups" : m_config.backupDir;
}

Result<BackupInfo> PlatformService::backup() {
    std::lock_guard one(m_backupMutex);
    const auto started = std::chrono::steady_clock::now();
    const auto fail = [this](Error e) -> Result<BackupInfo> {
        ++m_metrics->backupFailures;
        log::error("Backup failed: {}", e.message);
        return e;
    };
    std::error_code ec;
    const auto dir = backupDir();
    std::filesystem::create_directories(dir, ec);
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all, ec); // the platform key goes in there
    BackupInfo info;
    info.created = now();
    info.name = backupName(info.created);
    for (int n = 2; std::filesystem::exists(dir / info.name, ec); ++n) info.name = std::format("{}-{}", backupName(info.created), n);
    const auto partial = dir / ("." + info.name + ".partial");
    std::filesystem::remove_all(partial, ec);
    if (!std::filesystem::create_directories(partial, ec)) {
        return fail(makeError(ErrorCode::IoError, "Cannot create {}: {}", partial.string(), ec.message()));
    }
    {
        // No package or picture is deleted while the backup collects them.
        std::lock_guard files(m_fileRemovalMutex);
        if (auto s = m_db->backupTo(partial / "platform.db", m_dbMutex); !s) {
            std::filesystem::remove_all(partial, ec);
            return fail(s.error());
        }
        if (std::filesystem::exists(m_config.dataDir / "platform.key", ec)) {
            std::filesystem::copy_file(m_config.dataDir / "platform.key", partial / "platform.key", ec);
        }
        // The files the copy of the database refers to (not the live one:
        // that may have changed since).
        auto copy = Database::openReadOnly(partial / "platform.db");
        if (!copy) {
            std::filesystem::remove_all(partial, ec);
            return fail(copy.error());
        }
        try {
            auto version = (*copy)->prepare("PRAGMA user_version");
            if (version.step()) info.schemaVersion = static_cast<int>(version.integer(0));
            for (const auto& file : referencedFiles(**copy)) {
                if (linkOrCopy(m_config.dataDir / file, partial / file, info.bytes)) {
                    ++info.files;
                } else {
                    info.missing.push_back(file.generic_string());
                }
            }
        } catch (const DatabaseError& e) {
            copy->reset();
            std::filesystem::remove_all(partial, ec);
            return fail(makeError(ErrorCode::IoError, "Reading the copied database: {}", e.what()));
        }
    }
    info.bytes += std::filesystem::file_size(partial / "platform.db", ec);
    info.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    info.engineVersion = std::string(version::string);
    const json manifest = {{"format", "px.backup"},
                           {"formatVersion", 1},
                           {"created", info.created},
                           {"platform", m_config.name},
                           {"engineVersion", info.engineVersion},
                           {"schemaVersion", info.schemaVersion},
                           {"files", info.files},
                           {"bytes", info.bytes},
                           {"seconds", info.seconds},
                           {"missing", info.missing}};
    {
        std::ofstream out(partial / "backup.json", std::ios::binary);
        out << manifest.dump(2) << '\n';
        if (!out) {
            std::filesystem::remove_all(partial, ec);
            return fail(makeError(ErrorCode::IoError, "Cannot write {}", (partial / "backup.json").string()));
        }
    }
    std::filesystem::rename(partial, dir / info.name, ec);
    if (ec) {
        std::filesystem::remove_all(partial, ec);
        return fail(makeError(ErrorCode::IoError, "Cannot finish the backup {}: {}", info.name, ec.message()));
    }
    info.folder = dir / info.name;
    // Only the newest ones stay.
    if (m_config.backupKeep > 0) {
        const auto all = listBackups(dir);
        for (std::size_t i = static_cast<std::size_t>(m_config.backupKeep); i < all.size(); ++i) {
            std::filesystem::remove_all(all[i].folder, ec);
            log::info("Removed the old backup {}", all[i].name);
        }
    }
    ++m_metrics->backups;
    m_metrics->lastBackup = info.created;
    m_metrics->lastBackupMillis = static_cast<std::uint64_t>(info.seconds * 1000.0);
    m_metrics->lastBackupBytes = info.bytes;
    if (!info.missing.empty()) log::warn("Backup {}: {} referenced files were missing", info.name, info.missing.size());
    log::info("Backup {} made: {} files, {:.1f} MB in {:.2f} s", info.name, info.files, static_cast<double>(info.bytes) / 1e6,
              info.seconds);
    return info;
}

Status PlatformService::reloadCertificates(bool force) {
#if defined(PX_HAVE_TLS)
    if (!m_https) return {};
    std::error_code ec;
    const auto certTime = std::filesystem::last_write_time(m_config.tlsCert, ec);
    const auto keyTime = std::filesystem::last_write_time(m_config.tlsKey, ec);
    if (ec) return makeError(ErrorCode::IoError, "Cannot read {} or {}", m_config.tlsCert.string(), m_config.tlsKey.string());
    if (!force && certTime == m_certTime && keyTime == m_keyTime) return {};
    const auto cert = readFile(m_config.tlsCert);
    const auto key = readFile(m_config.tlsKey);
    if (!cert || !key) return makeError(ErrorCode::IoError, "Cannot read {} or {}", m_config.tlsCert.string(), m_config.tlsKey.string());
    // A renewal writes two files: a certificate without its key yet is tried
    // again at the next look.
    auto expires = certificateExpiry(*cert, *key);
    if (!expires) {
        ++m_metrics->certificateReloadFailures;
        return makeError(ErrorCode::InvalidData, "The TLS certificate {} was not taken into use: {}", m_config.tlsCert.string(),
                         expires.error().message);
    }
    auto* ssl = static_cast<httplib::SSLServer*>(m_https.get());
    if (!ssl->update_certs_pem(cert->c_str(), key->c_str())) {
        ++m_metrics->certificateReloadFailures;
        return makeError(ErrorCode::InvalidData, "The TLS certificate {} could not be loaded", m_config.tlsCert.string());
    }
    const bool first = m_metrics->certificateExpires.load() == 0;
    m_certTime = certTime;
    m_keyTime = keyTime;
    m_metrics->certificateExpires = *expires;
    if (!first) {
        ++m_metrics->certificateReloads;
        log::info("Took the renewed TLS certificate into use (valid for {} more days); new game servers use it too",
                  (*expires - now()) / 86400);
    }
    return {};
#else
    (void)force;
    return {};
#endif
}

} // namespace px::platform
