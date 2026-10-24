#include "PlayerApp.hpp"

#include "px/core/Log.hpp"
#include "px/core/Version.hpp"
#include "px/net/Udp.hpp"
#include "px/render/SceneRenderer.hpp"
#include "px/runtime/GameLoader.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <charconv>
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
           "  project-dir         folder containing project.json (default: the bundled example)\n"
           "  --connect ADDRESS   join a multiplayer server (px-server or another player's --host);\n"
           "                      you need the same project; the port defaults to 7777\n"
           "  --host              start a server in this window and play on it; others --connect\n"
           "  --port N            with --host: the UDP port to use (default 7777)\n"
           "  --name NAME         your name in multiplayer (default: your login name)\n"
           "  --window-pos X,Y    place the window at this screen position\n"
           "  --frames N          run exactly N frames, one simulation tick per frame, then exit\n"
           "  --screenshot FILE   with --frames: save the last frame as a PNG\n"
           "  --input-script FILE play scripted input (see docs/input-scripts.md)\n"
           "  --debug             start with the debug overlay (toggle with F1)\n"
           "  --scale N           initial window scale (default: from the project)\n"
           "\n"
           "  In game: arrows/WASD move, Space/Z/W jump, R restart, F5 reload scripts and\n"
           "           scenes from disk, F1 debug overlay, F2 screenshot, Esc quit.\n";
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
    return o;
}

namespace {

// Without a project argument: prefer examples/platformer next to the
// executable (packaged builds), then the source tree's copy (dev builds).
std::filesystem::path defaultProjectDir() {
    if (const char* base = SDL_GetBasePath()) {
        const auto packaged = std::filesystem::path(base) / "examples" / "platformer";
        std::error_code ec;
        if (std::filesystem::is_regular_file(packaged / "project.json", ec)) return packaged;
    }
    return std::filesystem::path(PX_DEFAULT_PROJECT_DIR);
}

} // namespace

Result<std::unique_ptr<PlayerApp>> PlayerApp::create(PlayerOptions options) {
    if (options.verbose) log::setMinLevel(log::Level::Debug);
    std::filesystem::path dir = options.projectDir.empty() ? defaultProjectDir() : options.projectDir;
    std::unique_ptr<PlayerApp> app(new PlayerApp(std::move(options), dir));

    auto game = runtime::loadGame(app->m_files);
    if (!game) return game.error();
    app->m_game = std::move(game.value());
    for (const auto& w : app->m_game.warnings) log::warn("{}", w);
    log::info("Loaded project '{}' from {} ({} scripts)", app->m_game.manifest.name, dir.generic_string(),
              app->m_game.scripts->size());

    const auto& display = app->m_game.manifest.settings.display;
    platform::WindowSettings window;
    window.title = std::format("{} - {}", app->m_game.manifest.name, version::productName);
    if (app->m_options.host || !app->m_options.connectAddress.empty()) {
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
    window.vsync = !app->m_deterministic;
    auto platform = platform::SdlPlatform::create(window);
    if (!platform) return platform.error();
    app->m_platform = std::move(platform.value());
    app->m_textures = std::make_unique<render::TextureCache>(app->m_platform->renderer(), app->m_files,
                                                             app->m_game.manifest.assets);

    app->m_clock = FixedStepClock(1.0 / app->m_game.manifest.settings.tickRate);
    app->m_showDebug = app->m_options.debugOverlay;
    app->m_actions = runtime::ActionSet(app->m_game.manifest.input);
    if (app->m_options.host || !app->m_options.connectAddress.empty()) {
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
    app->m_lastTime = app->m_platform->secondsSinceStart();
    return app;
}

PlayerApp::PlayerApp(PlayerOptions options, std::filesystem::path projectDir)
    : m_options(std::move(options)), m_files(std::move(projectDir)) {}

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
    if (!net::udpSupported()) return makeError(ErrorCode::Unsupported, "Multiplayer is not available in this build");
    std::string host = "127.0.0.1";
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
    } else {
        auto address = net::parseAddress(m_options.connectAddress, net::kDefaultPort);
        if (!address) return address.error();
        host = address->first;
        port = address->second;
    }
    m_connectTarget = std::format("{}:{}", host, port);
    auto endpoint = net::connectUdp(host, port);
    if (!endpoint) return endpoint.error();
    net::ClientOptions clientOptions;
    clientOptions.playerName = m_options.playerName.empty() ? defaultPlayerName() : m_options.playerName;
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
    auto game = runtime::loadGame(m_files);
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
    m_textures = std::make_unique<render::TextureCache>(m_platform->renderer(), m_files, m_game.manifest.assets);
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
    // A key tapped and released between two ticks still counts as one press.
    const std::uint32_t tapped = m_bindings.heldActions(m_platform->keysPressed());
    const std::uint32_t held = m_bindings.heldActions(m_platform->keysDown()) | tapped;
    m_platform->clearPressedKeys();
    runtime::InputFrame frame{held, (held & ~m_previousHeld) | tapped};
    m_previousHeld = held;
    return frame;
}

void PlayerApp::runTick() {
    if (networked()) {
        if (m_hostServer) m_hostServer->tick();
        m_net->tick(nextInput());
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

    // Deterministic runs advance exactly one tick per frame regardless of speed.
    const int steps = m_deterministic ? 1 : m_clock.advance(elapsed);
    if (networked()) m_net->receive(now);
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
    if (m_options.frames > 0 && m_frame >= m_options.frames) {
        if (networked()) {
            log::info("Finished {} frames: {}, score {}, {} players", m_frame, m_net->statusText(), m_net->game().score,
                      m_net->game().players.size());
            return Status::Success;
        }
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
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        switch (event.key.scancode) {
            case SDL_SCANCODE_ESCAPE:
                if (networked()) m_net->disconnect();
                return Status::Success;
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
    return Status::Continue;
}

} // namespace px::player
