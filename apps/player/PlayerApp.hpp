#pragma once

#include "PlayerSettings.hpp"
#include "px/core/FileSystem.hpp"
#include "px/core/Result.hpp"
#include "px/core/Time.hpp"
#include "px/net/GameClient.hpp"
#include "px/net/GameServer.hpp"
#include "px/platform_sdl/SdlPlatform.hpp"
#include "px/project/Project.hpp"
#include "px/render/Camera2D.hpp"
#include "px/render/TextureCache.hpp"
#include "px/runtime/GameLoader.hpp"
#include "px/runtime/GameSession.hpp"
#include "px/runtime/InputScript.hpp"
#include "px/runtime/TouchControls.hpp"
#include "px/scene/Scene.hpp"

#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

union SDL_Event;

namespace px::player {

struct PlayerOptions {
    std::filesystem::path projectDir;
    int frames = 0;                        // > 0: exit after this many frames (deterministic: 1 tick/frame)
    std::filesystem::path screenshotPath;  // saved after the last frame when frames > 0
    std::filesystem::path inputScriptPath; // play scripted input instead of the keyboard
    bool debugOverlay = false;
    int windowScale = 0;                   // 0: use the project's setting
    bool verbose = false;
    // Multiplayer (docs/multiplayer.md).
    std::string connectAddress;            // join this server ("host" or "host:port")
    bool host = false;                     // run a server in this window and join it
    int port = 0;                          // with --host: UDP port (0: the default)
    std::string playerName;                // shown to the other players
    std::optional<Vec2i> windowPosition;   // --window-pos X,Y (Studio tiles multiplayer test windows)
    // The platform (docs/platform.md#playing): join a published game.
    std::string platformUrl;               // --platform URL
    long long playGame = 0;                // --play ID
    std::string user;                      // --user NAME (else the login saved by Studio or the player)
    std::string password;                  // --password TEXT
    bool registerAccount = false;          // --register: create the account first
    bool touch = false;                    // --touch: show touch controls; the mouse acts as a finger
    std::string packagePath;               // --package FILE: play a game package (the browser page uses it)
    std::string token;                     // --token TEXT: connect token for --connect (from a ticket)
    // Started from the launcher: leaving the game returns to it. With a
    // platform login, the menu can invite friends (platformUrl + platformToken).
    bool fromLauncher = false;
    std::string platformToken;
    std::string gameTitle;                 // as the platform lists it (window title)
    bool launcher = false;                 // --launcher: open the launcher (also: no game given)
    std::string open;                      // --open game:ID|friends|you: the launcher's first page (tests)
    std::vector<std::string> invite;       // --invite NAME: invite these friends once in a platform game
};

// Whether these options start the launcher rather than a game: nothing to
// play was given (no project, package, server, platform game or --frames).
[[nodiscard]] bool wantsLauncher(const PlayerOptions& options);

// Games that come with this player and play without a platform: the
// bundled examples (desktop), or the game in the app (phones and tablets).
struct DeviceGame {
    std::string title;
    std::string gameType;
    std::filesystem::path projectDir; // or
    std::string packagePath;
};
[[nodiscard]] std::vector<DeviceGame> gamesOnThisDevice();

// Parses command-line arguments. Returns nullopt and sets `error` on failure.
[[nodiscard]] std::optional<PlayerOptions> parsePlayerArgs(int argc, char** argv, std::string& error);
[[nodiscard]] const char* playerUsage();

// The client application: owns the window, the running game session and the
// presentation (HUD, debug overlay). Driven by SDL's main callbacks.
class PlayerApp {
public:
    enum class Status { Continue, Success, Failure };

    // `window`: an open window to play in (the launcher's); taken only on success.
    [[nodiscard]] static Result<std::unique_ptr<PlayerApp>> create(
        PlayerOptions options, std::unique_ptr<platform::SdlPlatform>* window = nullptr);

    [[nodiscard]] Status iterate();
    [[nodiscard]] Status handleEvent(const SDL_Event& event);
    // Ends the game and gives the window back (to the launcher).
    [[nodiscard]] std::unique_ptr<platform::SdlPlatform> releaseWindow();
    // Why the game ended, for the launcher ("Disconnected: ..."), or empty.
    [[nodiscard]] std::string endMessage() const;

private:
    PlayerApp(PlayerOptions options, std::filesystem::path projectDir);

    // What the HUD shows, from a local session or from the server.
    struct HudState {
        std::string objective;
        bool levelComplete = false;
        std::string completionMessage;
        int scriptErrors = 0;
        std::string lastScriptError;
        int restartTicks = -1;
    };

    [[nodiscard]] px::Status restart();
    [[nodiscard]] px::Status startNetwork();
    // Logs in, asks the platform for a seat, downloads the game. Fills
    // m_files and the ticket fields.
    [[nodiscard]] px::Status preparePlatformGame();
    void reload();
    [[nodiscard]] runtime::InputFrame nextInput();
    void runTick();
    void drawFrame();
    void drawHud();
    void drawNameTags(const render::Camera2D& camera);
    void drawConnectionStatus();
    void drawDebugInfo();
    void drawTouchControls();
    void drawMenu();
    void drawInviteMenu();
    void handleMenuInput();
    void menuMove(int delta);
    void menuChange(int delta); // left/right or activate (0)
    enum class MenuItem { Resume, Vsync, FrameLimit, Scaling, ShowFps, Touch, Invite, Quit };
    [[nodiscard]] std::vector<MenuItem> menuItems() const;
    [[nodiscard]] int menuRows() const;   // rows shown now (the menu or the friends to invite)
    [[nodiscard]] int firstInvite() const; // the first friend shown (the list scrolls)
    // Invitations (docs/social.md#invitations): platform games with a login.
    [[nodiscard]] bool canInvite() const;
    void openInvites();
    void invite(const std::string& name);
    void updateInvites();
    void applySettings();
    [[nodiscard]] bool touchVisible() const;
    [[nodiscard]] bool networked() const { return m_net != nullptr; }
    [[nodiscard]] scene::Scene& currentScene();
    [[nodiscard]] HudState hudState() const;
    [[nodiscard]] double gameSeconds() const;
    void saveScreenshot(const std::filesystem::path& path);

    PlayerOptions m_options;
    std::unique_ptr<IFileSystem> m_files; // the project folder, or a downloaded package
    std::string m_ticketToken;            // platform: connect token for the game server
    std::string m_ticketAddress;          // platform: "host:port" of the game server
    std::string m_platformName;           // platform: the account name
    runtime::LoadedGame m_game; // manifest, start scene and compiled scripts
    std::string m_loadError;    // shown on screen when F5 reload fails
    std::unique_ptr<runtime::GameSession> m_session; // playing alone
    std::unique_ptr<net::GameServer> m_hostServer;   // --host
    std::unique_ptr<net::GameClient> m_net;          // multiplayer
    runtime::ActionSet m_actions;
    std::unique_ptr<platform::SdlPlatform> m_platform;
    std::unique_ptr<render::TextureCache> m_textures;
    runtime::KeyBindings m_bindings;
    std::optional<runtime::InputScript> m_script;
    std::optional<runtime::InputScriptPlayer> m_scriptPlayer;
    FixedStepClock m_clock{1.0 / 60.0};
    std::uint32_t m_previousHeld = 0;
    double m_lastTime = 0.0;
    double m_fpsTimer = 0.0;
    int m_fpsFrames = 0;
    double m_fps = 0.0;
    int m_frame = 0;
    int m_netTicks = 0;              // ticks played online (with --frames, online runs count these)
    std::size_t m_netMostPlayers = 0;
    bool m_netLevelComplete = false; // as last received
    bool m_netCompletedOnce = false;
    int m_netStatus = -1;            // last logged net::GameClient::Status
    int m_screenshotCounter = 0;
    std::filesystem::path m_pendingScreenshot;
    bool m_showDebug = false;
    bool m_deterministic = false;
    std::string m_connectTarget; // "host:port", for the connection screen
    PlayerSettings m_settings;
    std::filesystem::path m_settingsFile;
    runtime::TouchControls m_touch;
    std::uint32_t m_touchHeld = 0; // for drawing pressed buttons
    bool m_menuOpen = false;
    int m_menuItem = 0;
    bool m_quitRequested = false;
    TouchMode m_savedTouch = TouchMode::Auto; // --touch is for this run only, unless changed in the menu
    bool m_inviteOpen = false; // the menu shows friends to invite
    int m_inviteItem = 0;
    struct InviteFriend {
        std::string name;
        std::string state; // "PLAYING", "ONLINE", "OFFLINE"
    };
    std::vector<InviteFriend> m_inviteFriends;
    std::string m_inviteStatus;
    std::future<std::function<void(PlayerApp&)>> m_inviteJob; // a platform request
    std::vector<std::string> m_pendingInvites;                 // --invite, once playing
};

} // namespace px::player
