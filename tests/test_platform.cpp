// The platform service through its HTTP API: accounts, games, publishing,
// the catalog and moderation (docs/platform.md).
#include "PlatformHarness.hpp"
#include "px/crypto/Crypto.hpp"
#include "px/crypto/Encoding.hpp"
#include "px/runtime/GameLoader.hpp"

#include <doctest.h>

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
