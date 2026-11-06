#pragma once

#include "PlayerApp.hpp"
#include "px/online/PlatformClient.hpp"
#include "px/platform_sdl/SdlPlatform.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

union SDL_Event;

// The launcher (docs/launcher.md): what px-player shows when it is started
// without a game. Log in to a platform, browse its games and their
// leaderboards, see friends and invitations, play; leaving a game comes back
// here. Games on this device (the bundled examples, or the game that comes
// with a phone app) play without a platform. Dear ImGui draws it, in the
// window the games then use.
namespace px::player {

class Launcher {
public:
    enum class Status { Continue, Quit, Failed, Play };

    // `base`: the command line (platform address, login, test options).
    [[nodiscard]] static Result<std::unique_ptr<Launcher>> create(const PlayerOptions& base);
    ~Launcher();
    Launcher(const Launcher&) = delete;
    Launcher& operator=(const Launcher&) = delete;

    [[nodiscard]] Status iterate();
    [[nodiscard]] Status handleEvent(const SDL_Event& event);

    // After Status::Play: what to play, and the window to play it in.
    [[nodiscard]] PlayerOptions takeGame();
    [[nodiscard]] std::unique_ptr<platform::SdlPlatform> takeWindow();
    // The game ended (or could not start): the window comes back. `failed`:
    // `message` is an error.
    void resume(std::unique_ptr<platform::SdlPlatform> window, const std::string& message, bool failed);

private:
    explicit Launcher(PlayerOptions base);
    [[nodiscard]] px::Status initialize();

    enum class Page { Login, Games, Game, Friends, You };
    using Apply = std::function<void(Launcher&)>;
    struct Job {
        std::future<Apply> result;
        int session = 0;       // results of an earlier login are dropped
        bool blocking = false; // "Working..." and actions wait
    };
    // A request on a worker thread with a copy of the logged-in client; the
    // function it returns runs on the UI thread.
    void run(std::function<Apply(online::PlatformClient&)> work, bool blocking = true);
    void applyFinishedJobs();
    [[nodiscard]] bool busy() const;
    [[nodiscard]] online::PlatformClient client() const;
    void fail(std::string message);
    void notice(std::string message);

    // --- actions ------------------------------------------------------------------------------
    void tryLogin(bool registerAccount);
    void restoreLogin(const online::SavedLogin& saved);
    void loggedIn(const online::UserInfo& user, const std::string& token, const std::string& url, bool remember);
    void logOut();
    void openStartPage();
    void openPage(Page page);
    void openGame(std::int64_t id);
    void refreshGames();
    void refreshSocial();
    void refreshGamePage();
    void refreshProfile();
    void saveProfile();
    void searchPeople();
    void refreshAccount();
    void saveEmail();
    void changePassword();
    void requestPasswordReset();
    void deleteAccount();
    void endLogin(); // logged out here: the saved login goes too
    void requestFriend(const std::string& name);
    void play(std::int64_t gameId, const std::string& title);
    void joinFriend(const std::string& name, const std::string& title);
    void playLocal(std::size_t index);
    [[nodiscard]] PlayerOptions gameOptions(const std::string& title);
    void requestPicture(std::int64_t gameId, const std::string& hash);
    void clearPictures();

    // --- drawing ------------------------------------------------------------------------------
    void drawFrame();
    void drawTopBar();
    void drawLogin();
    void drawLocalGames();
    void drawInvitations();
    void drawGames();
    void drawGamePage();
    void drawLeaderboard(const online::LeaderboardView& board);
    void drawFriends();
    void drawPersonMenu(const std::string& name, bool isFriend);
    void drawYou();
    void drawReportPopup();
    void drawAccount();
    void drawDeletePopup();
    void drawMessages();
    void avatar(int index, float size);
    [[nodiscard]] bool gameCard(const online::GameInfo& game, float width);
    void picture(std::int64_t gameId, const std::string& hash, const std::string& label, float width);
    void touchScroll();
    [[nodiscard]] bool tapped() const; // the last click was not the end of a drag-scroll
    [[nodiscard]] float px(float logical) const { return logical * m_scale; }

    PlayerOptions m_base;
    bool m_test = false;          // --frames: draw that many settled frames, then quit
    bool m_testGame = false;      // --open play:/local:: the game also runs --frames
    std::unique_ptr<platform::SdlPlatform> m_window;
    std::filesystem::path m_dataDir; // login and package cache
    bool m_imgui = false;
    float m_scale = 1.0f;
    int m_frame = 0;
    double m_startedAt = 0.0;
    double m_frameStart = 0.0;
    bool m_quit = false;
    bool m_dragged = false;
    bool m_scrollTop = false;
    std::optional<PlayerOptions> m_launch;
    std::string m_pendingOpen; // --open, once logged in

    // Login
    std::string m_url;         // as typed
    std::string m_platformUrl; // of the current login
    std::string m_username;
    std::string m_password;
    bool m_remember = true;
    std::string m_token;
    std::optional<online::UserInfo> m_me;
    int m_session = 0;

    Page m_page = Page::Login;
    std::vector<Job> m_jobs;
    std::string m_error;
    std::string m_notice;

    // Games
    std::vector<online::GameInfo> m_games;
    std::string m_search;
    int m_sort = 0;
    bool m_gamesLoading = false;
    bool m_gamesLoaded = false;
    double m_gamesAt = -1.0e9;
    std::int64_t m_gameId = 0;
    std::optional<online::GameInfo> m_game;
    std::vector<online::LeaderboardView> m_boards;
    bool m_boardsLoaded = false;
    bool m_friendsBoards = false; // leaderboards: friends only

    // People
    online::SocialSummary m_social;
    bool m_socialLoading = false;
    double m_socialAt = -1.0e9;
    std::string m_addFriend;
    std::string m_searched;
    double m_searchAt = 0.0;
    std::vector<online::PersonInfo> m_suggestions;
    std::optional<online::Profile> m_profile;
    std::string m_bio;
    int m_avatar = 0;
    int m_joinPolicy = 0; // 0 friends, 1 nobody (online::JoinPolicy)
    std::vector<online::PersonInfo> m_blocked;
    std::string m_reportName; // the report popup is open for this account
    std::string m_reportReason;

    // Your account (docs/accounts.md)
    std::optional<online::AccountSettings> m_account;
    std::string m_email;
    std::string m_emailPassword;   // confirms a new address
    std::string m_currentPassword; // changing the password
    std::string m_newPassword;
    bool m_forgot = false;         // the login page shows "forgot your password?"
    std::string m_forgotName;
    bool m_deleting = false;       // the delete popup is open
    int m_scrollToAccount = 0;      // --open account: frames left to scroll there
    float m_accountTop = -1.0f;     // where the account settings start (screen y), this frame
    std::string m_deletePassword;

    std::vector<DeviceGame> m_local;

    // Game pictures by game id, in the window's renderer.
    struct Picture {
        std::string hash;
        unsigned int texture = 0; // 0: none
        bool loading = false;
    };
    std::map<std::int64_t, Picture> m_pictures;
    unsigned int m_avatars = 0; // the 4x4 sheet of portraits
};

} // namespace px::player
