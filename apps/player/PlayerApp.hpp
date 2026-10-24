#pragma once

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
#include "px/scene/Scene.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

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
};

// Parses command-line arguments. Returns nullopt and sets `error` on failure.
[[nodiscard]] std::optional<PlayerOptions> parsePlayerArgs(int argc, char** argv, std::string& error);
[[nodiscard]] const char* playerUsage();

// The client application: owns the window, the running game session and the
// presentation (HUD, debug overlay). Driven by SDL's main callbacks.
class PlayerApp {
public:
    enum class Status { Continue, Success, Failure };

    [[nodiscard]] static Result<std::unique_ptr<PlayerApp>> create(PlayerOptions options);

    [[nodiscard]] Status iterate();
    [[nodiscard]] Status handleEvent(const SDL_Event& event);

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
    void reload();
    [[nodiscard]] runtime::InputFrame nextInput();
    void runTick();
    void drawFrame();
    void drawHud();
    void drawNameTags(const render::Camera2D& camera);
    void drawConnectionStatus();
    void drawDebugInfo();
    [[nodiscard]] bool networked() const { return m_net != nullptr; }
    [[nodiscard]] scene::Scene& currentScene();
    [[nodiscard]] HudState hudState() const;
    [[nodiscard]] double gameSeconds() const;
    void saveScreenshot(const std::filesystem::path& path);

    PlayerOptions m_options;
    DirectoryFileSystem m_files;
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
    int m_screenshotCounter = 0;
    std::filesystem::path m_pendingScreenshot;
    bool m_showDebug = false;
    bool m_deterministic = false;
    std::string m_connectTarget; // "host:port", for the connection screen
};

} // namespace px::player
