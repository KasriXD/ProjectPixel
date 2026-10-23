#include "StudioApp.hpp"

#include "Panels.hpp"
#include "Utf8.hpp"

#include "px/core/Log.hpp"
#include "px/editor/Picking.hpp"
#include "px/core/Version.hpp"
#include "px/platform_sdl/SdlKeys.hpp"
#include "px/project/SceneFormat.hpp"
#include "px/scene/ComponentInfo.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace px::studio {

namespace {

constexpr const char* kDockSpace = "StudioDockSpace";

void applyStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.PopupRounding = 3.0f;
    style.ScrollbarRounding = 6.0f;
    style.FramePadding = {6.0f, 4.0f};
    style.ItemSpacing = {7.0f, 5.0f};
    style.WindowPadding = {8.0f, 8.0f};
    style.IndentSpacing = 16.0f;
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = {0.13f, 0.14f, 0.16f, 1.0f};
    c[ImGuiCol_ChildBg] = {0.11f, 0.12f, 0.14f, 1.0f};
    c[ImGuiCol_PopupBg] = {0.12f, 0.13f, 0.15f, 0.98f};
    c[ImGuiCol_MenuBarBg] = {0.10f, 0.11f, 0.13f, 1.0f};
    c[ImGuiCol_TitleBg] = {0.10f, 0.11f, 0.13f, 1.0f};
    c[ImGuiCol_TitleBgActive] = {0.12f, 0.14f, 0.18f, 1.0f};
    c[ImGuiCol_FrameBg] = {0.19f, 0.21f, 0.24f, 1.0f};
    c[ImGuiCol_FrameBgHovered] = {0.24f, 0.27f, 0.31f, 1.0f};
    c[ImGuiCol_FrameBgActive] = {0.27f, 0.31f, 0.37f, 1.0f};
    c[ImGuiCol_Header] = {0.22f, 0.36f, 0.55f, 0.65f};
    c[ImGuiCol_HeaderHovered] = {0.26f, 0.43f, 0.66f, 0.80f};
    c[ImGuiCol_HeaderActive] = {0.28f, 0.47f, 0.72f, 1.0f};
    c[ImGuiCol_Button] = {0.22f, 0.25f, 0.29f, 1.0f};
    c[ImGuiCol_ButtonHovered] = {0.28f, 0.42f, 0.62f, 1.0f};
    c[ImGuiCol_ButtonActive] = {0.30f, 0.48f, 0.72f, 1.0f};
    c[ImGuiCol_Tab] = {0.15f, 0.17f, 0.20f, 1.0f};
    c[ImGuiCol_TabHovered] = {0.28f, 0.42f, 0.62f, 1.0f};
    c[ImGuiCol_TabSelected] = {0.22f, 0.31f, 0.45f, 1.0f};
    c[ImGuiCol_TabDimmedSelected] = {0.18f, 0.22f, 0.28f, 1.0f};
    c[ImGuiCol_DockingPreview] = {0.30f, 0.55f, 0.90f, 0.6f};
    style.ScaleAllSizes(scale);
}

bool parseInt(const char* text, int& out) {
    char* end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (end == text || *end != '\0') return false;
    out = static_cast<int>(v);
    return true;
}

std::filesystem::path findTemplateFolder() {
    if (const char* base = SDL_GetBasePath()) {
        const auto packaged = std::filesystem::path(base) / "examples" / "platformer";
        std::error_code ec;
        if (std::filesystem::is_regular_file(packaged / "project.json", ec)) return packaged;
    }
    return std::filesystem::path(PX_DEFAULT_TEMPLATE_DIR);
}

// The guides opened from the Help menu: next to the executable in a
// package, otherwise the source tree's docs folder.
std::filesystem::path findDocsFolder() {
    if (const char* base = SDL_GetBasePath()) {
        const auto packaged = std::filesystem::path(base) / "docs";
        std::error_code ec;
        if (std::filesystem::is_regular_file(packaged / "studio.md", ec)) return packaged;
    }
    return std::filesystem::path(PX_DOCS_DIR);
}

} // namespace

const char* studioUsage() {
    return "usage: px-studio [project-dir] [--reset-layout] [--verbose]\n"
           "                 [--smoke-test [--screenshot FILE.png]]\n"
           "\n"
           "  project-dir     open this project (folder containing project.json)\n"
           "  --reset-layout  restore the default window layout\n"
           "  --smoke-test    automated UI run used by the test suite: opens the project,\n"
           "                  edits, undoes, plays for two seconds and exits; nothing is saved\n"
           "  --screenshot F  with --smoke-test: save a PNG of the window at the end\n";
}

std::optional<StudioOptions> parseStudioArgs(int argc, char** argv, std::string& error) {
    StudioOptions o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--smoke-test") {
            o.smokeTest = true;
        } else if (arg == "--screenshot") {
            if (i + 1 >= argc) {
                error = "--screenshot needs a file name";
                return std::nullopt;
            }
            o.screenshotPath = fromUtf8(argv[++i]);
        } else if (arg == "--reset-layout") {
            o.resetLayout = true;
        } else if (arg == "--verbose") {
            o.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            error.clear();
            return std::nullopt;
        } else if (!arg.starts_with("-") && o.projectDir.empty()) {
            o.projectDir = fromUtf8(arg);
        } else {
            error = std::format("unknown argument '{}'", arg);
            return std::nullopt;
        }
    }
    if (!o.screenshotPath.empty() && !o.smokeTest) {
        error = "--screenshot requires --smoke-test";
        return std::nullopt;
    }
    if (o.smokeTest && o.projectDir.empty()) {
        error = "--smoke-test needs a project folder";
        return std::nullopt;
    }
    (void)parseInt;
    return o;
}

// --- Lifetime --------------------------------------------------------------------------------

StudioApp::StudioApp(StudioOptions options) : m_options(std::move(options)) {}

Result<std::unique_ptr<StudioApp>> StudioApp::create(StudioOptions options) {
    if (options.verbose) log::setMinLevel(log::Level::Debug);
    std::unique_ptr<StudioApp> app(new StudioApp(std::move(options)));
    if (app->initWindowAndUi() != Status::Continue) {
        return makeError(ErrorCode::Unsupported, "Could not start Studio: {}", SDL_GetError());
    }
    log::info("{} Studio {} ({})", version::productName, version::string, SDL_GetRendererName(app->m_sdlRenderer));
    if (!app->m_options.projectDir.empty()) {
        app->openProjectNow(app->m_options.projectDir);
        if (!app->m_project) {
            if (app->m_options.smokeTest) return makeError(ErrorCode::NotFound, "Smoke test: the project did not open");
        }
    }
    app->m_lastTime = static_cast<double>(SDL_GetTicksNS()) * 1.0e-9;
    return app;
}

StudioApp::Status StudioApp::initWindowAndUi() {
    SDL_SetAppMetadata((std::string(version::productName) + " Studio").c_str(), version::string, "dev.px.studio");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return Status::Failure;

    if (!m_options.smokeTest) {
        if (char* pref = SDL_GetPrefPath("px", "studio")) {
            m_prefDir = fromUtf8(pref);
            SDL_free(pref);
            m_settingsFile = m_prefDir / "studio-settings.txt";
            m_iniPath = toUtf8(m_prefDir / "studio-layout.ini");
            m_settings = StudioSettings::load(m_settingsFile);
        }
    }
    m_templateFolder = findTemplateFolder();

    float displayScale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (displayScale <= 0.0f) displayScale = 1.0f;
    m_uiScale = displayScale * m_settings.uiScale;
    int width = static_cast<int>(1600.0f * displayScale);
    int height = static_cast<int>(940.0f * displayScale);
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 0 && usable.h > 0) {
        width = std::min(width, usable.w - 40);
        height = std::min(height, usable.h - 60);
    }
    m_window = SDL_CreateWindow(std::format("{} Studio", version::productName).c_str(), std::max(width, 800),
                                std::max(height, 500), SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (m_window == nullptr) return Status::Failure;
    m_sdlRenderer = SDL_CreateRenderer(m_window, nullptr);
    if (m_sdlRenderer == nullptr) return Status::Failure;
    SDL_SetRenderVSync(m_sdlRenderer, m_options.smokeTest ? 0 : 1);
    m_renderer2d = std::make_unique<platform::SdlRenderer2D>(m_sdlRenderer);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.IniFilename = m_iniPath.empty() ? nullptr : m_iniPath.c_str();
    applyStyle(m_uiScale);
    ImGui::GetStyle().FontScaleDpi = m_uiScale;
    ImFontConfig font;
    font.SizePixels = 15.0f;
    m_monoFont = io.Fonts->AddFontDefaultVector(&font);
    if (!ImGui_ImplSDL3_InitForSDLRenderer(m_window, m_sdlRenderer)) return Status::Failure;
    if (!ImGui_ImplSDLRenderer3_Init(m_sdlRenderer)) return Status::Failure;
    m_imguiReady = true;

    m_hierarchy = std::make_unique<HierarchyPanel>();
    m_inspector = std::make_unique<InspectorPanel>();
    m_viewport = std::make_unique<ViewportPanel>();
    m_assets = std::make_unique<AssetsPanel>();
    m_console = std::make_unique<ConsolePanel>();
    m_scripts = std::make_unique<ScriptEditorPanel>();
    m_projectSettings = std::make_unique<ProjectSettingsPanel>();
    if (m_options.resetLayout) m_layoutBuilt = false;
    return Status::Continue;
}

StudioApp::~StudioApp() {
    m_play.reset();
    if (!m_options.smokeTest && !m_settingsFile.empty()) (void)m_settings.save(m_settingsFile);
    m_textures.reset(); // GPU textures before the renderer
    m_renderer2d.reset();
    if (m_imguiReady) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    if (m_sdlRenderer != nullptr) SDL_DestroyRenderer(m_sdlRenderer);
    if (m_window != nullptr) SDL_DestroyWindow(m_window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

// --- Project and scene -----------------------------------------------------------------------------

scene::Scene* StudioApp::activeScene() {
    if (m_play) return &m_play->session().scene();
    if (m_document) return &m_document->sceneForEdit();
    return nullptr;
}

std::vector<scene::Entity> StudioApp::selection() {
    if (!m_document) return {};
    if (!m_play) return m_document->selection();
    std::vector<scene::Entity> out;
    const scene::Scene& running = m_play->session().scene();
    for (const scene::Entity e : m_document->selection()) {
        const Guid id = m_document->scene().get<scene::EntityInfo>(e).id;
        const scene::Entity live = running.findById(id);
        if (!live.isNull()) out.push_back(live);
    }
    return out;
}

scene::Entity StudioApp::primarySelection() {
    if (!m_document) return scene::kNullEntity;
    const scene::Entity e = m_document->primarySelection();
    if (!m_play || e.isNull()) return e;
    const auto* info = m_document->scene().tryGet<scene::EntityInfo>(e);
    return info != nullptr ? m_play->session().scene().findById(info->id) : scene::kNullEntity;
}

void StudioApp::select(scene::Entity entity, editor::SelectMode mode) {
    if (!m_document) return;
    if (m_play) {
        // Map from the running copy back to the edited scene (same ids).
        const auto* info = m_play->session().scene().tryGet<scene::EntityInfo>(entity);
        entity = info != nullptr ? m_document->scene().findById(info->id) : scene::kNullEntity;
    }
    if (entity.isNull()) {
        if (mode == editor::SelectMode::Replace) m_document->clearSelection();
        return;
    }
    m_document->select(entity, mode);
}

void StudioApp::requestOpenProject(const std::filesystem::path& folder) {
    runAfterSaveCheck("open another project", [this, folder] { openProjectNow(folder); });
}

void StudioApp::openProjectNow(const std::filesystem::path& folder) {
    auto project = editor::EditorProject::open(folder);
    if (!project) {
        m_errorModal = project.error().message;
        log::error("{}", project.error().message);
        m_settings.removeRecent(folder);
        return;
    }
    closeProjectNow();
    m_project = std::move(project.value());
    m_textures = std::make_unique<render::TextureCache>(*m_renderer2d, m_project->files(), m_project->assets());
    m_settings.addRecent(m_project->root());
    if (!m_settingsFile.empty() && !m_options.smokeTest) (void)m_settings.save(m_settingsFile);
    log::info("Opened project '{}' ({})", m_project->displayName(), toUtf8(m_project->root()));
    (void)m_project->checkScriptsChangedOnDisk(); // remember the current file times
    openSceneNow(m_project->manifest().startScene);
}

void StudioApp::requestNewProject(const std::filesystem::path& folder, const std::string& name, bool fromTemplate) {
    runAfterSaveCheck("create a project", [this, folder, name, fromTemplate] {
        std::optional<std::filesystem::path> tpl;
        if (fromTemplate) tpl = m_templateFolder;
        auto project = editor::EditorProject::create(folder, name, tpl);
        if (!project) {
            m_errorModal = project.error().message;
            log::error("{}", project.error().message);
            return;
        }
        log::info("Created project '{}' in {}", name, toUtf8(folder));
        project.value().reset();
        openProjectNow(folder);
    });
}

void StudioApp::requestCloseProject() {
    runAfterSaveCheck("close the project", [this] { closeProjectNow(); });
}

void StudioApp::closeProjectNow() {
    m_play.reset();
    m_document.reset();
    m_textures.reset();
    m_project.reset();
    m_viewport->resetCamera();
    *m_scripts = ScriptEditorPanel{};
    showProjectSettings = false;
    updateTitle();
}

void StudioApp::requestOpenScene(assets::AssetId scene) {
    if (m_document && m_document->assetId() == scene) return;
    runAfterSaveCheck("open another scene", [this, scene] { openSceneNow(scene); });
}

void StudioApp::openSceneNow(assets::AssetId sceneId) {
    if (!m_project) return;
    stopPlay();
    std::vector<std::string> warnings;
    auto scene = m_project->loadScene(sceneId, &warnings);
    if (!scene) {
        m_errorModal = scene.error().message;
        log::error("{}", scene.error().message);
        return;
    }
    for (const auto& w : warnings) log::warn("{}", w);
    m_document = std::make_unique<editor::SceneDocument>(sceneId, std::move(scene.value()));
    m_viewport->resetCamera();
    const auto* entry = m_project->asset(sceneId);
    log::info("Opened scene '{}'", entry != nullptr ? entry->name : sceneId.toString());
    updateTitle();
}

void StudioApp::requestQuit() {
    runAfterSaveCheck("quit", [this] { m_quit = true; });
}

bool StudioApp::hasUnsavedChanges() const {
    if (!m_project) return false;
    return (m_document && m_document->isDirty()) || m_project->anyScriptBufferDirty() || m_project->manifestDirty();
}

std::string StudioApp::unsavedSummary() const {
    std::vector<std::string> parts;
    if (m_document && m_document->isDirty()) {
        const auto* entry = m_project->asset(m_document->assetId());
        parts.push_back(std::format("the scene '{}'", entry != nullptr ? entry->name : "?"));
    }
    if (m_project && m_project->anyScriptBufferDirty()) parts.emplace_back("edited scripts");
    if (m_project && m_project->manifestDirty()) parts.emplace_back("the project settings");
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += i + 1 == parts.size() ? " and " : ", ";
        out += parts[i];
    }
    return out;
}

bool StudioApp::saveAll() {
    if (!m_project) return true;
    bool ok = true;
    if (m_document && m_document->isDirty()) {
        if (auto saved = m_document->save(*m_project); !saved) {
            ok = false;
            m_errorModal = saved.error().message;
            log::error("{}", saved.error().message);
        }
    }
    if (auto saved = m_project->saveAllScripts(); !saved) {
        ok = false;
        log::error("{}", saved.error().message);
        notify(saved.error().message, true);
    }
    if (m_project->manifestDirty()) {
        if (auto saved = m_project->saveManifest(); !saved) {
            ok = false;
            log::error("{}", saved.error().message);
            notify(saved.error().message, true);
        }
    }
    if (ok) notify("Saved");
    updateTitle();
    return ok;
}

void StudioApp::runAfterSaveCheck(std::string actionLabel, std::function<void()> action) {
    if (!hasUnsavedChanges()) {
        action();
        return;
    }
    m_pending = PendingAction{std::move(actionLabel), std::move(action)};
    m_openSaveModal = true;
}

// --- Play mode ---------------------------------------------------------------------------------------

void StudioApp::startPlay() {
    if (!m_project || !m_document || m_play) return;
    if (m_settings.clearConsoleOnPlay) m_log.clear();
    auto play = editor::PlayMode::start(*m_project, m_document->scene());
    if (!play) {
        log::error("Cannot play: {}", play.error().message);
        notify("Cannot play: " + play.error().message, true);
        return;
    }
    m_play = std::move(play.value());
    m_playClock = FixedStepClock(1.0 / m_project->manifest().settings.tickRate);
    m_bindings = runtime::KeyBindings(m_project->manifest().input, m_play->session().actions());
    m_keysDown = {};
    m_keysPressed = {};
    m_previousHeld = 0;
    m_gameHasFocus = true;
    ImGui::SetWindowFocus(kSceneWindow);
    log::info("Playing '{}'", m_document->scene().name());
    updateTitle();
}

void StudioApp::stopPlay() {
    if (!m_play) return;
    const auto& state = m_play->session().state();
    log::info("Stopped after {:.1f} s: score {}, level complete: {}, {} script errors",
              m_play->session().elapsedSeconds(), state.score, state.levelComplete, state.scriptErrors);
    m_play.reset();
    updateTitle();
}

void StudioApp::togglePause() {
    if (m_play) m_play->setPaused(!m_play->paused());
}

void StudioApp::stepOnce() {
    if (!m_play) return;
    m_play->setPaused(true);
    m_play->requestStep();
}

void StudioApp::advancePlay(double elapsed) {
    if (!m_play) return;
    int steps = m_play->paused() ? 1 : m_playClock.advance(elapsed);
    for (int i = 0; i < steps; ++i) {
        std::uint32_t held = 0;
        std::uint32_t tapped = 0;
        if (m_gameHasFocus) {
            tapped = m_bindings.heldActions(m_keysPressed);
            held = m_bindings.heldActions(m_keysDown) | tapped;
        }
        m_keysPressed = {};
        const runtime::InputFrame frame{held, (held & ~m_previousHeld) | tapped};
        if (!m_play->advance(frame)) break; // paused
        m_previousHeld = held;
        for (const auto& event : m_play->session().events()) {
            if (event.type == runtime::GameEvent::Type::LevelCompleted) {
                log::info("Level complete after {:.2f} s: {}", m_play->session().elapsedSeconds(), event.text);
            } else if (event.type == runtime::GameEvent::Type::PlayerRespawned) {
                log::info("Player fell and respawned");
            }
        }
    }
}

// --- Misc commands -------------------------------------------------------------------------------------

void StudioApp::openScript(assets::AssetId script, int line) { m_scripts->open(*this, script, line); }

namespace {

bool nameContains(const std::string& name, std::string_view word) {
    std::string lower = name;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find(word) != std::string::npos;
}

} // namespace

assets::AssetId StudioApp::defaultTexture(editor::EntityPreset preset) const {
    if (!m_project) return {};
    std::string_view prefer;
    bool avoidTiles = false;
    switch (preset) {
        case editor::EntityPreset::Player: prefer = "player"; break;
        case editor::EntityPreset::Tilemap: prefer = "tile"; break;
        case editor::EntityPreset::SolidBlock: prefer = "platform"; break;
        default: avoidTiles = true; break;
    }
    assets::AssetId first;
    assets::AssetId fallback;
    for (const auto& e : m_project->assets().entries()) {
        if (e.type != assets::AssetType::Texture) continue;
        if (!prefer.empty() && nameContains(e.name, prefer)) return e.id;
        if (fallback.isNil()) fallback = e.id;
        if (first.isNil() && !(avoidTiles && nameContains(e.name, "tile"))) first = e.id;
    }
    return first.isNil() ? fallback : first;
}

scene::Entity StudioApp::createEntity(editor::EntityPreset preset, scene::Entity parent) {
    if (!m_document || m_play) return scene::kNullEntity;
    const scene::Scene& sc = m_document->scene();
    const Vec2 at = parent.isNull() ? newEntityPosition() : sc.worldPosition(parent);
    const bool usesTexture = preset == editor::EntityPreset::Sprite || preset == editor::EntityPreset::Tilemap ||
                             preset == editor::EntityPreset::SolidBlock || preset == editor::EntityPreset::Player;
    const assets::AssetId texture = usesTexture ? defaultTexture(preset) : assets::AssetId{};
    m_document->beginEdit(std::format("Create {}", editor::presetName(preset)));
    const scene::Entity e = m_document->createEntity(preset, {}, parent, at, texture);
    if (!e.isNull() && !texture.isNil() && m_textures) {
        // Size the sprite like its image; tilemaps get the tileset's column count.
        const Vec2i size = m_renderer2d->textureSize(m_textures->get(texture));
        scene::Scene& edit = m_document->sceneForEdit();
        if (size.x > 0 && size.y > 0) {
            if (auto* sprite = edit.tryGet<scene::Sprite>(e); sprite && preset != editor::EntityPreset::Player) {
                sprite->size = {static_cast<float>(size.x), static_cast<float>(size.y)};
                if (auto* box = edit.tryGet<scene::BoxCollider>(e); box && preset == editor::EntityPreset::SolidBlock) {
                    box->size = sprite->size;
                }
            }
            if (auto* map = edit.tryGet<scene::Tilemap>(e)) {
                map->tilesetColumns = std::max(1, static_cast<int>(static_cast<float>(size.x) / map->tileSize.x));
            }
            m_document->markChanged();
        }
    }
    // A tilemap's position is its top-left corner: centre the map in the view
    // and put its corner on a grid line, so tiles line up with the grid.
    if (const auto* map = e.isNull() ? nullptr : m_document->scene().tryGet<scene::Tilemap>(e); map && parent.isNull()) {
        Vec2 corner = m_viewport->viewCenter() - Vec2{static_cast<float>(map->width) * map->tileSize.x * 0.5f,
                                                      static_cast<float>(map->height) * map->tileSize.y * 0.5f};
        if (m_settings.snapToGrid) corner = editor::snapToGrid(corner, m_settings.gridSize);
        m_document->sceneForEdit().setWorldPosition(e, corner);
        m_document->markChanged();
    }
    m_document->endEdit();
    return e;
}

Vec2 StudioApp::placeOnGrid(Vec2 world) const {
    if (!m_settings.snapToGrid) return world;
    // The centre of the grid cell, where a sprite of the grid's size lines up with tiles.
    const float g = m_settings.gridSize;
    return {std::floor(world.x / g) * g + g * 0.5f, std::floor(world.y / g) * g + g * 0.5f};
}

Vec2 StudioApp::newEntityPosition() const { return placeOnGrid(m_viewport->viewCenter()); }

void StudioApp::frameSelection() { m_viewport->frameSelection(*this); }

void StudioApp::importFiles(const std::vector<std::filesystem::path>& files) {
    if (!m_project) {
        notify("Open a project before importing files", true);
        return;
    }
    int imported = 0;
    for (const auto& file : files) {
        auto id = m_project->importFile(file);
        if (!id) {
            log::error("{}", id.error().message);
            notify(id.error().message, true);
        } else {
            ++imported;
        }
    }
    if (imported > 0) {
        assetsChanged();
        notify(std::format("Imported {} file{}", imported, imported == 1 ? "" : "s"));
    }
}

void StudioApp::assetsChanged() {
    if (m_textures) m_textures->clear();
    if (m_project) m_project->refreshScripts();
}

void StudioApp::notify(std::string text, bool error) {
    const double now = static_cast<double>(SDL_GetTicksNS()) * 1.0e-9;
    m_toasts.push_back({std::move(text), error, now + (error ? 6.0 : 2.5)});
    if (m_toasts.size() > 4) m_toasts.erase(m_toasts.begin());
}

void StudioApp::openPath(const std::filesystem::path& path) {
    std::string url = "file://" + toUtf8(std::filesystem::absolute(path));
#ifdef _WIN32
    std::replace(url.begin(), url.end(), '\\', '/');
    if (url.size() > 7 && url[7] != '/') url.insert(7, "/");
#endif
    if (!SDL_OpenURL(url.c_str())) notify(std::format("Could not open {}: {}", toUtf8(path), SDL_GetError()), true);
}

void StudioApp::updateTitle() {
    std::string title = std::format("{} Studio", version::productName);
    if (m_project) {
        title = std::format("{} - {}", m_project->displayName(), title);
        if (m_document) {
            const auto* entry = m_project->asset(m_document->assetId());
            title = std::format("{}{} - {}", entry != nullptr ? entry->name : "?", hasUnsavedChanges() ? " *" : "", title);
        }
        if (m_play) title = "[Playing] " + title;
    }
    if (title != m_title) {
        m_title = title;
        SDL_SetWindowTitle(m_window, m_title.c_str());
    }
}

// --- Events ---------------------------------------------------------------------------------------------

StudioApp::Status StudioApp::handleEvent(const SDL_Event& event) {
    ImGui_ImplSDL3_ProcessEvent(&event);
    switch (event.type) {
        case SDL_EVENT_QUIT:
            requestQuit();
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (event.window.windowID == SDL_GetWindowID(m_window)) requestQuit();
            break;
        case SDL_EVENT_KEY_DOWN: {
            const Key key = platform::keyFromScancode(event.key.scancode);
            if (key != Key::Unknown) {
                if (!event.key.repeat) m_keysPressed[static_cast<std::size_t>(key)] = true;
                m_keysDown[static_cast<std::size_t>(key)] = true;
            }
            break;
        }
        case SDL_EVENT_KEY_UP: {
            const Key key = platform::keyFromScancode(event.key.scancode);
            if (key != Key::Unknown) m_keysDown[static_cast<std::size_t>(key)] = false;
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            m_keysDown = {};
            break;
        case SDL_EVENT_DROP_FILE:
            if (event.drop.data != nullptr) importFiles({fromUtf8(event.drop.data)});
            break;
        default:
            break;
    }
    return m_quit ? Status::Success : Status::Continue;
}

// --- Frame ----------------------------------------------------------------------------------------------

StudioApp::Status StudioApp::iterate() {
    const double now = static_cast<double>(SDL_GetTicksNS()) * 1.0e-9;
    double elapsed = now - m_lastTime;
    m_lastTime = now;
    if (m_options.smokeTest) elapsed = 1.0 / 60.0; // deterministic: one tick per frame

    handleDialogResults();
    m_scriptPollTimer += elapsed;
    if (m_project && m_scriptPollTimer > 1.0) {
        m_scriptPollTimer = 0.0;
        if (m_project->checkScriptsChangedOnDisk()) log::info("Scripts changed on disk; recompiled");
    }
    advancePlay(elapsed);

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    drawUi();
    if (m_options.smokeTest) runSmokeTest();
    ImGui::Render();

    const ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderTarget(m_sdlRenderer, nullptr);
    SDL_SetRenderScale(m_sdlRenderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColorFloat(m_sdlRenderer, 0.08f, 0.09f, 0.10f, 1.0f);
    SDL_RenderClear(m_sdlRenderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), m_sdlRenderer);
    if (m_options.smokeTest && m_smokeStep == -1 && !m_options.screenshotPath.empty()) {
        saveScreenshot(m_options.screenshotPath);
    }
    SDL_RenderPresent(m_sdlRenderer);
    ++m_frame;
    updateTitle();

    if (m_options.smokeTest && m_smokeStep == -1) {
        const std::size_t errors = m_log.count(log::Level::Error);
        if (errors > 0) {
            log::error("Smoke test: {} error(s) were logged", errors);
            return Status::Failure;
        }
        log::info("Smoke test passed");
        return Status::Success;
    }
    if (m_options.smokeTest && m_smokeStep == -2) return Status::Failure;
    return m_quit ? Status::Success : Status::Continue;
}

void StudioApp::handleDialogResults() {
    for (auto& result : m_dialogs.poll()) {
        if (!result.error.empty()) {
            log::warn("The system file dialog is not available ({}); type the path instead", result.error);
            notify("The file dialog is not available; type the path instead", true);
            continue;
        }
        if (result.paths.empty()) continue; // cancelled
        switch (result.purpose) {
            case FileDialogs::Purpose::OpenProjectFolder: requestOpenProject(result.paths.front()); break;
            case FileDialogs::Purpose::NewProjectParent: m_newParent = toUtf8(result.paths.front()); break;
            case FileDialogs::Purpose::ImportAssets: importFiles(result.paths); break;
        }
    }
}

void StudioApp::drawUi() {
    entityShortcuts = false; // set by the hierarchy and scene view when they have focus
    drawMenuBar();
    if (!m_project) {
        drawWelcome();
    } else {
        const ImGuiID dockspace = ImGui::GetID(kDockSpace);
        if (!m_layoutBuilt) {
            if (m_options.resetLayout || ImGui::DockBuilderGetNode(dockspace) == nullptr) buildDefaultLayout(dockspace);
            m_layoutBuilt = true;
        }
        ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport());
        m_hierarchy->draw(*this);
        m_inspector->draw(*this);
        m_viewport->draw(*this);
        m_assets->draw(*this);
        m_console->draw(*this);
        m_scripts->draw(*this);
        if (showProjectSettings) m_projectSettings->draw(*this, showProjectSettings);
        if (showDemoWindow) ImGui::ShowDemoWindow(&showDemoWindow);
    }
    handleShortcuts();
    drawModals();
    drawStatusToasts();
}

void StudioApp::buildDefaultLayout(unsigned int dockspace) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);
    ImGuiID center = dockspace;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.27f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
    ImGui::DockBuilderDockWindow(kHierarchyWindow, left);
    ImGui::DockBuilderDockWindow(kInspectorWindow, right);
    ImGui::DockBuilderDockWindow(kAssetsWindow, bottom);
    ImGui::DockBuilderDockWindow(kConsoleWindow, bottom);
    ImGui::DockBuilderDockWindow(kSceneWindow, center);
    ImGui::DockBuilderFinish(dockspace);
}

void StudioApp::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    const bool haveProject = m_project != nullptr;
    const bool haveDoc = m_document != nullptr;
    const bool editable = haveDoc && !m_play;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Project...")) requestCloseProject();
        if (ImGui::MenuItem("Open Project...", "Ctrl+O")) {
            m_dialogs.pickFolder(m_window, FileDialogs::Purpose::OpenProjectFolder);
        }
        if (ImGui::BeginMenu("Open Recent", !m_settings.recentProjects.empty())) {
            for (const auto& recent : m_settings.recentProjects) {
                if (ImGui::MenuItem(toUtf8(recent).c_str())) requestOpenProject(recent);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save All", "Ctrl+S", false, haveProject && !m_play)) saveAll();
        if (ImGui::MenuItem("Project Settings...", nullptr, false, haveProject)) showProjectSettings = true;
        if (ImGui::MenuItem("Show Project Folder", nullptr, false, haveProject)) openPath(m_project->root());
        ImGui::Separator();
        if (ImGui::MenuItem("Close Project", nullptr, false, haveProject)) requestCloseProject();
        if (ImGui::MenuItem("Quit", "Alt+F4")) requestQuit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const std::string undo = haveDoc && m_document->canUndo() ? "Undo " + m_document->undoLabel() : "Undo";
        const std::string redo = haveDoc && m_document->canRedo() ? "Redo " + m_document->redoLabel() : "Redo";
        if (ImGui::MenuItem(undo.c_str(), "Ctrl+Z", false, editable && m_document->canUndo())) m_document->undo();
        if (ImGui::MenuItem(redo.c_str(), "Ctrl+Y", false, editable && m_document->canRedo())) m_document->redo();
        ImGui::Separator();
        const bool hasSelection = haveDoc && !m_document->selection().empty();
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, editable && hasSelection)) {
            SDL_SetClipboardText(m_document->copySelection().c_str());
        }
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, editable && SDL_HasClipboardText())) {
            if (char* text = SDL_GetClipboardText()) {
                auto pasted = m_document->paste(text, &m_project->assets());
                if (!pasted) notify(pasted.error().message, true);
                SDL_free(text);
            }
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, editable && hasSelection)) m_document->duplicateSelection();
        if (ImGui::MenuItem("Delete", "Del", false, editable && hasSelection)) m_document->deleteSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", "Ctrl+A", false, haveDoc)) {
            std::vector<scene::Entity> all = m_document->scene().children(scene::kNullEntity);
            m_document->setSelection(all);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Create", editable)) {
        for (const auto preset : {editor::EntityPreset::Empty, editor::EntityPreset::Group, editor::EntityPreset::Sprite,
                                  editor::EntityPreset::Tilemap, editor::EntityPreset::SolidBlock,
                                  editor::EntityPreset::Trigger, editor::EntityPreset::Player,
                                  editor::EntityPreset::Camera}) {
            if (ImGui::MenuItem(std::string(editor::presetName(preset)).c_str())) createEntity(preset);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Scene", haveProject)) {
        for (const auto& entry : m_project->assets().entries()) {
            if (entry.type != assets::AssetType::Scene) continue;
            const bool current = haveDoc && m_document->assetId() == entry.id;
            const std::string label = entry.name + (entry.id == m_project->manifest().startScene ? "  (start)" : "");
            if (ImGui::MenuItem(label.c_str(), nullptr, current, !m_play)) requestOpenScene(entry.id);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Set as Start Scene", nullptr, false,
                            haveDoc && m_document->assetId() != m_project->manifest().startScene)) {
            if (auto ok = m_project->setStartScene(m_document->assetId()); !ok) notify(ok.error().message, true);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Play", haveDoc)) {
        if (ImGui::MenuItem(m_play ? "Stop" : "Play", "F5")) m_play ? stopPlay() : startPlay();
        if (ImGui::MenuItem("Pause", "F6", m_play && m_play->paused(), m_play != nullptr)) togglePause();
        if (ImGui::MenuItem("Step One Tick", "F7", false, m_play != nullptr)) stepOnce();
        ImGui::Separator();
        ImGui::MenuItem("Game Camera While Playing", nullptr, &gameCameraInPlay);
        ImGui::MenuItem("Clear Console On Play", nullptr, &m_settings.clearConsoleOnPlay);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Grid", nullptr, &m_settings.showGrid);
        ImGui::MenuItem("Colliders", nullptr, &m_settings.showColliders);
        ImGui::MenuItem("Snap To Grid", nullptr, &m_settings.snapToGrid);
        if (ImGui::MenuItem("Frame Selection", "F", false, haveDoc)) m_viewport->frameSelection(*this);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) {
            m_options.resetLayout = true;
            m_layoutBuilt = false;
        }
        if (ImGui::BeginMenu("UI Scale")) {
            for (const float s : {0.75f, 1.0f, 1.25f, 1.5f, 2.0f}) {
                if (ImGui::MenuItem(std::format("{:.0f}%", s * 100.0f).c_str(), nullptr, m_settings.uiScale == s)) {
                    m_settings.uiScale = s;
                    notify("The UI scale applies after restarting Studio");
                }
            }
            ImGui::EndMenu();
        }
        ImGui::MenuItem("Dear ImGui Demo", nullptr, &showDemoWindow);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        static const std::filesystem::path docs = findDocsFolder();
        if (ImGui::MenuItem("Studio Guide")) openPath(docs / "studio.md");
        if (ImGui::MenuItem("Scripting Guide")) openPath(docs / "scripting.md");
        if (ImGui::MenuItem("Scripting API Reference")) openPath(docs / "scripting-api.md");
        if (ImGui::MenuItem("Component Reference")) openPath(docs / "components.md");
        ImGui::Separator();
        ImGui::TextDisabled("%s Studio %s", version::productName, version::string);
        ImGui::EndMenu();
    }
    drawToolbar();
    ImGui::EndMainMenuBar();
}

void StudioApp::drawToolbar() {
    if (!m_document) return;
    // Play controls in the middle of the menu bar.
    const float width = ImGui::GetFontSize() * 14.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 20.0f, (ImGui::GetWindowWidth() - width) * 0.5f));
    if (m_play) {
        ImGui::PushStyleColor(ImGuiCol_Button, ui::kPlayColor);
        if (ImGui::Button(" Stop ")) stopPlay();
        ImGui::PopStyleColor();
        ui::tooltip("Stop the game and return to editing (F5). Changes made while playing are discarded.");
        if (ImGui::Button(m_play->paused() ? "Resume" : "Pause")) togglePause();
        ui::tooltip("F6");
        if (ImGui::Button("Step")) stepOnce();
        ui::tooltip("Advance one tick while paused (F7)");
        ImGui::TextColored(ui::kPlayColor, "PLAYING  %.1f s", m_play->session().elapsedSeconds());
    } else {
        if (ImGui::Button(" Play ")) startPlay();
        ui::tooltip("Play the scene as it is now, unsaved changes included (F5)");
    }
}

void StudioApp::handleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal) && !m_play) saveAll();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal)) {
        m_dialogs.pickFolder(m_window, FileDialogs::Purpose::OpenProjectFolder);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        if (m_play) {
            stopPlay();
        } else {
            startPlay();
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) togglePause();
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) stepOnce();
    if (!m_document || m_play || io.WantTextInput) return;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) m_document->undo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) {
        m_document->redo();
    }
    // Entity shortcuts only while the hierarchy or the scene view has focus.
    if (!entityShortcuts) return;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, ImGuiInputFlags_RouteGlobal)) m_document->duplicateSelection();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, ImGuiInputFlags_RouteGlobal)) {
        const std::string text = m_document->copySelection();
        if (!text.empty()) SDL_SetClipboardText(text.c_str());
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, ImGuiInputFlags_RouteGlobal)) {
        if (char* text = SDL_GetClipboardText()) {
            auto pasted = m_document->paste(text, &m_project->assets());
            if (!pasted) notify(pasted.error().message, true);
            SDL_free(text);
        }
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A, ImGuiInputFlags_RouteGlobal)) {
        m_document->setSelection(m_document->scene().children(scene::kNullEntity)); // top level: moves everything
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) m_document->deleteSelection();
}

void StudioApp::drawModals() {
    if (m_openSaveModal) {
        ImGui::OpenPopup("Unsaved changes");
        m_openSaveModal = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save %s before you %s?", unsavedSummary().c_str(), m_pending ? m_pending->label.c_str() : "continue");
        ImGui::Spacing();
        if (ImGui::Button("Save")) {
            ImGui::CloseCurrentPopup();
            if (saveAll() && m_pending) {
                auto action = std::move(m_pending->action);
                m_pending.reset();
                action();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save")) {
            ImGui::CloseCurrentPopup();
            if (m_project) {
                // Discard: forget unsaved script edits and settings.
                for (const auto& entry : m_project->assets().entries()) m_project->discardScriptBuffer(entry.id);
                if (m_project->manifestDirty()) (void)m_project->reloadManifest();
            }
            if (m_document) m_document.reset();
            if (m_pending) {
                auto action = std::move(m_pending->action);
                m_pending.reset();
                action();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_pending.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!m_errorModal.empty() && !ImGui::IsPopupOpen("Error")) ImGui::OpenPopup("Error");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSizeConstraints({ImGui::GetFontSize() * 20.0f, 0}, {ImGui::GetFontSize() * 45.0f, FLT_MAX});
    if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 42.0f);
        ImGui::TextUnformatted(m_errorModal.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_errorModal.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void StudioApp::drawStatusToasts() {
    const double now = static_cast<double>(SDL_GetTicksNS()) * 1.0e-9;
    std::erase_if(m_toasts, [now](const Toast& t) { return t.until < now; });
    if (m_toasts.empty()) return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x - 16.0f, viewport->WorkPos.y + viewport->WorkSize.y - 16.0f},
                            ImGuiCond_Always, {1.0f, 1.0f});
    ImGui::SetNextWindowBgAlpha(0.92f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##toasts", nullptr, flags)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        for (const Toast& t : m_toasts) {
            if (t.error) {
                ImGui::TextColored(ui::kErrorColor, "%s", t.text.c_str());
            } else {
                ImGui::TextUnformatted(t.text.c_str());
            }
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

void StudioApp::drawWelcome() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!ImGui::Begin("Welcome", nullptr, flags)) {
        ImGui::End();
        return;
    }
    const float em = ImGui::GetFontSize();
    const float columnWidth = std::min(em * 34.0f, ImGui::GetContentRegionAvail().x * 0.48f);
    ImGui::SetCursorPos({std::max(em, (ImGui::GetWindowWidth() - columnWidth * 2.0f - em * 2.0f) * 0.5f), em * 3.0f});
    ImGui::BeginGroup();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.8f);
    ImGui::TextUnformatted(std::format("{} Studio", version::productName).c_str());
    ImGui::PopFont();
    ImGui::TextColored(ui::kDimColor, "Build 2D games, then play them right here. Version %s", version::string);
    ImGui::Dummy({0, em});

    // New project.
    ImGui::BeginChild("new", {columnWidth, em * 22.0f}, ImGuiChildFlags_Borders);
    ImGui::TextColored(ui::kAccentColor, "New project");
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##name", "Project name", &m_newName);
    ImGui::TextUnformatted("Create it in this folder:");
    ImGui::SetNextItemWidth(-em * 6.0f);
    ImGui::InputTextWithHint("##parent", "e.g. C:/Users/me/Games", &m_newParent);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##parent")) {
        m_dialogs.pickFolder(m_window, FileDialogs::Purpose::NewProjectParent, fromUtf8(m_newParent));
    }
    ImGui::Spacing();
    if (ImGui::RadioButton("Platformer starter (art, player, coins, scripts)", m_newFromTemplate)) {
        m_newFromTemplate = true;
    }
    if (ImGui::RadioButton("Empty (one scene with a camera)", !m_newFromTemplate)) m_newFromTemplate = false;
    ImGui::Spacing();
    const std::string stem = editor::toFileStem(m_newName);
    const std::filesystem::path target = m_newParent.empty() || stem.empty()
                                             ? std::filesystem::path()
                                             : fromUtf8(m_newParent) / fromUtf8(m_newName);
    if (!target.empty()) ImGui::TextColored(ui::kDimColor, "%s", toUtf8(target).c_str());
    ImGui::BeginDisabled(target.empty());
    if (ImGui::Button("Create Project", {-1, 0})) requestNewProject(target, m_newName, m_newFromTemplate);
    ImGui::EndDisabled();
    ImGui::EndChild();

    ImGui::SameLine(0, em * 2.0f);

    // Open project.
    ImGui::BeginChild("open", {columnWidth, em * 22.0f}, ImGuiChildFlags_Borders);
    ImGui::TextColored(ui::kAccentColor, "Open a project");
    ImGui::Separator();
    if (ImGui::Button("Open Project Folder...", {-1, 0})) {
        m_dialogs.pickFolder(m_window, FileDialogs::Purpose::OpenProjectFolder);
    }
    ImGui::SetNextItemWidth(-em * 4.0f);
    ImGui::InputTextWithHint("##openpath", "or type the folder path", &m_openPath);
    ImGui::SameLine();
    ImGui::BeginDisabled(m_openPath.empty());
    if (ImGui::Button("Open")) requestOpenProject(fromUtf8(m_openPath));
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::TextUnformatted("Recent");
    ImGui::Separator();
    if (m_settings.recentProjects.empty()) ImGui::TextColored(ui::kDimColor, "No recent projects yet.");
    std::optional<std::filesystem::path> chosen;
    for (const auto& recent : m_settings.recentProjects) {
        const std::string label = toUtf8(recent.filename().empty() ? recent : recent.filename());
        if (ImGui::Selectable((label + "##" + toUtf8(recent)).c_str())) chosen = recent;
        ui::tooltip(toUtf8(recent));
    }
    if (chosen) requestOpenProject(*chosen);
    ImGui::EndChild();
    ImGui::EndGroup();
    ImGui::End();
}

// --- Smoke test -------------------------------------------------------------------------------------------
// A scripted run through the real UI code for the automated tests: every
// panel is drawn with real content, edits are made and undone, the game is
// played for two seconds. Nothing is written to the project.

void StudioApp::runSmokeTest() {
    if (m_smokeStep < 0) return;
    const auto fail = [this](const std::string& why) {
        log::error("Smoke test failed: {}", why);
        m_smokeStep = -2;
    };
    if (!m_project || !m_document) {
        fail("no project or scene is open");
        return;
    }
    switch (m_frame) {
        case 2: {
            const scene::Entity player = m_document->scene().findByName("Player");
            if (player.isNull()) return fail("the example scene has no Player");
            m_document->select(player);
            break;
        }
        case 4:
            for (const auto& entry : m_project->assets().entries()) {
                if (entry.type == assets::AssetType::Script) {
                    openScript(entry.id, 3);
                    break;
                }
            }
            showProjectSettings = true;
            break;
        case 6: {
            assets::AssetId texture;
            for (const auto& e : m_project->assets().entries()) {
                if (e.type == assets::AssetType::Texture) texture = e.id;
            }
            std::vector<scene::Entity> created;
            for (const auto preset : {editor::EntityPreset::Empty, editor::EntityPreset::Group,
                                      editor::EntityPreset::Sprite, editor::EntityPreset::Tilemap,
                                      editor::EntityPreset::SolidBlock, editor::EntityPreset::Trigger,
                                      editor::EntityPreset::Player, editor::EntityPreset::Camera}) {
                created.push_back(m_document->createEntity(preset, {}, scene::kNullEntity, {100, 100}, texture));
            }
            m_document->setSelection(created);
            break;
        }
        case 8: {
            m_document->duplicateSelection();
            const std::string clip = m_document->copySelection();
            if (auto pasted = m_document->paste(clip, &m_project->assets()); !pasted) return fail(pasted.error().message);
            m_document->select(m_document->scene().findByName("Level"));
            tool = Tool::Paint;
            break;
        }
        case 10:
            while (m_document->canUndo()) m_document->undo();
            if (m_document->isDirty()) return fail("undoing every step did not restore the saved scene");
            tool = Tool::Select;
            showProjectSettings = false;
            m_document->select(m_document->scene().findByName("Player"));
            break;
        case 12:
            startPlay();
            if (!m_play) return fail("play mode did not start");
            break;
        default:
            break;
    }
    if (m_play && m_frame > 12 && m_frame < 135) {
        m_gameHasFocus = true;
        m_keysDown[static_cast<std::size_t>(Key::Right)] = true;
    }
    if (m_frame == 135) {
        m_keysDown = {};
        if (!m_play) return fail("play mode stopped by itself");
        const auto& state = m_play->session().state();
        if (state.score < 1) return fail("the player did not collect a coin while walking right");
        if (state.scriptErrors > 0) return fail("script errors while playing");
        log::info("Smoke test: played {} ticks, score {}", m_play->session().tickCount(), state.score);
        stopPlay();
    }
    if (m_frame == 140) {
        if (m_document->isDirty()) return fail("playing changed the edited scene");
        m_smokeStep = -1; // done: screenshot (if any) this frame, then exit
    }
}

void StudioApp::saveScreenshot(const std::filesystem::path& path) {
    SDL_Surface* surface = SDL_RenderReadPixels(m_sdlRenderer, nullptr);
    if (surface == nullptr) {
        log::error("Could not read back the window: {}", SDL_GetError());
        return;
    }
    if (!SDL_SavePNG(surface, toUtf8(path).c_str())) {
        log::error("Could not save {}: {}", toUtf8(path), SDL_GetError());
    } else {
        log::info("Saved screenshot {}", toUtf8(path));
    }
    SDL_DestroySurface(surface);
}

} // namespace px::studio
