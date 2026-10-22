#include "PlayerApp.hpp"

#include "px/core/Log.hpp"
#include "px/core/Version.hpp"
#include "px/project/SceneFormat.hpp"
#include "px/render/SceneRenderer.hpp"

#include <SDL3/SDL.h>

#include <charconv>
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

} // namespace

const char* playerUsage() {
    return "usage: px-player [project-dir] [--frames N] [--screenshot FILE.png]\n"
           "                 [--input-script FILE] [--debug] [--scale N] [--verbose]\n"
           "\n"
           "  project-dir         folder containing project.json (default: the bundled example)\n"
           "  --frames N          run exactly N frames, one simulation tick per frame, then exit\n"
           "  --screenshot FILE   with --frames: save the last frame as a PNG\n"
           "  --input-script FILE play scripted input (see docs/input-scripts.md)\n"
           "  --debug             start with the debug overlay (toggle with F1)\n"
           "  --scale N           initial window scale (default: from the project)\n"
           "\n"
           "  In game: arrows/WASD move, Space/Z/W jump, R restart, F1 debug overlay,\n"
           "           F2 screenshot, Esc quit.\n";
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
    return o;
}

Result<std::unique_ptr<PlayerApp>> PlayerApp::create(PlayerOptions options) {
    if (options.verbose) log::setMinLevel(log::Level::Debug);
    std::filesystem::path dir = options.projectDir.empty() ? std::filesystem::path(PX_DEFAULT_PROJECT_DIR)
                                                           : options.projectDir;
    std::unique_ptr<PlayerApp> app(new PlayerApp(std::move(options), dir));

    auto manifest = project::loadProjectManifest(app->m_files);
    if (!manifest) return manifest.error();
    app->m_manifest = std::move(manifest.value());
    auto scene = project::loadScene(app->m_files, app->m_manifest, app->m_manifest.startScene);
    if (!scene) return scene.error();
    app->m_startScene = std::move(scene.value());
    log::info("Loaded project '{}' from {}", app->m_manifest.name, dir.generic_string());

    const auto& display = app->m_manifest.settings.display;
    platform::WindowSettings window;
    window.title = std::format("{} - {}", app->m_manifest.name, version::productName);
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
                                                             app->m_manifest.assets);

    app->m_clock = FixedStepClock(1.0 / app->m_manifest.settings.tickRate);
    app->m_showDebug = app->m_options.debugOverlay;
    app->restart();

    if (!app->m_options.inputScriptPath.empty()) {
        const auto& path = app->m_options.inputScriptPath;
        DirectoryFileSystem scriptDir(path.has_parent_path() ? path.parent_path() : std::filesystem::path("."));
        auto text = scriptDir.readText(path.filename().generic_string());
        if (!text) return text.error();
        auto script = runtime::InputScript::parse(text.value(), app->m_session->actions());
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

void PlayerApp::restart() {
    m_session = std::make_unique<runtime::GameSession>(m_manifest, m_startScene);
    m_bindings = runtime::KeyBindings(m_manifest.input, m_session->actions());
    m_previousHeld = 0;
    m_clock.reset();
    if (m_script) m_scriptPlayer.emplace(*m_script);
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
    m_session->tick(nextInput());
    for (const auto& event : m_session->events()) {
        switch (event.type) {
            case runtime::GameEvent::Type::Collected:
                log::info("Collected {} ({}/{})", event.kind, m_session->state().collectiblesCollected,
                          m_session->state().collectiblesTotal);
                break;
            case runtime::GameEvent::Type::PlayerRespawned:
                log::info("Player fell and respawned");
                break;
            case runtime::GameEvent::Type::AllCollected:
                log::info("All collectibles collected after {:.2f} s", m_session->elapsedSeconds());
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
    for (int i = 0; i < steps; ++i) runTick();

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
        log::info("Finished {} frames: score {}, {}/{} collected, {} respawns", m_frame, m_session->state().score,
                  m_session->state().collectiblesCollected, m_session->state().collectiblesTotal,
                  m_session->state().respawns);
        return Status::Success;
    }
    return Status::Continue;
}

void PlayerApp::drawFrame() {
    auto& renderer = m_platform->renderer();
    const auto& res = m_manifest.settings.display.virtualResolution;
    const Vec2 viewport{static_cast<float>(res.x), static_cast<float>(res.y)};
    const render::Camera2D camera = render::cameraForScene(m_session->scene(), viewport);

    renderer.beginFrame(m_session->scene().settings().backgroundColor);
    render::renderScene(m_session->scene(), camera, renderer, *m_textures);
    if (m_showDebug) render::renderCollisionOverlay(m_session->scene(), camera, renderer);
    drawHud();
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
    const auto& state = m_session->state();
    const auto text = [&](Vec2 p, const std::string& s, Color c) {
        r.drawDebugText({p.x + 1, p.y + 1}, s, kHudShadow);
        r.drawDebugText(p, s, c);
    };
    const float width = static_cast<float>(m_manifest.settings.display.virtualResolution.x);
    const float height = static_cast<float>(m_manifest.settings.display.virtualResolution.y);
    constexpr float g = render::IRenderer2D::kDebugGlyphSize;

    if (state.collectiblesTotal > 0) {
        text({6, 6}, std::format("COINS {}/{}", state.collectiblesCollected, state.collectiblesTotal), kHudText);
    }
    if (m_session->elapsedSeconds() < 6.0 && !m_scriptPlayer) {
        const std::string hint = "ARROWS MOVE  SPACE JUMP  R RESTART";
        text({(width - g * static_cast<float>(hint.size())) * 0.5f, height - 14}, hint, colors::white);
    }
    if (state.allCollected) {
        const std::string line1 = "ALL COINS COLLECTED!";
        const std::string line2 = "PRESS R TO PLAY AGAIN";
        const float boxW = g * static_cast<float>(line2.size()) + 24;
        const Rect box{(width - boxW) * 0.5f, height * 0.5f - 22, boxW, 44};
        r.fillRect(box, Color{20, 16, 40, 200});
        r.drawRect(box, kHudText);
        text({(width - g * static_cast<float>(line1.size())) * 0.5f, box.y + 9}, line1, kHudText);
        text({(width - g * static_cast<float>(line2.size())) * 0.5f, box.y + 27}, line2, colors::white);
    }
}

void PlayerApp::drawDebugInfo() {
    auto& r = m_platform->renderer();
    std::string playerLine = "PLAYER -";
    if (const auto* t = m_session->scene().tryGet<scene::Transform>(m_session->player())) {
        playerLine = std::format("PLAYER {:.1f},{:.1f}", t->position.x, t->position.y);
    }
    const std::string lines[] = {
        std::format("FPS {:.0f}  TICK {}", m_fps, m_session->tickCount()),
        std::format("ENTITIES {}", m_session->scene().entityCount()),
        playerLine,
    };
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
                return Status::Success;
            case SDL_SCANCODE_R:
                log::info("Restarting");
                restart();
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
