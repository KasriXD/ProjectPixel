// Running the platform on the internet (docs/deployment.md): client
// addresses behind a reverse proxy, /healthz and /metrics, backups and
// restoring them, and renewed TLS certificates.
#include "Operations.hpp"
#include "PlatformHarness.hpp"
#include "px/core/FileSystem.hpp"

#include <doctest.h>
#include <httplib.h>

#if defined(PX_HAVE_TLS)
#include "Certificates.hpp"
#endif

#include <fstream>

using namespace px;
using namespace px::online;
using pxtest::contains;
using pxtest::TestPlatform;

TEST_CASE("Operations: trusted proxies and the client's address") {
    auto ranges = platform::AddressRanges::parse({"127.0.0.1", "10.0.0.0/8", " fd00::/8 ", "192.168.1.0/24"});
    REQUIRE(ranges.ok());
    CHECK(ranges->contains("127.0.0.1"));
    CHECK_FALSE(ranges->contains("127.0.0.2"));
    CHECK(ranges->contains("10.200.3.4"));
    CHECK_FALSE(ranges->contains("11.0.0.1"));
    CHECK(ranges->contains("192.168.1.77"));
    CHECK_FALSE(ranges->contains("192.168.2.1"));
    CHECK(ranges->contains("::ffff:10.1.2.3")); // IPv4 on a dual-stack socket
    CHECK(ranges->contains("fd12:3456::1"));
    CHECK_FALSE(ranges->contains("fe80::1"));
    CHECK_FALSE(ranges->contains("not an address"));
    CHECK_FALSE(platform::AddressRanges::parse({"10.0.0.0/33"}).ok());
    CHECK_FALSE(platform::AddressRanges::parse({"example.com"}).ok());
    CHECK(platform::isLoopback("127.0.0.1"));
    CHECK(platform::isLoopback("::1"));
    CHECK_FALSE(platform::isLoopback("10.0.0.1"));

    const auto& t = *ranges;
    // Without a trusted peer the header means nothing.
    CHECK(platform::clientAddress(t, "203.0.113.9", "198.51.100.1") == "203.0.113.9");
    CHECK(platform::clientAddress(platform::AddressRanges{}, "127.0.0.1", "198.51.100.1") == "127.0.0.1");
    // Through the proxy: the address it saw.
    CHECK(platform::clientAddress(t, "127.0.0.1", "198.51.100.1") == "198.51.100.1");
    // Through two proxies, and a client that wrote its own header first.
    CHECK(platform::clientAddress(t, "127.0.0.1", "6.6.6.6, 198.51.100.1, 10.0.0.5") == "198.51.100.1");
    CHECK(platform::clientAddress(t, "127.0.0.1", "  ") == "127.0.0.1");
    CHECK(platform::clientAddress(t, "127.0.0.1", "garbage") == "127.0.0.1");
    CHECK(platform::clientAddress(t, "127.0.0.1", "10.0.0.7") == "10.0.0.7"); // all trusted: the first
}

TEST_CASE("Operations: rate limits count the client behind a trusted proxy") {
    TestPlatform p(true, 0, [](platform::ServiceConfig& c) { c.trustedProxies = {"127.0.0.1", "::1"}; });
    (void)p.account("Ada");
    httplib::Client http(p.service->url());
    const auto login = [&](const char* from, const std::string& name) {
        const httplib::Headers headers{{"X-Forwarded-For", from}};
        auto res = http.Post("/api/v1/sessions", headers, std::format(R"({{"username":"{}","password":"wrong one"}})", name),
                             "application/json");
        REQUIRE(res);
        return res->status;
    };
    // (Other names each time: the limit per address, not the one per name.)
    int status = 0;
    for (int i = 0; i < 25 && status != 429; ++i) status = login("198.51.100.1", std::format("nobody{}", i));
    CHECK(status == 429);
    CHECK(login("198.51.100.2", "Ada") == 401); // someone else is not limited
    CHECK(p.service->metrics().rateLimited.load() >= 1);
}

TEST_CASE("Operations: /healthz and /metrics") {
    TestPlatform p;
    auto ada = p.account("Ada");
    httplib::Client http(p.service->url());
    auto health = http.Get("/healthz");
    REQUIRE(health);
    CHECK(health->status == 200);
    CHECK(contains(health->body, R"("status":"ok")"));
    CHECK(platform::checkHealth(p.service->url()).ok());
    CHECK_FALSE(platform::checkHealth("http://127.0.0.1:1").ok());

    auto metrics = http.Get("/metrics");
    REQUIRE(metrics);
    CHECK(metrics->status == 200);
    CHECK(contains(metrics->get_header_value("Content-Type"), "text/plain"));
    const std::string& m = metrics->body;
    CHECK(contains(m, "# TYPE px_accounts gauge\npx_accounts 1\n"));
    CHECK(contains(m, "px_build_info{version=\""));
    CHECK(contains(m, "px_http_requests_total{code=\"2xx\"} "));
    CHECK(contains(m, "px_game_servers{state=\"ready\"} 0"));
    CHECK(contains(m, "px_game_servers_ended_total{how=\"cpu_limit\"} 0"));
    CHECK(contains(m, "px_backup_last_success_timestamp_seconds 0"));
#if defined(__linux__)
    CHECK(contains(m, "process_resident_memory_bytes "));
#endif
    // Not through a proxy, not from elsewhere.
    auto proxied = http.Get("/metrics", httplib::Headers{{"X-Forwarded-For", "198.51.100.1"}});
    REQUIRE(proxied);
    CHECK(proxied->status == 403);

    // With a token: only with it.
    TestPlatform q(false, 0, [](platform::ServiceConfig& c) { c.metricsToken = "secret-metrics-token"; });
    httplib::Client other(q.service->url());
    auto refused = other.Get("/metrics");
    REQUIRE(refused);
    CHECK(refused->status == 401);
    auto wrong = other.Get("/metrics", httplib::Headers{{"Authorization", "Bearer secret-metrics-tokem"}});
    REQUIRE(wrong);
    CHECK(wrong->status == 401);
    auto allowed = other.Get("/metrics", httplib::Headers{{"Authorization", "Bearer secret-metrics-token"},
                                                          {"X-Forwarded-For", "198.51.100.1"}});
    REQUIRE(allowed);
    CHECK(allowed->status == 200);
}

TEST_CASE("Operations: backups while running, and restoring one") {
    TestPlatform p(false, 0, [](platform::ServiceConfig& c) { c.backupKeep = 2; });
    auto ada = p.account("Ada");
    auto bob = p.account("Bob");
    REQUIRE(p.service->setRole("Bob", Role::Admin).ok());
    auto game = ada.createGame("Coin Run", "Collect every coin.", Visibility::Public);
    REQUIRE(game.ok());
    auto version = ada.publish(game->id, pxtest::examplePackage("platformer"), "first");
    REQUIRE(version.ok());
    auto picture = readLocalFile(std::filesystem::path(PX_EXAMPLES_DIR) / "topdown" / "assets" / "scenery.png");
    REQUIRE(picture.ok());
    REQUIRE(ada.uploadThumbnail(game->id, picture.value()).ok());

    // An administrator asks for one over the API.
    httplib::Client http(p.service->url());
    const httplib::Headers admin{{"Authorization", "Bearer " + bob.token()}};
    auto made = http.Post("/api/v1/admin/backups", admin, "", "application/json");
    REQUIRE(made);
    REQUIRE_MESSAGE(made->status == 201, made->body);
    auto refused = http.Post("/api/v1/admin/backups", httplib::Headers{{"Authorization", "Bearer " + ada.token()}}, "",
                             "application/json");
    REQUIRE(refused);
    CHECK(refused->status == 403);

    auto backups = platform::listBackups(p.dir / "backups");
    REQUIRE(backups.size() == 1);
    const platform::BackupInfo first = backups.front(); // (a copy: the list is read again below)
    CHECK(first.files == 2); // the package and the picture
    CHECK(first.missing.empty());
    CHECK(first.schemaVersion >= 3);
    const std::filesystem::path folder = first.folder;
    CHECK(std::filesystem::is_regular_file(folder / "platform.db"));
    CHECK_FALSE(std::filesystem::exists(folder / "platform.db-wal"));
    CHECK(std::filesystem::is_regular_file(folder / "platform.key"));
    CHECK(std::filesystem::is_regular_file(folder / "packages" / version->packageHash.substr(0, 2) /
                                           (version->packageHash + ".pxpkg")));
    CHECK(std::filesystem::is_regular_file(folder / "backup.json"));
    CHECK(p.service->metrics().backups.load() == 1);
    CHECK(p.service->metrics().lastBackup.load() > 0);

    // Only the newest two stay (names differ even within a second).
    REQUIRE(p.service->backup().ok());
    auto last = p.service->backup();
    REQUIRE(last.ok());
    backups = platform::listBackups(p.dir / "backups");
    REQUIRE(backups.size() == 2);
    CHECK(backups.front().name == last->name);
    CHECK_FALSE(std::filesystem::exists(folder));

    // After the backup, Ada's account goes; the backup still has her.
    REQUIRE(ada.deleteAccount("password-Ada").ok());

    // Restoring: into an empty folder only.
    CHECK(contains(platform::restoreBackup(last->folder, p.dir).error().message, "has a platform already"));
    CHECK(contains(platform::restoreBackup(p.dir, pxtest::tempDir("not-restored")).error().message, "not a platform backup"));
    const auto restoredDir = pxtest::tempDir("restored");
    std::filesystem::remove_all(restoredDir);
    auto restored = platform::restoreBackup(last->folder, restoredDir);
    REQUIRE_MESSAGE(restored.ok(), (restored.ok() ? "" : restored.error().message));
    CHECK(restored->accounts == 2);
    CHECK(restored->games == 1);
    CHECK(restored->files == 2);
    CHECK(restored->missing.empty());

    platform::ServiceConfig config;
    config.dataDir = restoredDir;
    config.port = 0;
    config.passwordHashing = {64, 1};
    config.headlessExe = PX_HEADLESS_EXE;
    config.serverExe = PX_SERVER_EXE;
    config.backupHours = 0;
    auto service = platform::PlatformService::create(config);
    REQUIRE(service.ok());
    REQUIRE((*service)->start().ok());
    CHECK((*service)->publicKey() == p.service->publicKey()); // tickets keep working
    PlatformClient back((*service)->url());
    REQUIRE(back.login("Ada", "password-Ada").ok());
    auto games = back.myGames();
    REQUIRE(games.ok());
    REQUIRE(games->size() == 1);
    CHECK(back.downloadPackage(version->packageHash).ok());
    CHECK_FALSE(back.thumbnail(game->id)->empty());
    (*service)->stop();
    service->reset();
    std::filesystem::remove_all(restoredDir);
}

#if defined(PX_HAVE_TLS)
TEST_CASE("Operations: a renewed TLS certificate is taken into use") {
    const auto certDir = pxtest::tempDir("renew");
    const auto cert = certDir / "cert.pem";
    const auto key = certDir / "key.pem";
    REQUIRE(platform::makeDevCertificate({"localhost", "127.0.0.1"}, cert, key).ok());
    TestPlatform p(false, 0, [&](platform::ServiceConfig& c) {
        c.httpsPort = -1;
        c.tlsCert = cert;
        c.tlsKey = key;
    });
    const auto expires = p.service->metrics().certificateExpires.load();
    CHECK(expires > std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const auto pem = [](const std::filesystem::path& f) {
        std::ifstream in(f, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    CHECK(platform::certificateExpiry(pem(cert), pem(key)).value() == expires);

    // A new certificate (another key) where the old one was.
    const auto renewedDir = pxtest::tempDir("renewed");
    REQUIRE(platform::makeDevCertificate({"localhost", "127.0.0.1"}, renewedDir / "cert.pem", renewedDir / "key.pem").ok());
    CHECK_FALSE(platform::certificateExpiry(pem(renewedDir / "cert.pem"), pem(key)).ok()); // not its key

    // Half-way through a renewal (new certificate, old key): kept as it was.
    std::filesystem::copy_file(cert, certDir / "original.pem");
    std::filesystem::copy_file(renewedDir / "cert.pem", cert, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::last_write_time(cert, std::filesystem::last_write_time(cert) + std::chrono::seconds(2));
    CHECK_FALSE(p.service->reloadCertificates().ok());
    CHECK(p.service->metrics().certificateReloadFailures.load() == 1);
    PlatformClient old(p.service->httpsUrl());
    old.setCaFile((certDir / "original.pem").string());
    CHECK(old.info().ok());
    // The key arrives: now it is used.
    std::filesystem::copy_file(renewedDir / "key.pem", key, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::last_write_time(key, std::filesystem::last_write_time(key) + std::chrono::seconds(2));
    REQUIRE(p.service->reloadCertificates().ok());
    CHECK(p.service->metrics().certificateReloads.load() == 1);
    CHECK(p.service->reloadCertificates().ok()); // nothing changed: nothing to do
    CHECK(p.service->metrics().certificateReloads.load() == 1);
    PlatformClient renewed(p.service->httpsUrl());
    renewed.setCaFile((renewedDir / "cert.pem").string());
    auto info = renewed.info();
    REQUIRE_MESSAGE(info.ok(), (info.ok() ? "" : info.error().message));
    CHECK_FALSE(old.info().ok()); // the old certificate is gone
    std::filesystem::remove_all(certDir);
    std::filesystem::remove_all(renewedDir);
}
#endif
