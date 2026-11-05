// The platform service through its HTTP API: accounts, games, publishing,
// the catalog and moderation (docs/platform.md).
#include "PlatformHarness.hpp"
#include "px/crypto/Crypto.hpp"
#include "px/crypto/Encoding.hpp"
#include "px/runtime/GameLoader.hpp"

#include <doctest.h>
#include <httplib.h>

#include <fstream>

using namespace px;
using namespace px::online;
using pxtest::contains;
using pxtest::TestPlatform;

TEST_CASE("Platform: accounts register, log in and out") {
    TestPlatform p;
    auto info = p.client().info();
    REQUIRE(info.ok());
    CHECK(info->apiVersion == kPlatformApiVersion);
    CHECK(info->publicKey.size() == 64);

    auto ada = p.client();
    auto reg = ada.registerAccount("Ada", "correct horse");
    REQUIRE(reg.ok());
    CHECK(reg->user.username == "Ada");
    CHECK(reg->user.role == Role::User);
    CHECK_FALSE(reg->token.empty());
    auto me = ada.me();
    REQUIRE(me.ok());
    CHECK(me->id == reg->user.id);

    auto other = p.client();
    auto taken = other.registerAccount("ada", "something else");
    REQUIRE_FALSE(taken.ok());
    CHECK(contains(taken.error().message, "taken"));
    CHECK(contains(other.registerAccount("a", "longenough").error().message, "3 to 20"));
    CHECK(contains(other.registerAccount("no spaces", "longenough").error().message, "3 to 20"));
    CHECK(contains(other.registerAccount("Bob", "short").error().message, "at least 8"));
    CHECK_FALSE(other.me().ok());

    auto wrong = p.client().login("ADA", "wrong password");
    REQUIRE_FALSE(wrong.ok());
    CHECK(wrong.error().code == ErrorCode::AccessDenied);
    CHECK(p.client().login("nobody", "whatever1").error().message == wrong.error().message);
    auto again = p.client();
    REQUIRE(again.login("ADA", "correct horse").ok()); // names ignore case
    REQUIRE(again.logout().ok());
    again.setToken(reg->token == again.token() ? "" : reg->token);
    CHECK(ada.me().ok()); // other sessions stay valid
    PlatformClient stale = p.client();
    stale.setToken("made-up-token");
    CHECK(stale.me().error().code == ErrorCode::AccessDenied);
}

TEST_CASE("Platform: publishing checks the game, versions it and lists it") {
    TestPlatform p;
    auto ada = p.account("Ada");
    auto bo = p.account("Bob");
    auto game = ada.createGame("Coin Run", "Collect every coin.", Visibility::Public);
    REQUIRE(game.ok());
    CHECK(game->latestVersion == 0);
    // Not in the catalog until something is published.
    CHECK(p.client().games()->empty());

    const auto package = pxtest::examplePackage("platformer");
    auto v1 = ada.publish(game->id, package, "first");
    REQUIRE_MESSAGE(v1.ok(), (v1.ok() ? "" : v1.error().message));
    CHECK(v1->number == 1);
    CHECK(v1->packageHash.size() == 64);
    CHECK(v1->scriptErrors == 0);
    auto v2 = ada.publish(game->id, package, "second");
    REQUIRE(v2.ok());
    CHECK(v2->number == 2);
    CHECK(v2->packageHash == v1->packageHash); // same content, same package

    auto listed = p.client().games();
    REQUIRE(listed.ok());
    REQUIRE(listed->size() == 1);
    CHECK(listed->front().title == "Coin Run");
    CHECK(listed->front().ownerName == "Ada");
    CHECK(listed->front().gameType == "platformer");
    CHECK(listed->front().latestVersion == 2);
    CHECK(p.client().games({"coin"})->size() == 1);
    CHECK(p.client().games({"every"})->size() == 1);
    CHECK(p.client().games({"zzz"})->empty());
    CHECK(p.client().games({"%"})->empty()); // LIKE wildcards are literal

    // Anyone can download it; it is the checked, re-encoded package.
    auto bytes = p.client().downloadPackage(listed->front().packageHash);
    REQUIRE(bytes.ok());
    auto decoded = project::decodePackage(bytes.value());
    REQUIRE(decoded.ok());
    auto fs = decoded->toFileSystem();
    CHECK(runtime::loadGame(*fs).ok());
    CHECK_FALSE(p.client().downloadPackage(std::string(64, '0')).ok());

    // Only the owner publishes and sees versions.
    CHECK(bo.publish(game->id, package).error().code == ErrorCode::NotFound);
    CHECK_FALSE(bo.versions(game->id).ok());
    auto versions = ada.versions(game->id);
    REQUIRE(versions.ok());
    CHECK(versions->size() == 2);
    CHECK(versions->front().notes == "second");

    // A broken game is refused with the checker's message.
    auto broken = project::collectPackage(std::filesystem::path(PX_EXAMPLES_DIR) / "platformer");
    REQUIRE(broken.ok());
    for (auto& f : broken->files) {
        if (f.path == "scripts/coin.luau") f.data = {'r', 'e', 't', 'u', 'r', 'n', ' ', '('};
    }
    auto refused = ada.publish(game->id, project::encodePackage(broken.value()));
    REQUIRE_FALSE(refused.ok());
    CHECK(contains(refused.error().message, "not accepted"));
    CHECK(contains(refused.error().message, "coin.luau"));
    std::vector<std::uint8_t> garbage(100, 7);
    CHECK(contains(ada.publish(game->id, garbage).error().message, "not a game package"));

    // Settings.
    GameUpdate update;
    update.title = "Coin Run 2";
    update.maxPlayers = 4;
    auto updated = ada.updateGame(game->id, update);
    REQUIRE(updated.ok());
    CHECK(updated->title == "Coin Run 2");
    CHECK(updated->maxPlayers == 4);
    update.maxPlayers = 99;
    CHECK_FALSE(ada.updateGame(game->id, update).ok());
    GameUpdate steal;
    steal.title = "Mine";
    CHECK_FALSE(bo.updateGame(game->id, steal).ok());
    CHECK(contains(ada.createGame("Bad\ntitle", "", Visibility::Public).error().message, "line breaks"));
    CHECK(ada.myGames()->size() == 1);
}

TEST_CASE("Platform: private and unlisted games") {
    TestPlatform p;
    auto ada = p.account("Ada");
    auto bo = p.account("Bob");
    const auto package = pxtest::examplePackage("topdown");
    auto secret = ada.createGame("Secret", "", Visibility::Private);
    auto unlisted = ada.createGame("Unlisted", "", Visibility::Unlisted);
    REQUIRE(secret.ok());
    REQUIRE(unlisted.ok());
    auto v = ada.publish(secret->id, package);
    REQUIRE(v.ok());
    REQUIRE(ada.publish(unlisted->id, package).ok());
    CHECK(p.client().games()->empty());
    CHECK(bo.game(secret->id).error().code == ErrorCode::NotFound);
    CHECK(bo.game(unlisted->id).ok());
    CHECK(ada.game(secret->id).ok());
    CHECK(bo.join(secret->id).error().code == ErrorCode::NotFound);
    // The package of a private game is not downloadable by others.
    CHECK(p.client().downloadPackage(v->packageHash).ok()); // it is also the unlisted game's package
}

TEST_CASE("Platform: reports, moderation and the audit log") {
    TestPlatform p;
    auto ada = p.account("Ada");
    auto bo = p.account("Bob");
    auto mod = p.account("Mod");
    REQUIRE(p.service->setRole("mod", Role::Admin).ok());
    REQUIRE(mod.me()->role == Role::Admin);

    auto game = ada.createGame("Rude Game", "", Visibility::Public);
    REQUIRE(game.ok());
    REQUIRE(ada.publish(game->id, pxtest::examplePackage("platformer")).ok());
    REQUIRE(bo.report(game->id, "Offensive text in the level").ok());
    CHECK_FALSE(bo.report(game->id, "").ok());
    CHECK(bo.reports().error().code == ErrorCode::AccessDenied);

    auto reports = mod.reports();
    REQUIRE(reports.ok());
    REQUIRE(reports->size() == 1);
    CHECK(reports->front().reporter == "Bob");
    CHECK(reports->front().gameTitle == "Rude Game");
    REQUIRE(mod.resolveReport(reports->front().id, "hide_game", "offensive").ok());
    CHECK(mod.reports()->empty());
    CHECK(mod.reports(false)->size() == 1);

    // Hidden: gone from the catalog and unplayable; the owner sees why.
    CHECK(p.client().games()->empty());
    CHECK(bo.game(game->id).error().code == ErrorCode::NotFound);
    auto own = ada.game(game->id);
    REQUIRE(own.ok());
    CHECK(own->hidden);
    CHECK(own->moderationNote == "offensive");
    CHECK(contains(ada.join(game->id).error().message, "taken down"));
    REQUIRE(mod.setGameHidden(game->id, false, "appeal accepted").ok());
    CHECK(p.client().games()->size() == 1);

    // Banning: sessions end, logging in is refused, the games disappear.
    REQUIRE(mod.setUserBanned("ada", true, "repeated abuse").ok());
    CHECK(contains(ada.me().error().message, "Log in"));
    CHECK(contains(p.client().login("Ada", "password-Ada").error().message, "banned"));
    CHECK(p.client().games()->empty());
    CHECK_FALSE(mod.setUserBanned("mod", true, "").ok()); // not yourself
    REQUIRE(mod.setUserBanned("ada", false, "").ok());
    CHECK(p.client().login("Ada", "password-Ada").ok());

    auto audit = mod.audit();
    REQUIRE(audit.ok());
    REQUIRE(audit->size() >= 5);
    CHECK(audit->front().action == "unban_user");
    CHECK(audit->front().actor == "Mod");
    bool sawHide = false;
    for (const auto& e : audit.value()) sawHide = sawHide || e.action == "hide_game";
    CHECK(sawHide);
    CHECK(bo.audit().error().code == ErrorCode::AccessDenied);
}

#if defined(PX_HAVE_TLS)
#include "Certificates.hpp"

TEST_CASE("Platform: HTTPS with a development certificate") {
    const auto certDir = pxtest::tempDir("tls");
    const auto cert = certDir / "cert.pem";
    const auto key = certDir / "key.pem";
    REQUIRE(platform::makeDevCertificate({"localhost", "127.0.0.1"}, cert, key).ok());
    TestPlatform p(false, 0, [&](platform::ServiceConfig& c) {
        c.httpsPort = -1; // any free port
        c.tlsCert = cert;
        c.tlsKey = key;
    });
    REQUIRE(p.service->httpsPort() > 0);
    CHECK(p.service->httpsUrl().starts_with("https://127.0.0.1:"));

    // Trusting the certificate: everything works over HTTPS.
    PlatformClient secure(p.service->httpsUrl());
    secure.setCaFile(cert.string());
    auto info = secure.info();
    REQUIRE_MESSAGE(info.ok(), (info.ok() ? "" : info.error().message));
    auto reg = secure.registerAccount("Ada", "correct horse");
    REQUIRE(reg.ok());
    CHECK(secure.me().ok());
    // The same account over plain HTTP (game servers use it on this computer).
    CHECK(p.client().login("Ada", "correct horse").ok());

    // Not trusting it: refused, with a message saying how to trust it.
    PlatformClient untrusted(p.service->httpsUrl());
    untrusted.setCaFile("");
    auto refused = untrusted.info();
    REQUIRE_FALSE(refused.ok());
    CHECK(contains(refused.error().message, "does not trust"));
    // A certificate for another name is refused too.
    PlatformClient wrongName(std::format("https://localhost.localdomain:{}", p.service->httpsPort()));
    wrongName.setCaFile(cert.string());
    CHECK_FALSE(wrongName.info().ok());
    std::filesystem::remove_all(certDir);
}
#endif

TEST_CASE("Platform: the browser page and player files") {
    // Without a web player: the page says so, the player files are missing.
    {
        TestPlatform p;
        auto info = p.client().info();
        REQUIRE(info.ok());
        CHECK_FALSE(info->webPlayer);
        httplib::Client http(p.service->url());
        auto page = http.Get("/play");
        REQUIRE(page);
        CHECK(page->status == 200);
        CHECK(page->get_header_value("Content-Type") == "text/html; charset=utf-8");
        CHECK(contains(page->get_header_value("Content-Security-Policy"), "script-src 'self' 'wasm-unsafe-eval'"));
        CHECK(contains(page->get_header_value("Content-Security-Policy"), "frame-ancestors 'none'"));
        CHECK(page->get_header_value("X-Frame-Options") == "DENY");
        CHECK(page->get_header_value("X-Content-Type-Options") == "nosniff");
        CHECK(contains(page->body, "<script src=\"/play/play.js\" defer>"));
        auto script = http.Get("/play/play.js");
        REQUIRE(script);
        CHECK(script->status == 200);
        CHECK(script->get_header_value("Content-Type") == "text/javascript; charset=utf-8");
        CHECK(contains(script->body, "noUrlArguments: true"));
        CHECK(http.Get("/play/play.css")->status == 200);
        CHECK(http.Get("/play/other.js")->status == 404);
        CHECK(http.Get("/play/player/px-player.js")->status == 404);
        CHECK(http.Get("/api/v1/games")->get_header_value("X-Content-Type-Options") == "nosniff");
    }
    // With one: served with an ETag; only the player's own files.
    const auto web = pxtest::tempDir("web");
    {
        std::ofstream(web / "px-player.js") << "console.log('player');";
        std::ofstream(web / "px-player.wasm", std::ios::binary) << std::string("\0asm\1\0\0\0", 8);
        std::ofstream(web / "px-player.data", std::ios::binary) << "data";
        std::ofstream(web / "secret.txt") << "not served";
    }
    TestPlatform p(false, 0, [&](platform::ServiceConfig& c) { c.webPlayerDir = web; });
    auto info = p.client().info();
    REQUIRE(info.ok());
    CHECK(info->webPlayer);
    httplib::Client http(p.service->url());
    auto js = http.Get("/play/player/px-player.js");
    REQUIRE(js);
    CHECK(js->status == 200);
    CHECK(js->body == "console.log('player');");
    CHECK(js->get_header_value("Content-Type") == "text/javascript; charset=utf-8");
    const std::string etag = js->get_header_value("ETag");
    CHECK_FALSE(etag.empty());
    auto again = http.Get("/play/player/px-player.js", httplib::Headers{{"If-None-Match", etag}});
    REQUIRE(again);
    CHECK(again->status == 304);
    CHECK(again->body.empty());
    auto wasm = http.Get("/play/player/px-player.wasm");
    REQUIRE(wasm);
    CHECK(wasm->get_header_value("Content-Type") == "application/wasm");
    CHECK(wasm->body.size() == 8);
    CHECK(http.Get("/play/player/px-player.data")->body == "data");
    CHECK(http.Get("/play/player/secret.txt")->status == 404);
    CHECK(http.Get("/play/player/..%2Fsecret.txt")->status == 404);
    auto home = http.Get("/");
    REQUIRE(home);
    CHECK(contains(home->body, "href=\"/play\""));
    std::filesystem::remove_all(web);
}

TEST_CASE("Platform: rate limits and unknown game servers") {
    TestPlatform p(true);
    auto ada = p.account("Ada");
    int refused = 0;
    for (int i = 0; i < 15; ++i) {
        auto r = p.client().login("Ada", "wrong");
        if (!r.ok() && r.error().code == ErrorCode::LimitExceeded) ++refused;
    }
    CHECK(refused >= 4); // 10 attempts per name per minute
    auto server = p.client();
    server.setServerKey("not-a-real-key");
    CHECK(server.serverHeartbeat({}).error().code == ErrorCode::AccessDenied);
    CHECK(server.serverLoadData("1").error().code == ErrorCode::AccessDenied);
}

// --- Joining games: real game servers (px-server processes) ----------------------------------

using pxtest::eventually;
using pxtest::OnlinePlayer;

TEST_CASE("Platform: joining starts a game server; players play there and their data is saved") {
    TestPlatform p;
    auto ada = p.account("Ada");
    auto bob = p.account("Bob");
    auto game = ada.createGame("Coin Run", "", Visibility::Public);
    REQUIRE(game.ok());
    REQUIRE(ada.publish(game->id, pxtest::examplePackage("platformer")).ok());
    auto other = ada.createGame("Garden", "", Visibility::Public);
    REQUIRE(other.ok());
    REQUIRE(ada.publish(other->id, pxtest::examplePackage("topdown")).ok());

    auto ticket = ada.join(game->id);
    REQUIRE_MESSAGE(ticket.ok(), (ticket.ok() ? "" : ticket.error().message));
    CHECK(ticket->host == "127.0.0.1");
    CHECK(ticket->version == 1);
    REQUIRE(p.service->fleet().sessions().size() == 1);
    CHECK(p.service->fleet().sessions().front().state == "ready");

    OnlinePlayer adaPlays(ada, ticket.value());
    REQUIRE(adaPlays.waitUntilPlaying());
    adaPlays.play(0.5);
    REQUIRE_FALSE(adaPlays.client->game().players.empty());
    CHECK(adaPlays.client->game().players.front().name == "Ada");

    // Bob gets a seat on the same server.
    auto bobTicket = bob.join(game->id);
    REQUIRE(bobTicket.ok());
    CHECK(bobTicket->session == ticket->session);
    // Bob joins the way a browser does: over the ticket's WebSocket address.
    CHECK(bobTicket->webSocket == std::format("ws://127.0.0.1:{}/play", bobTicket->port));
    OnlinePlayer bobPlays(bob, bobTicket.value(), true);
    REQUIRE(bobPlays.waitUntilPlaying());
    CHECK(eventually([&] { return p.service->fleet().playing(game->id) == 2; }, {&adaPlays, &bobPlays}));
    CHECK(eventually([&] { return p.client().game(game->id)->playing == 2; }, {&adaPlays, &bobPlays}));
    CHECK(p.client().game(game->id)->plays == 2);

    // One game at a time: Ada cannot be in two games (her data is held by this one).
    auto twice = ada.join(other->id);
    REQUIRE_FALSE(twice.ok());
    CHECK(contains(twice.error().message, "another game"));

    // A stolen ticket does not work for someone else: tokens are single use.
    OnlinePlayer thief(bob, ticket.value());
    CHECK(eventually([&] { return thief.client->status() == net::GameClient::Status::Disconnected; },
                     {&thief, &adaPlays, &bobPlays}));
    CHECK(thief.client->status() == net::GameClient::Status::Disconnected);

    // Ada runs right into the first coin; when she leaves, her data is saved.
    adaPlays.play(2.0, {"move_right"});
    CHECK(adaPlays.client->game().score >= 1);
    adaPlays.client->disconnect();
    std::string saved;
    CHECK(eventually(
        [&] {
            auto data = ada.playerData(game->id, "Ada");
            saved = data ? data.value() : "";
            return contains(saved, "\"coins\"");
        },
        {&bobPlays}));
    CHECK(contains(saved, "\"coins\":1"));
    CHECK_FALSE(bob.playerData(game->id, "Ada").ok()); // only the owner looks at players' data

    // Released: Ada can play the other game now, and her data stays per game.
    CHECK(eventually([&] { return ada.join(other->id).ok(); }, {&bobPlays}));

    // Moderation stops a game's servers.
    auto mod = p.account("Mod");
    REQUIRE(p.service->setRole("Mod", Role::Admin).ok());
    REQUIRE(mod.setGameHidden(game->id, true, "test").ok());
    bobPlays.play(8.0);
    CHECK(bobPlays.client->status() == net::GameClient::Status::Disconnected);
}
