// People on the platform (docs/social.md), leaderboards (docs/leaderboards.md)
// and thumbnails, through the HTTP API with real game servers.
#include "PlatformHarness.hpp"
#include "px/assets/Image.hpp"
#include "px/core/FileSystem.hpp"
#include "px/online/Thumbnail.hpp"

#include <doctest.h>
#include <httplib.h>

#include <fstream>

using namespace px;
using namespace px::online;
using pxtest::contains;
using pxtest::eventually;
using pxtest::OnlinePlayer;
using pxtest::TestPlatform;

namespace {

// A tiny game: each player puts a score on two leaderboards as they join,
// and the HUD shows the top two of "score" (read from the server's copy).
std::vector<std::uint8_t> boardGamePackage() {
    const auto dir = pxtest::tempDir("boardgame");
    std::filesystem::create_directories(dir / "scenes");
    std::filesystem::create_directories(dir / "scripts");
    std::ofstream(dir / "project.json") << R"({
  "format": "px.project", "formatVersion": 2, "id": "aaaaaaaa-0000-4000-8000-0000000000b0", "name": "Board Game",
  "startScene": "aaaaaaaa-0000-4000-8000-0000000000b1",
  "settings": {"gameType": "topdown"},
  "input": {"actions": {"move_left": ["Left"], "move_right": ["Right"]}},
  "leaderboards": [{"id": "score", "title": "Score"},
                   {"id": "time", "title": "Time", "order": "lowest", "format": "time"}],
  "assets": [
    {"id": "aaaaaaaa-0000-4000-8000-0000000000b1", "type": "scene", "path": "scenes/board.scene.json"},
    {"id": "aaaaaaaa-0000-4000-8000-0000000000b2", "type": "script", "path": "scripts/board.luau"}
  ]
})";
    std::ofstream(dir / "scenes" / "board.scene.json") << R"({
  "format": "px.scene", "formatVersion": 3, "name": "Board",
  "entities": [
    {"id": "aaaaaaaa-0000-4000-8000-0000000000c1", "name": "Player",
     "components": {"Transform": {"position": [40, 40]}, "BoxCollider": {"size": [10, 10]}, "KinematicBody": {},
                    "TopDownController": {}, "Player": {}}},
    {"id": "aaaaaaaa-0000-4000-8000-0000000000c2", "name": "Rules",
     "components": {"Scripts": [{"script": "aaaaaaaa-0000-4000-8000-0000000000b2"}]}}
  ]
})";
    std::ofstream(dir / "scripts" / "board.luau") << R"lua(
local Board = {}
function Board:PlayerJoined(player)
    local score = player.playerName == "Ada" and 42 or 7
    Leaderboard.submit(player, "score", score)
    Leaderboard.submit(player, "time", score / 2)
end
function Board:Tick()
    local parts = {}
    for _, row in Leaderboard.top("score", 2) do
        table.insert(parts, row.name .. " " .. row.score)
    end
    Game.setObjective(table.concat(parts, ", "))
end
return Board
)lua";
    auto package = project::collectPackage(dir);
    REQUIRE_MESSAGE(package.ok(), (package.ok() ? "" : package.error().message));
    std::filesystem::remove_all(dir);
    return project::encodePackage(package.value());
}

const FriendInfo* friendNamed(const SocialSummary& s, std::string_view name) {
    for (const auto& f : s.friends) {
        if (f.username == name) return &f;
    }
    return nullptr;
}

} // namespace

TEST_CASE("Social: profiles, search and what others see") {
    TestPlatform p;
    auto ada = p.account("Ada");
    auto bob = p.account("Bob");
    ProfileUpdate update;
    update.bio = "I make\nplatformers";
    update.avatar = 5;
    update.joinPolicy = JoinPolicy::Nobody;
    auto mine = ada.updateProfile(update);
    REQUIRE_MESSAGE(mine.ok(), (mine.ok() ? "" : mine.error().message));
    CHECK(mine->relation == "self");
    CHECK(mine->avatar == 5);
    CHECK(mine->joinPolicy == JoinPolicy::Nobody);
    REQUIRE(mine->presence.has_value());
    CHECK(mine->presence->state == "online");

    auto seen = bob.profile("ada"); // names ignore case
    REQUIRE(seen.ok());
    CHECK(seen->user.username == "Ada");
    CHECK(seen->bio == "I make\nplatformers");
    CHECK(seen->avatar == 5);
    CHECK(seen->relation == "none");
    CHECK_FALSE(seen->presence.has_value()); // only friends see presence
    auto anonymous = p.client().profile("Ada");
    REQUIRE(anonymous.ok());
    CHECK(anonymous->relation == "none");

    ProfileUpdate bad;
    bad.avatar = kAvatarCount;
    CHECK_FALSE(ada.updateProfile(bad).ok());
    ProfileUpdate longBio;
    longBio.bio = std::string(kMaxBioLength + 1, 'x');
    CHECK_FALSE(ada.updateProfile(longBio).ok());
    ProfileUpdate unicode;
    unicode.bio = std::string(kMaxBioLength / 2, 'x') + "\xc3\xa5\xc3\xa4\xc3\xb6"; // åäö count as 3 characters
    CHECK(ada.updateProfile(unicode).ok());
    ProfileUpdate control;
    control.bio = std::string("bell\x07");
    CHECK_FALSE(ada.updateProfile(control).ok());

    (void)p.account("Adalind");
    auto found = bob.searchUsers("ada");
    REQUIRE(found.ok());
    REQUIRE(found->size() == 2);
    CHECK(found->at(0).username == "Ada");
    CHECK(found->at(1).username == "Adalind");
    CHECK_FALSE(bob.searchUsers("a").ok()); // two letters at least
    CHECK(bob.searchUsers("a%").value().empty()); // wildcards are letters
    CHECK_FALSE(bob.profile("Nobody").ok());
}

TEST_CASE("Social: friend requests, accepting, declining and removing") {
    TestPlatform p;
    auto ada = p.account("Ada");
    auto bob = p.account("Bob");
    auto cy = p.account("Cyd");

    CHECK(ada.requestFriend("Bob").value() == "requested");
    CHECK(ada.requestFriend("Bob").value() == "requested"); // asking twice changes nothing
    CHECK_FALSE(ada.requestFriend("Ada").ok());
    CHECK_FALSE(ada.requestFriend("Ghost").ok());
    auto bobs = bob.social();
    REQUIRE(bobs.ok());
    REQUIRE(bobs->incoming.size() == 1);
    CHECK(bobs->incoming[0].username == "Ada");
    CHECK(ada.social()->outgoing.size() == 1);
    CHECK(bob.profile("Ada")->relation == "incoming");
    CHECK(ada.profile("Bob")->relation == "requested");

    REQUIRE(bob.acceptFriend("Ada").ok());
    auto adas = ada.social();
    REQUIRE(adas.ok());
    REQUIRE(adas->friends.size() == 1);
    CHECK(adas->friends[0].username == "Bob");
    CHECK(adas->friends[0].presence.state == "online"); // Bob was just here
    CHECK(adas->outgoing.empty());
    CHECK(bob.profile("Ada")->relation == "friend");
    CHECK(bob.profile("Ada")->presence.has_value());
    CHECK(ada.profile("Ada")->friends == 1);
    CHECK_FALSE(bob.acceptFriend("Cyd").ok()); // Cy never asked

    // Asking someone who already asked you makes you friends at once.
    CHECK(cy.requestFriend("Ada").value() == "requested");
    CHECK(ada.requestFriend("Cyd").value() == "friends");
    CHECK(ada.social()->friends.size() == 2);

    // Declining and taking back.
    auto dee = p.account("Dee");
    CHECK(dee.requestFriend("Bob").value() == "requested");
    REQUIRE(bob.removeFriendRequest("Dee").ok());
    CHECK(bob.social()->incoming.empty());
    CHECK(dee.social()->outgoing.empty());

    REQUIRE(ada.removeFriend("Bob").ok());
    CHECK(friendNamed(ada.social().value(), "Bob") == nullptr);
    CHECK(friendNamed(bob.social().value(), "Ada") == nullptr);
}

TEST_CASE("Social: blocking ends friendships and stops requests") {
    TestPlatform p;
    auto ada = p.account("Ada");
    auto eve = p.account("Eve");
    CHECK(eve.requestFriend("Ada").value() == "requested");
    CHECK(ada.requestFriend("Eve").value() == "friends");
    REQUIRE(ada.block("Eve").ok());
    CHECK(ada.social()->friends.empty());
    CHECK(eve.social()->friends.empty());
    auto again = eve.requestFriend("Ada");
    REQUIRE_FALSE(again.ok());
    CHECK(ada.requestFriend("Eve").error().message.find("Unblock") != std::string::npos);
    CHECK(ada.profile("Eve")->relation == "blocked");
    REQUIRE(ada.blocked()->size() == 1);
    CHECK_FALSE(ada.block("Ada").ok());
    REQUIRE(ada.unblock("Eve").ok());
    CHECK(ada.blocked()->empty());
    CHECK(eve.requestFriend("Ada").value() == "requested");

    // Reports about accounts reach administrators, who can clear a profile.
    ProfileUpdate rude;
    rude.bio = "something rude";
    REQUIRE(eve.updateProfile(rude).ok());
    REQUIRE(ada.reportUser("Eve", "rude bio").ok());
    CHECK_FALSE(ada.reportUser("Ada", "me").ok());
    auto mod = p.account("Mod");
    REQUIRE(p.service->setRole("Mod", Role::Admin).ok());
    CHECK_FALSE(ada.userReports().ok()); // admins only
    auto reports = mod.userReports();
    REQUIRE(reports.ok());
    REQUIRE(reports->size() == 1);
    CHECK(reports->at(0).username == "Eve");
    CHECK(reports->at(0).reporter == "Ada");
    REQUIRE(mod.resolveUserReport(reports->at(0).id, "clear_profile", "please be kind").ok());
    CHECK(ada.profile("Eve")->bio.empty());
    CHECK(mod.userReports()->empty());
    auto audit = mod.audit();
    REQUIRE(audit.ok());
    bool cleared = false;
    for (const auto& a : audit.value()) cleared = cleared || a.action == "clear_profile";
    CHECK(cleared);
}

TEST_CASE("Social: presence, joining friends, invitations and keeping blocked players apart") {
    TestPlatform p;
    auto ada = p.account("Ada");
    auto bob = p.account("Bob");
    auto cy = p.account("Cyd");
    auto dee = p.account("Dee");
    auto eve = p.account("Eve");
    for (auto* other : {&bob, &dee}) {
        REQUIRE(ada.requestFriend(other == &bob ? "Bob" : "Dee").ok());
        REQUIRE(other->acceptFriend("Ada").ok());
    }
    auto game = ada.createGame("Boards", "", Visibility::Public);
    REQUIRE(game.ok());
    REQUIRE(ada.publish(game->id, boardGamePackage()).ok());

    CHECK_FALSE(bob.joinFriend("Ada").ok()); // Ada is not playing yet
    auto ticket = ada.join(game->id);
    REQUIRE(ticket.ok());
    OnlinePlayer adaPlays(ada, ticket.value());
    REQUIRE(adaPlays.waitUntilPlaying());

    // Bob sees Ada playing and joins her server.
    SocialSummary bobSees;
    REQUIRE(eventually(
        [&] {
            if (auto s = bob.social()) bobSees = *s;
            const FriendInfo* f = friendNamed(bobSees, "Ada");
            return f != nullptr && f->presence.state == "playing";
        },
        {&adaPlays}));
    const FriendInfo* seen = friendNamed(bobSees, "Ada");
    CHECK(seen->presence.gameId == game->id);
    CHECK(seen->presence.gameTitle == "Boards");
    CHECK(seen->presence.joinable);
    auto bobTicket = bob.joinFriend("Ada");
    REQUIRE_MESSAGE(bobTicket.ok(), (bobTicket.ok() ? "" : bobTicket.error().message));
    CHECK(bobTicket->session == ticket->session);
    OnlinePlayer bobPlays(bob, bobTicket.value());
    REQUIRE(bobPlays.waitUntilPlaying());

    // Not a friend: no.
    auto cyTry = cy.joinFriend("Ada");
    REQUIRE_FALSE(cyTry.ok());
    CHECK(contains(cyTry.error().message, "not your friend"));

    // Ada lets friends join only when she invites them.
    ProfileUpdate closed;
    closed.joinPolicy = JoinPolicy::Nobody;
    REQUIRE(ada.updateProfile(closed).ok());
    auto deeTry = dee.joinFriend("Ada");
    REQUIRE_FALSE(deeTry.ok());
    CHECK(contains(deeTry.error().message, "invited"));
    CHECK_FALSE(friendNamed(dee.social().value(), "Ada")->presence.joinable);
    CHECK_FALSE(cy.invite("Ada").ok()); // Cy is not playing and not her friend
    REQUIRE(ada.invite("Dee").ok());
    auto invited = dee.social();
    REQUIRE(invited.ok());
    REQUIRE(invited->invitations.size() == 1);
    CHECK(invited->invitations[0].from == "Ada");
    CHECK(invited->invitations[0].gameTitle == "Boards");
    CHECK(friendNamed(*invited, "Ada")->presence.joinable);
    auto deeTicket = dee.joinFriend("Ada");
    REQUIRE_MESSAGE(deeTicket.ok(), (deeTicket.ok() ? "" : deeTicket.error().message));
    CHECK(deeTicket->session == ticket->session);
    REQUIRE(dee.dismissInvitation(invited->invitations[0].id).ok());
    CHECK(dee.social()->invitations.empty());

    // Someone Ada blocked never gets a seat on her server.
    REQUIRE(ada.block("Eve").ok());
    auto eveTicket = eve.join(game->id);
    REQUIRE_MESSAGE(eveTicket.ok(), (eveTicket.ok() ? "" : eveTicket.error().message));
    CHECK(eveTicket->session != ticket->session);
    CHECK(p.service->fleet().sessions().size() == 2);

    // Leaderboards: Ada's 42 and Bob's 7 reach the platform from the server.
    std::vector<LeaderboardView> boards;
    REQUIRE(eventually(
        [&] {
            auto list = bob.leaderboards(game->id);
            if (list) boards = list.value();
            return boards.size() == 2 && boards[0].top.size() == 2;
        },
        {&adaPlays, &bobPlays}));
    const LeaderboardView& score = boards[0];
    CHECK(score.id == "score");
    CHECK(score.order == "highest");
    CHECK(score.top[0].username == "Ada");
    CHECK(score.top[0].value == doctest::Approx(42));
    CHECK(score.top[0].rank == 1);
    CHECK(score.top[1].username == "Bob");
    CHECK(score.top[1].rank == 2);
    REQUIRE(score.you.has_value());
    CHECK(score.you->username == "Bob");
    CHECK(score.you->rank == 2);
    CHECK(score.friends.size() == 2); // Bob and his friend Ada
    const LeaderboardView& time = boards[1];
    CHECK(time.format == "time");
    CHECK(time.top[0].username == "Bob"); // lowest is best: 3.5
    CHECK(time.top[0].value == doctest::Approx(3.5));
    auto anonymous = p.client().leaderboards(game->id);
    REQUIRE(anonymous.ok());
    CHECK_FALSE(anonymous->at(0).you.has_value());
    CHECK(anonymous->at(0).friends.empty());
    // The scripts read the server's copy.
    CHECK(eventually([&] { return adaPlays.client->game().objective == "Ada 42, Bob 7"; }, {&adaPlays, &bobPlays}));

    // Owners can take a score away; others cannot.
    CHECK_FALSE(bob.clearLeaderboard(game->id, "score", "Ada").ok());
    REQUIRE(ada.clearLeaderboard(game->id, "score", "Bob").ok());
    CHECK(ada.leaderboards(game->id)->at(0).top.size() == 1);
    REQUIRE(ada.clearLeaderboard(game->id, "time").ok());
    CHECK(ada.leaderboards(game->id)->at(1).top.empty());
}

TEST_CASE("Thumbnails: checked, made 320x180, served with an ETag") {
    TestPlatform p;
    auto ada = p.account("Ada");
    auto bob = p.account("Bob");
    auto game = ada.createGame("Pictures", "", Visibility::Public);
    REQUIRE(game.ok());
    auto none = p.client().thumbnail(game->id);
    REQUIRE(none.ok());
    CHECK(none->empty());

    auto picture = readLocalFile(std::filesystem::path(PX_EXAMPLES_DIR) / "topdown" / "assets" / "scenery.png");
    REQUIRE(picture.ok());
    REQUIRE_MESSAGE(ada.uploadThumbnail(game->id, picture.value()).ok(), "upload");
    CHECK_FALSE(bob.uploadThumbnail(game->id, picture.value()).ok()); // not his game
    const std::vector<std::uint8_t> garbage(1000, 0x42);
    auto refused = ada.uploadThumbnail(game->id, garbage);
    REQUIRE_FALSE(refused.ok());
    CHECK(contains(refused.error().message, "not accepted"));

    auto info = p.client().game(game->id);
    REQUIRE(info.ok());
    CHECK(info->thumbnail.size() == 32);
    auto png = p.client().thumbnail(game->id);
    REQUIRE(png.ok());
    auto image = assets::decodeImage(png.value(), "thumbnail");
    REQUIRE(image.ok());
    CHECK(image->width == kThumbnailWidth);
    CHECK(image->height == kThumbnailHeight);

    httplib::Client http(p.service->url());
    auto first = http.Get(std::format("/api/v1/games/{}/thumbnail", game->id));
    REQUIRE(first);
    CHECK(first->get_header_value("Content-Type") == "image/png");
    const std::string etag = first->get_header_value("ETag");
    auto again = http.Get(std::format("/api/v1/games/{}/thumbnail", game->id), httplib::Headers{{"If-None-Match", etag}});
    REQUIRE(again);
    CHECK(again->status == 304);
}
