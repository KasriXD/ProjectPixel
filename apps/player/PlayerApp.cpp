#include "PlayerApp.hpp"

#include "px/project/Package.hpp"
#if defined(PX_HAVE_PLATFORM)
#include "px/online/PlatformClient.hpp"
#endif

#include "px/core/Log.hpp"
#include "px/core/Version.hpp"
#include "px/net/Udp.hpp"
#include "px/net/WebSocket.hpp"
#include "px/render/SceneRenderer.hpp"
#include "px/runtime/GameLoader.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string_view>

namespace px::player {

namespace {

bool parseInt(const char* text, int& out) {
    const std::string_view s(text);
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

constexpr Color kHudText{255, 236, 140, 255};
constexpr Color kHudShadow{30, 24, 40, 200};

std::string upper(std::string text) {
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

// The name other players see: --name, else PX_PLAYER_NAME, else the user's login name.
std::string defaultPlayerName() {
    for (const char* variable : {"PX_PLAYER_NAME", "USER", "USERNAME"}) {
        if (const char* value = std::getenv(variable); value != nullptr && *value != '\0') return value;
    }
    return "Player";
}

} // namespace

const char* playerUsage() {
    return "usage: px-player [project-dir] [--frames N] [--screenshot FILE.png]\n"
           "                 [--input-script FILE] [--debug] [--scale N] [--verbose]\n"
           "                 [--connect ADDRESS[:PORT] | --host [--port N]] [--name NAME]\n"
           "\n"
           "  project-dir         folder containing project.json; without one, the launcher opens\n"
           "                      (with --frames, the bundled example plays)\n"
           "  --connect ADDRESS   join a multiplayer server (px-server or another player's --host),\n"
           "                      over UDP (HOST[:PORT]) or WebSockets (ws://HOST:PORT/play, wss://...);\n"
           "                      you need the same project; the port defaults to 7777\n"
           "  --host              start a server in this window and play on it; others --connect\n"
           "  --port N            with --host: the UDP port to use (default 7777)\n"
           "  --name NAME         your name in multiplayer (default: your login name)\n"
           "  --platform URL      a platform (px-platform), with --play ID: play a published game on\n"
           "                      one of its servers, as the account logged in from Studio or\n"
           "                      given with --user NAME --password TEXT [--register]\n"
           "  --window-pos X,Y    place the window at this screen position\n"
           "  --frames N          run exactly N frames, one simulation tick per frame, then exit\n"
           "                      (online: N ticks on the server, at its pace)\n"
           "  --screenshot FILE   with --frames: save the last frame as a PNG\n"
           "  --input-script FILE play scripted input (see docs/input-scripts.md)\n"
           "  --debug             start with the debug overlay (toggle with F1)\n"
           "  --touch             show touch controls; the mouse acts as a finger (to try them)\n"
           "  --launcher          open the launcher (the default when no game is given): log in to a\n"
           "                      platform (--platform URL [--user NAME --password TEXT]), browse, play\n"
           "  --invite NAME       with --play: invite this friend once in the game (also from the menu)\n"
           "  --open PAGE         the launcher's first page: game:ID, friends or you; play:ID, join:NAME\n"
           "                      or local:N start a game (with --frames, the game also runs N frames)\n"
           "  --package FILE      play a game package (*.pxpkg) instead of a project folder\n"
           "  --token TEXT        with --connect: the connect token of a platform ticket\n"
           "  --scale N           initial window scale (default: from the project)\n"
           "\n"
           "  In game: arrows/WASD move, Space/Z/W jump, R restart, F5 reload scripts and\n"
           "           scenes from disk, F1 debug overlay, F2 screenshot, Esc menu (settings, quit).\n"
           "           Gamepads work at once; touch screens get on-screen controls.\n";
}

std::optional<PlayerOptions> parsePlayerArgs(int argc, char** argv, std::string& error) {
    PlayerOptions o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--frames") {
            const char* v = value();
            if (v == nullptr || !parseInt(v, o.frames) || o.frames < 1) {
                error = "--frames needs a positive number";
                return std::nullopt;
            }
        } else if (arg == "--screenshot") {
            const char* v = value();
            if (v == nullptr) {
                error = "--screenshot needs a file name";
                return std::nullopt;
            }
            o.screenshotPath = v;
        } else if (arg == "--input-script") {
            const char* v = value();
            if (v == nullptr) {
                error = "--input-script needs a file name";
                return std::nullopt;
            }
            o.inputScriptPath = v;
        } else if (arg == "--scale") {
            const char* v = value();
            if (v == nullptr || !parseInt(v, o.windowScale) || o.windowScale < 1 || o.windowScale > 16) {
                error = "--scale needs a number from 1 to 16";
                return std::nullopt;
            }
        } else if (arg == "--debug") {
            o.debugOverlay = true;
        } else if (arg == "--connect") {
            const char* v = value();
            if (v == nullptr) {
                error = "--connect needs a server address";
                return std::nullopt;
            }
            o.connectAddress = v;
        } else if (arg == "--host") {
            o.host = true;
        } else if (arg == "--window-pos") {
            const char* v = value();
            const std::string_view text = v != nullptr ? v : "";
            const auto comma = text.find(',');
            Vec2i position;
            if (comma == std::string_view::npos ||
                std::from_chars(text.data(), text.data() + comma, position.x).ec != std::errc() ||
                std::from_chars(text.data() + comma + 1, text.data() + text.size(), position.y).ec != std::errc()) {
                error = "--window-pos needs X,Y";
                return std::nullopt;
            }
            o.windowPosition = position;
        } else if (arg == "--port") {
            const char* v = value();
            if (v == nullptr || !parseInt(v, o.port) || o.port < 1 || o.port > 65535) {
                error = "--port needs a number from 1 to 65535";
                return std::nullopt;
            }
        } else if (arg == "--name") {
            const char* v = value();
            if (v == nullptr) {
                error = "--name needs a name";
                return std::nullopt;
            }
            o.playerName = v;
        } else if (arg == "--verbose") {
            o.verbose = true;
        } else if (arg == "--platform" || arg == "--user" || arg == "--password") {
            const char* v = value();
            if (v == nullptr) {
                error = std::format("{} needs a value", arg);
                return std::nullopt;
            }
            (arg == "--platform" ? o.platformUrl : arg == "--user" ? o.user : o.password) = v;
        } else if (arg == "--play") {
            const char* v = value();
            if (v == nullptr || std::from_chars(v, v + std::strlen(v), o.playGame).ec != std::errc() || o.playGame < 1) {
                error = "--play needs a game id";
                return std::nullopt;
            }
        } else if (arg == "--register") {
            o.registerAccount = true;
        } else if (arg == "--launcher") {
            o.launcher = true;
        } else if (arg == "--invite") {
            const char* v = value();
            if (v == nullptr) {
                error = "--invite needs a friend's name";
                return std::nullopt;
            }
            o.invite.emplace_back(v);
        } else if (arg == "--open") {
            const char* v = value();
            if (v == nullptr) {
                error = "--open needs game:ID, friends or you";
                return std::nullopt;
            }
            o.open = v;
        } else if (arg == "--touch") {
            o.touch = true;
        } else if (arg == "--package" || arg == "--token") {
            const char* v = value();
            if (v == nullptr) {
                error = std::format("{} needs a value", arg);
                return std::nullopt;
            }
            (arg == "--package" ? o.packagePath : o.token) = v;
        } else if (arg == "--help" || arg == "-h") {
            error.clear();
            return std::nullopt;
        } else if (!arg.starts_with("-") && o.projectDir.empty()) {
            o.projectDir = arg;
        } else {
            error = std::format("unknown argument '{}'", arg);
            return std::nullopt;
        }
    }
    if (!o.screenshotPath.empty() && o.frames == 0) {
        error = "--screenshot requires --frames";
        return std::nullopt;
    }
    if (o.host && !o.connectAddress.empty()) {
        error = "use either --host or --connect, not both";
        return std::nullopt;
    }
    if (o.port != 0 && !o.host) {
        error = "--port goes with --host (with --connect, write ADDRESS:PORT)";
        return std::nullopt;
    }
    if (o.playGame != 0 && o.platformUrl.empty()) {
        error = "--play needs --platform";
        return std::nullopt;
    }
    if (!o.platformUrl.empty() && o.playGame == 0 && !wantsLauncher(o)) {
        error = "--platform goes with --play ID or the launcher (--launcher)";
        return std::nullopt;
    }
    if (!o.invite.empty() && o.playGame == 0) {
        error = "--invite goes with --play (in the launcher, the game's menu invites)";
        return std::nullopt;
    }
    if (!o.open.empty() && !wantsLauncher(o)) {
        error = "--open is for the launcher (--launcher)";
        return std::nullopt;
    }
    if (o.playGame != 0 && (o.host || !o.connectAddress.empty() || !o.projectDir.empty())) {
        error = "--play downloads the game; do not give a project, --host or --connect";
        return std::nullopt;
    }
    return o;
}

namespace {

// Without a project argument: prefer examples/platformer next to the
// executable (packaged builds), then the source tree's copy (dev builds).
// Reads a game package through SDL, which also reaches files inside an
// Android app's assets ("game.pxpkg") and the iOS app bundle.
Result<project::Package> loadPackage(const std::string& path) {
    std::size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (data == nullptr) return makeError(ErrorCode::NotFound, "Could not read {}: {}", path, SDL_GetError());
    if (size > project::kMaxPackageBytes) {
        SDL_free(data);
        return makeError(ErrorCode::LimitExceeded, "{} is larger than a game package may be", path);
    }
    auto package = project::decodePackage({static_cast<const std::uint8_t*>(data), size});
    SDL_free(data);
    return package;
}

// Phones and tablets: the game comes in the app as a package.
std::string mobilePackage() {
#if defined(PX_MOBILE_PACKAGE)
#if defined(SDL_PLATFORM_ANDROID)
    return PX_MOBILE_PACKAGE; // in the APK's assets
#else
    const char* base = SDL_GetBasePath();
    return std::string(base != nullptr ? base : "") + PX_MOBILE_PACKAGE;
#endif
#else
    return {};
#endif
}

std::filesystem::path defaultProjectDir() {
    if (const char* base = SDL_GetBasePath()) {
        const auto packaged = std::filesystem::path(base) / "examples" / "platformer";
        std::error_code ec;
        if (std::filesystem::is_regular_file(packaged / "project.json", ec)) return packaged;
    }
    return std::filesystem::path(PX_DEFAULT_PROJECT_DIR);
}

} // namespace

std::vector<DeviceGame> gamesOnThisDevice() {
    std::vector<DeviceGame> out;
    if (const std::string package = mobilePackage(); !package.empty()) {
        auto loaded = loadPackage(package);
        if (!loaded) {
            log::warn("{}", loaded.error().message);
            return out;
        }
        auto files = loaded->toFileSystem();
        auto manifest = project::loadProjectManifest(*files);
        if (!manifest) return out;
        out.push_back({manifest->name, std::string(project::gameTypeName(manifest->settings.gameType)), {}, package});
        return out;
    }
    // The folder of examples (each with a project.json).
    std::error_code ec;
    const auto examples = defaultProjectDir().parent_path();
    for (const auto& entry : std::filesystem::directory_iterator(examples, ec)) {
        if (!entry.is_directory(ec) || !std::filesystem::is_regular_file(entry.path() / "project.json", ec)) continue;
        DirectoryFileSystem files(entry.path());
        auto manifest = project::loadProjectManifest(files);
        if (!manifest) continue;
        out.push_back({manifest->name, std::string(project::gameTypeName(manifest->settings.gameType)), entry.path(), {}});
    }
    std::sort(out.begin(), out.end(), [](const DeviceGame& a, const DeviceGame& b) { return a.title < b.title; });
    return out;
}

bool wantsLauncher(const PlayerOptions& o) {
    if (o.launcher) return true;
#if defined(PX_HAVE_LAUNCHER)
    return o.projectDir.empty() && o.packagePath.empty() && o.connectAddress.empty() && !o.host && o.playGame == 0 &&
           o.frames == 0;
#else
    return false;
#endif
}

Result<std::unique_ptr<PlayerApp>> PlayerApp::create(PlayerOptions options, std::unique_ptr<platform::SdlPlatform>* reuse) {
    if (options.verbose) log::setMinLevel(log::Level::Debug);
    if (options.projectDir.empty() && options.packagePath.empty()) options.packagePath = mobilePackage();
    std::filesystem::path dir = options.projectDir.empty() ? defaultProjectDir() : options.projectDir;
    const bool platformGame = options.playGame != 0;
    std::unique_ptr<PlayerApp> app(new PlayerApp(std::move(options), dir));
    std::string source = dir.generic_string();
    if (platformGame) {
        if (auto prepared = app->preparePlatformGame(); !prepared) return prepared.error();
        source = std::format("game {} on {}", app->m_options.playGame, app->m_options.platformUrl);
    } else if (!app->m_options.packagePath.empty() || dir.extension() == ".pxpkg") {
        source = app->m_options.packagePath.empty() ? dir.generic_string() : app->m_options.packagePath;
        auto package = loadPackage(source);
        if (!package) return package.error();
        app->m_files = package->toFileSystem();
    }

    auto game = runtime::loadGame(*app->m_files);
    if (!game) return game.error();
    app->m_game = std::move(game.value());
    for (const auto& w : app->m_game.warnings) log::warn("{}", w);
    log::info("Loaded project '{}' from {} ({} scripts)", app->m_game.manifest.name, source,
              app->m_game.scripts->size());

    const auto& display = app->m_game.manifest.settings.display;
    platform::WindowSettings window;
    window.title = std::format("{} - {}", app->m_game.manifest.name, version::productName);
    if (platformGame) {
        window.title = std::format("{} - {} - {}", app->m_game.manifest.name, app->m_platformName, version::productName);
    } else if (!app->m_options.gameTitle.empty()) {
        window.title = std::format("{} - {} - {}", app->m_options.gameTitle, app->m_options.playerName, version::productName);
    } else if (app->m_options.host || !app->m_options.connectAddress.empty()) {
        // Online, several windows of the same game are told apart by name.
        window.title = std::format("{} - {} - {}", app->m_game.manifest.name,
                                   app->m_options.playerName.empty() ? defaultPlayerName() : app->m_options.playerName,
                                   version::productName);
    }
    window.position = app->m_options.windowPosition;
    window.virtualResolution = display.virtualResolution;
    window.presentation = display.scaling == project::ScalingMode::Integer ? platform::Presentation::IntegerScale
                                                                           : platform::Presentation::Letterbox;
    window.windowScale = app->m_options.windowScale > 0 ? app->m_options.windowScale : display.windowScale;
    app->m_deterministic = app->m_options.frames > 0;
    // Saved settings; runs with --frames (tests, screenshots) use the defaults.
    const char* prefs = app->m_deterministic ? nullptr : SDL_GetPrefPath("ProjectPixel", "player");
    if (prefs != nullptr) {
        app->m_settingsFile = std::filesystem::path(prefs) / "settings.txt";
        SDL_free(const_cast<char*>(prefs));
        app->m_settings = PlayerSettings::load(app->m_settingsFile);
    }
    app->m_savedTouch = app->m_settings.touch;
    if (app->m_options.touch) app->m_settings.touch = TouchMode::On;
    window.vsync = !app->m_deterministic && app->m_settings.vsync;
    window.mouseAsTouch = app->m_options.touch;
    if (app->m_settings.scaling != Scaling::GameDefault) {
        window.presentation = app->m_settings.scaling == Scaling::PixelPerfect ? platform::Presentation::IntegerScale
                                                                               : platform::Presentation::Letterbox;
    }
    if (reuse != nullptr && *reuse) {
        // Everything that can fail is done: take the window now.
        app->m_platform = std::move(*reuse);
        app->m_platform->reconfigure(window);
    } else {
        auto platform = platform::SdlPlatform::create(window);
        if (!platform) return platform.error();
        app->m_platform = std::move(platform.value());
    }
    app->m_textures = std::make_unique<render::TextureCache>(app->m_platform->renderer(), *app->m_files,
                                                             app->m_game.manifest.assets);

    app->m_clock = FixedStepClock(1.0 / app->m_game.manifest.settings.tickRate);
    app->m_showDebug = app->m_options.debugOverlay;
    app->m_actions = runtime::ActionSet(app->m_game.manifest.input);
    app->m_touch = runtime::TouchControls(app->m_actions, {static_cast<float>(display.virtualResolution.x),
                                                           static_cast<float>(display.virtualResolution.y)});
    if (platformGame || app->m_options.host || !app->m_options.connectAddress.empty()) {
        if (auto started = app->startNetwork(); !started) return started.error();
    } else if (auto started = app->restart(); !started) {
        return started.error();
    }

    if (!app->m_options.inputScriptPath.empty()) {
        const auto& path = app->m_options.inputScriptPath;
        DirectoryFileSystem scriptDir(path.has_parent_path() ? path.parent_path() : std::filesystem::path("."));
        auto text = scriptDir.readText(path.filename().generic_string());
        if (!text) return text.error();
        auto script = runtime::InputScript::parse(text.value(), app->m_actions);
        if (!script) return makeError(script.error().code, "{}: {}", path.generic_string(), script.error().message);
        app->m_script = std::move(script.value());
        app->m_scriptPlayer.emplace(*app->m_script);
        log::info("Playing input script {} ({} ticks)", path.generic_string(), app->m_script->totalTicks());
    }
    app->m_pendingInvites = app->m_options.invite;
    app->m_lastTime = app->m_platform->secondsSinceStart();
    return app;
}

std::unique_ptr<platform::SdlPlatform> PlayerApp::releaseWindow() {
    if (networked()) m_net->disconnect();
    m_net.reset();
    m_hostServer.reset();
    m_session.reset();
    m_textures.reset(); // frees the game's textures in the window's renderer
    if (m_platform) m_platform->clearInput();
    return std::move(m_platform);
}

std::string PlayerApp::endMessage() const {
    if (m_quitRequested) return {}; // the player left
    if (networked() && m_net->status() == net::GameClient::Status::Disconnected) return m_net->statusText();
    return {};
}

PlayerApp::PlayerApp(PlayerOptions options, std::filesystem::path projectDir)
    : m_options(std::move(options)), m_files(std::make_unique<DirectoryFileSystem>(std::move(projectDir))) {}

px::Status PlayerApp::preparePlatformGame() {
#if defined(PX_HAVE_PLATFORM)
    online::PlatformClient platform(m_options.platformUrl);
    const auto dir = online::userPlatformDir();
    if (!m_options.user.empty()) {
        auto auth = m_options.registerAccount ? platform.registerAccount(m_options.user, m_options.password)
                                              : platform.login(m_options.user, m_options.password);
        if (!auth) return auth.error();
        m_platformName = auth->user.username;
        m_options.platformToken = auth->token;
        if (auto saved = online::saveLogin(dir, {m_options.platformUrl, m_platformName, auth->token}); !saved) {
            log::warn("{}", saved.error().message);
        }
    } else {
        const auto saved = online::loadSavedLogin(dir);
        if (!saved || saved->platformUrl != platform.baseUrl()) {
            return makeError(ErrorCode::AccessDenied,
                             "Not logged in to {}: log in from Studio (Platform panel) or add --user NAME --password TEXT",
                             m_options.platformUrl);
        }
        platform.setToken(saved->token);
        auto me = platform.me();
        if (!me) return makeError(me.error().code, "Logging in as {}: {}", saved->username, me.error().message);
        m_platformName = me->username;
        m_options.platformToken = saved->token;
    }
    log::info("Joining game {} on {} as {}", m_options.playGame, m_options.platformUrl, m_platformName);
    auto ticket = platform.join(m_options.playGame);
    if (!ticket) return ticket.error();
    online::PackageCache cache(dir / "packages");
    auto bytes = cache.get(platform, ticket->packageHash);
    if (!bytes) return bytes.error();
    auto package = project::decodePackage(bytes.value());
    if (!package) return package.error();
    m_files = package->toFileSystem();
    m_ticketToken = ticket->token;
    m_ticketAddress = std::format("{}:{}", ticket->host, ticket->port);
    return {};
#else
    return makeError(ErrorCode::Unsupported, "This build cannot connect to a platform");
#endif
}

px::Status PlayerApp::restart() {
    auto session = runtime::startSession(m_game);
    if (!session) return session.error();
    m_session = std::move(session.value());
    m_bindings = runtime::KeyBindings(m_game.manifest.input, m_session->actions());
    m_previousHeld = 0;
    m_clock.reset();
    m_loadError.clear();
    if (m_script) m_scriptPlayer.emplace(*m_script);
    return {};
}

px::Status PlayerApp::startNetwork() {
    std::string target = m_ticketAddress.empty() ? m_options.connectAddress : m_ticketAddress;
    const bool webSocket = target.starts_with("ws://") || target.starts_with("wss://");
    if (!webSocket && !net::udpSupported()) {
        return makeError(ErrorCode::Unsupported, "This build joins games over WebSockets only (ws:// or wss:// addresses)");
    }
    std::uint16_t port = net::kDefaultPort;
    if (m_options.host) {
        if (m_options.port > 0) port = static_cast<std::uint16_t>(m_options.port);
        auto endpoint = net::listenUdp(port, 8);
        if (!endpoint) return endpoint.error();
        net::ServerOptions serverOptions;
        serverOptions.name = std::format("{}'s game", m_options.playerName.empty() ? defaultPlayerName()
                                                                                    : m_options.playerName);
        auto server = net::GameServer::create(m_game, std::move(endpoint.value()), serverOptions);
        if (!server) return server.error();
        m_hostServer = std::move(server.value());
        log::info("Hosting '{}' on UDP port {}. Others join with: px-player --connect <this computer's address>:{}",
                  m_game.manifest.name, port, port);
        target = std::format("127.0.0.1:{}", port);
    }
    m_connectTarget = target;
    auto endpoint = net::connectToServer(target, net::kDefaultPort);
    if (!endpoint) return endpoint.error();
    net::ClientOptions clientOptions;
    clientOptions.playerName = !m_platformName.empty() ? m_platformName
                               : m_options.playerName.empty() ? defaultPlayerName()
                                                              : m_options.playerName;
    clientOptions.token = !m_ticketToken.empty() ? m_ticketToken : m_options.token;
    auto client = net::GameClient::create(m_game, std::move(endpoint.value()), clientOptions);
    if (!client) return client.error();
    m_net = std::move(client.value());
    m_bindings = runtime::KeyBindings(m_game.manifest.input, m_actions);
    m_previousHeld = 0;
    m_clock.reset();
    log::info("Connecting to {} as '{}'", m_connectTarget, clientOptions.playerName);
    return {};
}

scene::Scene& PlayerApp::currentScene() { return networked() ? m_net->view() : m_session->scene(); }

double PlayerApp::gameSeconds() const {
    return networked() ? m_platform->secondsSinceStart() : m_session->elapsedSeconds();
}

PlayerApp::HudState PlayerApp::hudState() const {
    HudState h;
    if (networked()) {
        const net::NetGameState& g = m_net->game();
        h.objective = g.objective;
        h.levelComplete = g.levelComplete;
        h.completionMessage = g.completionMessage;
        h.scriptErrors = g.scriptErrors;
        h.lastScriptError = g.lastScriptError;
        h.restartTicks = g.restartTicks;
    } else {
        const auto& s = m_session->state();
        h.objective = s.objective;
        h.levelComplete = s.levelComplete;
        h.completionMessage = s.completionMessage;
        h.scriptErrors = s.scriptErrors;
        h.lastScriptError = s.lastScriptError;
    }
    return h;
}

void PlayerApp::reload() {
    if (networked()) {
        log::warn("Reloading from disk is not available while playing online");
        return;
    }
    // Reads project.json, the start scene and every script again, so edits to
    // Luau scripts or scene files take effect without restarting the program.
    // If anything fails to load, the current game keeps running and the
    // error is shown on screen. (Window and resolution settings are not re-applied.)
    auto game = runtime::loadGame(*m_files);
    if (!game) {
        m_loadError = game.error().message;
        log::error("Reload failed: {}", m_loadError);
        return;
    }
    auto session = runtime::startSession(game.value());
    if (!session) {
        m_loadError = session.error().message;
        log::error("Reload failed: {}", m_loadError);
        return;
    }
    for (const auto& w : game->warnings) log::warn("{}", w);
    m_session.reset(); // the old session references the old game's scripts
    m_game = std::move(game.value());
    m_textures = std::make_unique<render::TextureCache>(m_platform->renderer(), *m_files, m_game.manifest.assets);
    m_session = std::move(session.value());
    m_bindings = runtime::KeyBindings(m_game.manifest.input, m_session->actions());
    m_previousHeld = 0;
    m_clock.reset();
    m_loadError.clear();
    if (m_script) m_scriptPlayer.emplace(*m_script);
    log::info("Reloaded '{}' ({} scripts)", m_game.manifest.name, m_game.scripts->size());
}

runtime::InputFrame PlayerApp::nextInput() {
    if (m_scriptPlayer) return m_scriptPlayer->next();
    if (m_menuOpen) {
        // Nothing pressed in the menu reaches the game, not even after it closes.
        m_platform->clearPressedKeys();
        m_platform->clearPressedPad();
        m_platform->clearTaps();
        m_previousHeld = 0;
        return {};
    }
    // Keyboard, gamepads and touch together. A key, button or tap released
    // between two ticks still counts as one press.
    const std::vector<Vec2> fingers = m_platform->touches();
    const std::uint32_t touchHeld = touchVisible() ? m_touch.heldActions(fingers) : 0u;
    const std::uint32_t touchTapped = touchVisible() ? m_touch.heldActions(m_platform->taps()) : 0u;
    m_touchHeld = touchHeld;
    const std::uint32_t tapped = m_bindings.heldActions(m_platform->keysPressed()) |
                                 m_bindings.heldActions(m_platform->padPressed()) | touchTapped;
    const std::uint32_t held =
        m_bindings.heldActions(m_platform->keysDown()) | m_bindings.heldActions(m_platform->padDown()) | touchHeld | tapped;
    m_platform->clearPressedKeys();
    m_platform->clearPressedPad();
    m_platform->clearTaps();
    runtime::InputFrame frame{held, (held & ~m_previousHeld) | tapped};
    m_previousHeld = held;
    return frame;
}

void PlayerApp::runTick() {
    if (networked()) {
        if (m_hostServer) m_hostServer->tick();
        // Input (and a scripted route) starts once the server has let us in.
        if (m_net->status() != net::GameClient::Status::Playing) return;
        m_net->tick(nextInput());
        ++m_netTicks;
        return;
    }
    m_session->tick(nextInput());
    for (const auto& event : m_session->events()) {
        switch (event.type) {
            case runtime::GameEvent::Type::ObjectiveChanged:
                log::info("{}", event.text);
                break;
            case runtime::GameEvent::Type::PlayerRespawned:
                log::info("Player fell and respawned");
                break;
            case runtime::GameEvent::Type::LevelCompleted:
                log::info("Level complete after {:.2f} s: {}", m_session->elapsedSeconds(), event.text);
                break;
            case runtime::GameEvent::Type::ScoreChanged:
            case runtime::GameEvent::Type::ScriptError: // already logged by the session
            case runtime::GameEvent::Type::ScriptEvent:
                break;
        }
    }
}

PlayerApp::Status PlayerApp::iterate() {
    const double now = m_platform->secondsSinceStart();
    const double elapsed = now - m_lastTime;
    m_lastTime = now;

    handleMenuInput();
    if (m_quitRequested) return Status::Success;
    if (!m_menuOpen) m_inviteOpen = false;
    updateInvites();
    // Deterministic runs alone advance exactly one tick per frame regardless
    // of speed; online the server's clock rules, so ticks follow real time.
    int steps = m_deterministic && !networked() ? 1 : m_clock.advance(elapsed);
    if (m_menuOpen && !networked()) steps = 0; // playing alone, the menu pauses the game
    if (networked()) {
        m_net->receive(now);
        if (const int status = static_cast<int>(m_net->status()); status != m_netStatus) {
            m_netStatus = status;
            if (m_net->status() == net::GameClient::Status::Disconnected) log::warn("{}", m_net->statusText());
        }
        const auto& g = m_net->game();
        m_netMostPlayers = std::max(m_netMostPlayers, g.players.size());
        if (g.levelComplete && !m_netLevelComplete) log::info("Level complete (online), score {}", g.score);
        m_netLevelComplete = g.levelComplete;
        m_netCompletedOnce = m_netCompletedOnce || g.levelComplete;
    }
    for (int i = 0; i < steps; ++i) runTick();
    if (networked()) m_net->updateView(now);

    ++m_fpsFrames;
    m_fpsTimer += elapsed;
    if (m_fpsTimer >= 0.5) {
        m_fps = m_fpsFrames / m_fpsTimer;
        m_fpsFrames = 0;
        m_fpsTimer = 0.0;
    }

    drawFrame();
    ++m_frame;
#if !defined(__EMSCRIPTEN__)
    // The frame-rate limit (the browser paces frames itself).
    if (m_settings.frameLimit > 0 && !m_deterministic) {
        const double frame = 1.0 / m_settings.frameLimit;
        const double spent = m_platform->secondsSinceStart() - now;
        if (spent < frame) SDL_DelayPrecise(static_cast<Uint64>((frame - spent) * 1.0e9));
    }
#endif
    if (m_options.frames > 0 && networked()) {
        // Online, --frames counts ticks played on the server (or ends at a disconnect).
        const bool gone = m_net->status() == net::GameClient::Status::Disconnected;
        if (m_netTicks >= m_options.frames || gone) {
            log::info("Finished {} ticks online: {}, score {}, level complete: {}, {} players (most at once {})",
                      m_netTicks, m_net->statusText(), m_net->game().score, m_netCompletedOnce,
                      m_net->game().players.size(), m_netMostPlayers);
            return Status::Success;
        }
        return Status::Continue;
    }
    if (m_options.frames > 0 && m_frame >= m_options.frames) {
        const auto& state = m_session->state();
        log::info("Finished {} frames: score {}, objective \"{}\", level complete: {}, {} respawns, {} script errors",
                  m_frame, state.score, state.objective, state.levelComplete, state.respawns, state.scriptErrors);
        return Status::Success;
    }
    return Status::Continue;
}

void PlayerApp::drawFrame() {
    auto& renderer = m_platform->renderer();
    const auto& res = m_game.manifest.settings.display.virtualResolution;
    const Vec2 viewport{static_cast<float>(res.x), static_cast<float>(res.y)};
    scene::Scene& scene = currentScene();
    const render::Camera2D camera = render::cameraForScene(scene, viewport);

    renderer.beginFrame(scene.settings().backgroundColor);
    render::renderScene(scene, camera, renderer, *m_textures);
    if (m_showDebug) render::renderCollisionOverlay(scene, camera, renderer);
    if (networked()) drawNameTags(camera);
    drawHud();
    if (networked() && m_net->status() != net::GameClient::Status::Playing) drawConnectionStatus();
    if (m_showDebug) drawDebugInfo();
    if (touchVisible() && !m_menuOpen) drawTouchControls();
    if (m_settings.showFps) {
        const std::string fps = std::format("{:.0f} FPS", m_fps);
        const float width = static_cast<float>(m_game.manifest.settings.display.virtualResolution.x);
        const Vec2 at{width - 6 - render::IRenderer2D::kDebugGlyphSize * static_cast<float>(fps.size()),
                      touchVisible() ? std::round(m_touch.pauseButton().y + m_touch.pauseButton().h + 4) : 6.0f};
        renderer.drawDebugText({at.x + 1, at.y + 1}, fps, kHudShadow);
        renderer.drawDebugText(at, fps, colors::white);
    }
    if (m_menuOpen) drawMenu();

    const bool lastFrame = m_options.frames > 0 && m_frame + 1 >= m_options.frames;
    if (lastFrame && !m_options.screenshotPath.empty()) saveScreenshot(m_options.screenshotPath);
    if (!m_pendingScreenshot.empty()) {
        saveScreenshot(m_pendingScreenshot);
        m_pendingScreenshot.clear();
    }
    renderer.endFrame();
}

void PlayerApp::drawHud() {
    auto& r = m_platform->renderer();
    const HudState state = hudState();
    const auto text = [&](Vec2 p, const std::string& s, Color c) {
        r.drawDebugText({p.x + 1, p.y + 1}, s, kHudShadow);
        r.drawDebugText(p, s, c);
    };
    const float width = static_cast<float>(m_game.manifest.settings.display.virtualResolution.x);
    const float height = static_cast<float>(m_game.manifest.settings.display.virtualResolution.y);
    constexpr float g = render::IRenderer2D::kDebugGlyphSize;

    // The objective line is set by scripts (Game.setObjective).
    if (!state.objective.empty()) text({6, 6}, state.objective, kHudText);
    const std::string& error = !m_loadError.empty() ? m_loadError : state.lastScriptError;
    if (gameSeconds() < 6.0 && !m_scriptPlayer && error.empty()) {
        std::string hint = m_game.manifest.settings.gameType == project::GameType::TopDown
                               ? "ARROWS MOVE  R RESTART"
                               : "ARROWS MOVE  SPACE JUMP  R RESTART";
        if (networked()) hint.resize(hint.size() - std::string_view("  R RESTART").size()); // the server restarts
        text({(width - g * static_cast<float>(hint.size())) * 0.5f, height - 14}, hint, colors::white);
    }
    if (state.levelComplete) {
        std::string line1 = state.completionMessage.empty() ? std::string("LEVEL COMPLETE!") : state.completionMessage;
        const std::size_t maxChars = static_cast<std::size_t>((width - 32) / g);
        if (line1.size() > maxChars) line1.resize(maxChars);
        std::string line2 = "PRESS R TO PLAY AGAIN";
        if (networked()) {
            line2 = state.restartTicks > 0
                        ? std::format("NEXT ROUND IN {}", (state.restartTicks + m_net->tickRate() - 1) / m_net->tickRate())
                        : std::string("WAITING FOR THE SERVER");
        }
        const float boxW = g * static_cast<float>(std::max(line1.size(), line2.size())) + 24;
        const Rect box{(width - boxW) * 0.5f, height * 0.5f - 22, boxW, 44};
        r.fillRect(box, Color{20, 16, 40, 200});
        r.drawRect(box, kHudText);
        text({(width - g * static_cast<float>(line1.size())) * 0.5f, box.y + 9}, line1, kHudText);
        text({(width - g * static_cast<float>(line2.size())) * 0.5f, box.y + 27}, line2, colors::white);
    }

    // Online: how many are playing, top right.
    if (networked() && m_net->status() == net::GameClient::Status::Playing) {
        const std::size_t n = m_net->game().players.size();
        const std::string line = n == 1 ? std::string("1 PLAYER") : std::format("{} PLAYERS", n);
        text({width - 6 - g * static_cast<float>(line.size()), 18}, line, colors::white); // below the objective
    }

    // Script and reload errors are shown on screen so creators notice them.
    if (!error.empty()) {
        const std::size_t perLine = static_cast<std::size_t>((width - 8) / g);
        std::vector<std::string> lines = {!m_loadError.empty() ? "RELOAD FAILED (F5 TO RETRY):"
                                                                : std::format("SCRIPT ERRORS: {}", state.scriptErrors)};
        for (std::size_t i = 0; i < error.size() && lines.size() < 4; i += perLine) lines.push_back(error.substr(i, perLine));
        const float boxH = static_cast<float>(lines.size()) * 10.0f + 4.0f;
        const float y0 = height - boxH;
        r.fillRect({0, y0, width, boxH}, Color{120, 20, 30, 220});
        for (std::size_t i = 0; i < lines.size(); ++i) {
            r.drawDebugText({4, y0 + 3 + static_cast<float>(i) * 10.0f}, lines[i], colors::white);
        }
    }
}

void PlayerApp::drawNameTags(const render::Camera2D& camera) {
    auto& r = m_platform->renderer();
    scene::Scene& scene = m_net->view();
    const scene::Entity own = m_net->character();
    constexpr float g = render::IRenderer2D::kDebugGlyphSize;
    for (const scene::Entity e : scene.entities()) {
        const auto* player = scene.tryGet<scene::Player>(e);
        if (player == nullptr || player->name.empty() || e == own) continue;
        const Vec2 position = scene.worldPosition(e);
        float top = position.y;
        if (const auto* sprite = scene.tryGet<scene::Sprite>(e)) top -= sprite->size.y * sprite->pivot.y;
        const Vec2 at = camera.worldToScreen({position.x, top});
        const std::string name = upper(player->name);
        const Vec2 p{std::round(at.x - g * static_cast<float>(name.size()) * 0.5f), std::round(at.y - g - 3.0f)};
        r.drawDebugText({p.x + 1, p.y + 1}, name, kHudShadow);
        r.drawDebugText(p, name, colors::white);
    }
}

void PlayerApp::drawConnectionStatus() {
    auto& r = m_platform->renderer();
    const float width = static_cast<float>(m_game.manifest.settings.display.virtualResolution.x);
    const float height = static_cast<float>(m_game.manifest.settings.display.virtualResolution.y);
    constexpr float g = render::IRenderer2D::kDebugGlyphSize;
    const std::size_t maxChars = static_cast<std::size_t>((width - 24) / g);
    std::vector<std::string> lines;
    switch (m_net->status()) {
        case net::GameClient::Status::Connecting: lines = {upper("Connecting to " + m_connectTarget) + "..."}; break;
        case net::GameClient::Status::Joining: lines = {"JOINING..."}; break;
        case net::GameClient::Status::Disconnected: {
            lines.push_back("DISCONNECTED");
            const std::string why = upper(m_net->statusText());
            for (std::size_t i = 0; i < why.size() && lines.size() < 5; i += maxChars) lines.push_back(why.substr(i, maxChars));
            lines.push_back("PRESS ESC TO QUIT");
            break;
        }
        case net::GameClient::Status::Playing: return;
    }
    for (auto& line : lines) {
        if (line.size() > maxChars) line.resize(maxChars);
    }
    std::size_t widest = 0;
    for (const auto& line : lines) widest = std::max(widest, line.size());
    const float boxW = g * static_cast<float>(widest) + 24;
    const float boxH = static_cast<float>(lines.size()) * 12.0f + 14.0f;
    const Rect box{std::round((width - boxW) * 0.5f), std::round((height - boxH) * 0.5f), boxW, boxH};
    r.fillRect(box, Color{20, 16, 40, 220});
    r.drawRect(box, kHudText);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const float x = std::round((width - g * static_cast<float>(lines[i].size())) * 0.5f);
        r.drawDebugText({x, box.y + 8 + static_cast<float>(i) * 12.0f}, lines[i], i == 0 ? kHudText : colors::white);
    }
}

void PlayerApp::drawDebugInfo() {
    auto& r = m_platform->renderer();
    std::vector<std::string> lines;
    if (networked()) {
        const net::ClientStats st = m_net->stats();
        const double seconds = std::max(1.0, m_platform->secondsSinceStart());
        const auto own = m_net->predictedPosition();
        lines = {
            std::format("FPS {:.0f}  PING {:.0f} MS  LOSS {:.0f}%", m_fps, st.roundTripMs, st.packetLoss * 100.0),
            std::format("DOWN {:.1f} KB/S  UP {:.1f} KB/S", static_cast<double>(st.bytesReceived) / 1024.0 / seconds,
                        static_cast<double>(st.bytesSent) / 1024.0 / seconds),
            std::format("SNAPSHOTS {}  DROPPED {}  {} B", st.snapshots, st.snapshotsDropped, st.lastSnapshotBytes),
            std::format("CORRECTIONS {}  LAST {:.2f}  PENDING {}", st.corrections, st.lastCorrection, st.pendingInputs),
            own ? std::format("PLAYER {} AT {:.1f},{:.1f}", m_net->player(), own->x, own->y)
                : std::format("PLAYER {}", m_net->player()),
        };
        if (m_hostServer) {
            lines.push_back(std::format("HOSTING: {} PLAYING, TICK {}", m_hostServer->playerCount(),
                                        m_hostServer->ticksRun()));
        }
    } else {
        std::string playerLine = "PLAYER -";
        if (const auto* t = m_session->scene().tryGet<scene::Transform>(m_session->player())) {
            playerLine = std::format("PLAYER {:.1f},{:.1f}", t->position.x, t->position.y);
        }
        lines = {
            std::format("FPS {:.0f}  TICK {}", m_fps, m_session->tickCount()),
            std::format("ENTITIES {}", m_session->scene().entityCount()),
            playerLine,
            std::format("SCRIPTS {}  LUAU {} KIB", m_session->scripts().stats().instances,
                        m_session->scripts().stats().memoryBytes / 1024),
        };
    }
    float y = 18;
    for (const auto& line : lines) {
        r.fillRect({4, y - 1, render::IRenderer2D::kDebugGlyphSize * static_cast<float>(line.size()) + 4, 10},
                   Color{0, 0, 0, 150});
        r.drawDebugText({6, y}, line, colors::white);
        y += 10;
    }
}

void PlayerApp::saveScreenshot(const std::filesystem::path& path) {
    if (auto status = m_platform->saveScreenshot(path); !status) {
        log::error("{}", status.error().message);
    } else {
        log::info("Saved screenshot {}", path.generic_string());
    }
}

PlayerApp::Status PlayerApp::handleEvent(const SDL_Event& event) {
    if (m_platform->processEvent(event)) return Status::Success;
    // A phone call, the home button, another app: pause (alone) behind the menu.
    if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND && !m_deterministic && !m_menuOpen) {
        m_menuOpen = true;
        m_menuItem = 0;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        switch (event.key.scancode) {
            case SDL_SCANCODE_ESCAPE:
            case SDL_SCANCODE_AC_BACK: // Android's back button
                if (networked() && m_net->status() == net::GameClient::Status::Disconnected) return Status::Success;
                if (m_inviteOpen) {
                    m_inviteOpen = false; // back to the menu
                    break;
                }
                m_menuOpen = !m_menuOpen;
                m_menuItem = 0;
                log::info("Menu {}", m_menuOpen ? "opened" : "closed");
                break;
            case SDL_SCANCODE_UP:
                if (m_menuOpen) menuMove(-1);
                break;
            case SDL_SCANCODE_DOWN:
                if (m_menuOpen) menuMove(1);
                break;
            case SDL_SCANCODE_LEFT:
                if (m_menuOpen) menuChange(-1);
                break;
            case SDL_SCANCODE_RIGHT:
                if (m_menuOpen) menuChange(1);
                break;
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_SPACE:
                if (m_menuOpen) menuChange(0);
                break;
            case SDL_SCANCODE_R:
                if (networked()) break; // the server restarts the level
                log::info("Restarting");
                if (auto status = restart(); !status) m_loadError = status.error().message;
                break;
            case SDL_SCANCODE_F5:
                log::info("Reloading project from disk");
                reload();
                break;
            case SDL_SCANCODE_F1:
                m_showDebug = !m_showDebug;
                break;
            case SDL_SCANCODE_F2:
                // Captured during the next frame, after drawing and before presenting.
                ++m_screenshotCounter;
                m_pendingScreenshot = std::format("px-screenshot-{}.png", m_screenshotCounter);
                break;
            default:
                break;
        }
    }
    if (m_quitRequested) {
        if (networked()) m_net->disconnect();
        return Status::Success;
    }
    return Status::Continue;
}

// --- Touch controls and the menu ----------------------------------------------------------------

namespace {
constexpr int kInviteRows = 8; // friends shown at once
const char* scalingName(Scaling s) {
    return s == Scaling::PixelPerfect ? "PIXEL PERFECT" : s == Scaling::Fit ? "FIT SCREEN" : "GAME DEFAULT";
}
const char* touchName(TouchMode t) { return t == TouchMode::On ? "ON" : t == TouchMode::Off ? "OFF" : "AUTO"; }
} // namespace

bool PlayerApp::touchVisible() const {
    if (m_scriptPlayer) return false;
    // Auto: from the first touch, or from the start on phones and tablets.
    static const bool mobile = std::string_view(SDL_GetPlatform()) == "Android" ||
                               std::string_view(SDL_GetPlatform()) == "iOS";
    return m_settings.touch == TouchMode::On ||
           (m_settings.touch == TouchMode::Auto && (mobile || m_platform->touchUsed()));
}

std::vector<PlayerApp::MenuItem> PlayerApp::menuItems() const {
    std::vector<MenuItem> items{MenuItem::Resume, MenuItem::Vsync,   MenuItem::FrameLimit,
                                MenuItem::Scaling, MenuItem::ShowFps, MenuItem::Touch};
    if (canInvite()) items.push_back(MenuItem::Invite);
    items.push_back(MenuItem::Quit);
    return items;
}

int PlayerApp::menuRows() const {
    if (!m_inviteOpen) return static_cast<int>(menuItems().size());
    return std::min(static_cast<int>(m_inviteFriends.size()), kInviteRows) + 1;
}

int PlayerApp::firstInvite() const {
    const int count = static_cast<int>(m_inviteFriends.size());
    return std::clamp(m_inviteItem - kInviteRows / 2, 0, std::max(0, count - kInviteRows));
}

void PlayerApp::menuMove(int delta) {
    if (m_inviteOpen) {
        const int n = static_cast<int>(m_inviteFriends.size()) + 1;
        m_inviteItem = (m_inviteItem + delta + n) % n;
        return;
    }
    const int n = static_cast<int>(menuItems().size());
    m_menuItem = (m_menuItem + delta + n) % n;
}

void PlayerApp::menuChange(int delta) {
    if (m_inviteOpen) {
        if (delta != 0) return;
        if (m_inviteItem >= static_cast<int>(m_inviteFriends.size())) {
            m_inviteOpen = false; // BACK
        } else {
            invite(m_inviteFriends[static_cast<std::size_t>(m_inviteItem)].name);
        }
        return;
    }
    const auto items = menuItems();
    if (m_menuItem < 0 || m_menuItem >= static_cast<int>(items.size())) m_menuItem = 0;
    const int step = delta == 0 ? 1 : delta;
    switch (items[static_cast<std::size_t>(m_menuItem)]) {
        case MenuItem::Resume:
            if (delta == 0) m_menuOpen = false;
            return;
        case MenuItem::Vsync: m_settings.vsync = !m_settings.vsync; break;
        case MenuItem::FrameLimit: {
            const int limits[] = {0, 30, 60, 120};
            int i = 0;
            while (i < 4 && limits[i] != m_settings.frameLimit) ++i;
            m_settings.frameLimit = limits[((i % 4) + step + 4) % 4];
            break;
        }
        case MenuItem::Scaling:
            m_settings.scaling = static_cast<Scaling>((static_cast<int>(m_settings.scaling) + step + 3) % 3);
            break;
        case MenuItem::ShowFps: m_settings.showFps = !m_settings.showFps; break;
        case MenuItem::Touch:
            m_settings.touch = static_cast<TouchMode>((static_cast<int>(m_settings.touch) + step + 3) % 3);
            m_savedTouch = m_settings.touch;
            break;
        case MenuItem::Invite:
            if (delta == 0) openInvites();
            return;
        case MenuItem::Quit:
            if (delta == 0) m_quitRequested = true;
            return;
    }
    applySettings();
}

// --- Inviting friends --------------------------------------------------------------------------

bool PlayerApp::canInvite() const {
#if defined(PX_HAVE_PLATFORM)
    return networked() && !m_options.platformUrl.empty() && !m_options.platformToken.empty();
#else
    return false;
#endif
}

void PlayerApp::openInvites() {
    m_inviteOpen = true;
    m_inviteItem = 0;
#if defined(PX_HAVE_PLATFORM)
    if (m_inviteJob.valid()) return;
    m_inviteStatus = "LOOKING FOR YOUR FRIENDS...";
    online::PlatformClient client(m_options.platformUrl);
    client.setToken(m_options.platformToken);
    client.setTimeoutSeconds(10);
    m_inviteJob = std::async(std::launch::async, [client]() mutable -> std::function<void(PlayerApp&)> {
        auto social = client.social();
        if (!social) return [m = social.error().message](PlayerApp& app) { app.m_inviteStatus = upper(m); };
        std::vector<InviteFriend> list;
        for (const char* state : {"playing", "online", "offline"}) {
            for (const auto& f : social->friends) {
                if (f.presence.state == state) list.push_back({f.username, upper(state)});
            }
        }
        return [list](PlayerApp& app) {
            app.m_inviteFriends = list;
            app.m_inviteItem = 0;
            app.m_inviteStatus = list.empty() ? "NO FRIENDS YET: ADD THEM IN THE LAUNCHER" : "";
        };
    });
#endif
}

void PlayerApp::invite(const std::string& name) {
#if defined(PX_HAVE_PLATFORM)
    if (m_inviteJob.valid()) return; // one request at a time
    m_inviteStatus = "INVITING " + upper(name) + "...";
    online::PlatformClient client(m_options.platformUrl);
    client.setToken(m_options.platformToken);
    client.setTimeoutSeconds(10);
    m_inviteJob = std::async(std::launch::async, [client, name]() mutable -> std::function<void(PlayerApp&)> {
        auto sent = client.invite(name);
        if (!sent) {
            return [name, m = sent.error().message](PlayerApp& app) {
                log::warn("Could not invite {}: {}", name, m);
                app.m_inviteStatus = upper(m);
            };
        }
        return [name](PlayerApp& app) {
            log::info("Invited {} to this game", name);
            app.m_inviteStatus = "INVITED " + upper(name);
        };
    });
#else
    (void)name;
#endif
}

void PlayerApp::updateInvites() {
    if (m_inviteJob.valid() && m_inviteJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        if (auto apply = m_inviteJob.get()) apply(*this);
    }
    // --invite: once the server has let us in.
    if (!m_pendingInvites.empty() && !m_inviteJob.valid() && canInvite() &&
        m_net->status() == net::GameClient::Status::Playing) {
        invite(m_pendingInvites.front());
        m_pendingInvites.erase(m_pendingInvites.begin());
    }
}

void PlayerApp::applySettings() {
    m_platform->setVsync(m_settings.vsync && !m_deterministic);
    const auto& display = m_game.manifest.settings.display;
    platform::Presentation p = display.scaling == project::ScalingMode::Integer ? platform::Presentation::IntegerScale
                                                                                : platform::Presentation::Letterbox;
    if (m_settings.scaling == Scaling::PixelPerfect) p = platform::Presentation::IntegerScale;
    if (m_settings.scaling == Scaling::Fit) p = platform::Presentation::Letterbox;
    m_platform->setPresentation(p);
    PlayerSettings saved = m_settings;
    saved.touch = m_savedTouch;
    if (!m_settingsFile.empty() && !saved.save(m_settingsFile)) log::warn("Could not save the settings");
}

void PlayerApp::handleMenuInput() {
    // Gamepad: Start opens and closes the menu; the D-pad, South and East use it.
    const PadStates& pad = m_platform->padPressed();
    const auto pressed = [&pad](PadButton b) { return pad[static_cast<std::size_t>(b)]; };
    bool used = false;
    if (pressed(PadButton::Start)) {
        m_menuOpen = !m_menuOpen;
        m_menuItem = 0;
        used = true;
    } else if (m_menuOpen) {
        if (pressed(PadButton::DPadUp)) menuMove(-1);
        if (pressed(PadButton::DPadDown)) menuMove(1);
        if (pressed(PadButton::DPadLeft)) menuChange(-1);
        if (pressed(PadButton::DPadRight)) menuChange(1);
        if (pressed(PadButton::South)) menuChange(0);
        if (pressed(PadButton::East) || pressed(PadButton::Back)) {
            if (m_inviteOpen) {
                m_inviteOpen = false;
            } else {
                m_menuOpen = false;
            }
        }
        used = true;
    }
    // Touch: the pause button, and rows of the menu.
    if (touchVisible() || m_menuOpen) {
        for (const Vec2 t : m_platform->taps()) {
            if (!m_menuOpen && touchVisible() && m_touch.hitsPause(t)) {
                m_menuOpen = true;
                m_menuItem = 0;
                used = true;
                break; // one tap, one action
            }
            if (m_menuOpen) {
                const float height = static_cast<float>(m_game.manifest.settings.display.virtualResolution.y);
                const int rows = menuRows();
                const float top = std::round(height * 0.5f - static_cast<float>(rows) * 6.0f);
                const int row = static_cast<int>(std::floor((t.y - top) / 12.0f));
                if (row >= 0 && row < rows) {
                    if (m_inviteOpen) {
                        const int shown = rows - 1; // friends, then BACK
                        m_inviteItem = row < shown ? firstInvite() + row : static_cast<int>(m_inviteFriends.size());
                    } else {
                        m_menuItem = row;
                    }
                    menuChange(0);
                }
                used = true;
                break;
            }
        }
    }
    if (used) {
        m_platform->clearPressedPad();
        m_platform->clearTaps();
    }
}

void PlayerApp::drawTouchControls() {
    auto& r = m_platform->renderer();
    constexpr float g = render::IRenderer2D::kDebugGlyphSize;
    for (const runtime::TouchButton& b : m_touch.buttons()) {
        const bool down = (m_touchHeld & b.actions) != 0;
        r.fillRect(b.area, down ? Color{255, 255, 255, 110} : Color{20, 20, 30, 90});
        r.drawRect(b.area, Color{255, 255, 255, static_cast<std::uint8_t>(down ? 230 : 150)});
        const Vec2 at{std::round(b.area.x + (b.area.w - g * static_cast<float>(b.label.size())) * 0.5f),
                      std::round(b.area.y + (b.area.h - g) * 0.5f)};
        r.drawDebugText(at, b.label, colors::white);
    }
    const Rect& p = m_touch.pauseButton();
    r.fillRect(p, Color{20, 20, 30, 90});
    r.drawRect(p, Color{255, 255, 255, 150});
    r.drawDebugText({std::round(p.x + (p.w - g * 2) * 0.5f), std::round(p.y + (p.h - g) * 0.5f)}, "II", colors::white);
}

void PlayerApp::drawMenu() {
    auto& r = m_platform->renderer();
    const auto& res = m_game.manifest.settings.display.virtualResolution;
    const float width = static_cast<float>(res.x);
    const float height = static_cast<float>(res.y);
    constexpr float g = render::IRenderer2D::kDebugGlyphSize;
    r.fillRect({0, 0, width, height}, Color{10, 8, 24, 190});
    if (m_inviteOpen) {
        drawInviteMenu();
        return;
    }
    std::vector<std::string> items;
    for (const MenuItem item : menuItems()) {
        switch (item) {
            case MenuItem::Resume: items.emplace_back(networked() ? "BACK TO THE GAME" : "RESUME"); break;
            case MenuItem::Vsync: items.push_back(std::format("VSYNC: {}", m_settings.vsync ? "ON" : "OFF")); break;
            case MenuItem::FrameLimit:
                items.push_back(m_settings.frameLimit == 0 ? std::string("FRAME LIMIT: NONE")
                                                           : std::format("FRAME LIMIT: {}", m_settings.frameLimit));
                break;
            case MenuItem::Scaling: items.push_back(std::format("SCALING: {}", scalingName(m_settings.scaling))); break;
            case MenuItem::ShowFps: items.push_back(std::format("SHOW FPS: {}", m_settings.showFps ? "ON" : "OFF")); break;
            case MenuItem::Touch: items.push_back(std::format("TOUCH CONTROLS: {}", touchName(m_settings.touch))); break;
            case MenuItem::Invite: items.emplace_back("INVITE A FRIEND"); break;
            case MenuItem::Quit:
                items.emplace_back(networked() ? "LEAVE THE GAME" : m_options.fromLauncher ? "BACK TO GAMES" : "QUIT");
                break;
        }
    }
    const int count = static_cast<int>(items.size());
    const float top = std::round(height * 0.5f - static_cast<float>(count) * 6.0f);
    const std::string title = networked() ? "MENU (THE GAME GOES ON)" : "PAUSED";
    r.drawDebugText({std::round((width - g * static_cast<float>(title.size())) * 0.5f), top - 16}, title, kHudText);
    for (int i = 0; i < count; ++i) {
        const std::string& s = items[static_cast<std::size_t>(i)];
        const float y = top + static_cast<float>(i) * 12.0f;
        if (i == m_menuItem) r.fillRect({width * 0.15f, y - 2, width * 0.7f, 12}, Color{255, 255, 255, 50});
        r.drawDebugText({std::round((width - g * static_cast<float>(s.size())) * 0.5f), y}, s,
                        i == m_menuItem ? kHudText : colors::white);
    }
}

void PlayerApp::drawInviteMenu() {
    auto& r = m_platform->renderer();
    const auto& res = m_game.manifest.settings.display.virtualResolution;
    const float width = static_cast<float>(res.x);
    const float height = static_cast<float>(res.y);
    constexpr float g = render::IRenderer2D::kDebugGlyphSize;
    const std::size_t columns = static_cast<std::size_t>(std::max(8.0f, width / g - 2.0f));
    const auto centred = [&](std::string text, float y, Color color) {
        if (text.size() > columns) text = text.substr(0, columns - 3) + "...";
        r.drawDebugText({std::round((width - g * static_cast<float>(text.size())) * 0.5f), y}, text, color);
    };
    const int rows = menuRows();
    const float top = std::round(height * 0.5f - static_cast<float>(rows) * 6.0f);
    centred("INVITE A FRIEND TO THIS GAME", top - 16, kHudText);
    const int first = firstInvite();
    const int shown = rows - 1;
    for (int row = 0; row < rows; ++row) {
        const bool back = row == shown;
        const int index = back ? static_cast<int>(m_inviteFriends.size()) : first + row;
        const float y = top + static_cast<float>(row) * 12.0f;
        const bool selected = index == m_inviteItem;
        if (selected) r.fillRect({width * 0.1f, y - 2, width * 0.8f, 12}, Color{255, 255, 255, 50});
        const std::string label =
            back ? std::string("BACK")
                 : std::format("{}  {}", upper(m_inviteFriends[static_cast<std::size_t>(index)].name),
                               m_inviteFriends[static_cast<std::size_t>(index)].state);
        centred(label, y, selected ? kHudText : colors::white);
    }
    if (!m_inviteStatus.empty()) centred(m_inviteStatus, top + static_cast<float>(rows) * 12.0f + 6.0f, kHudText);
}

} // namespace px::player
