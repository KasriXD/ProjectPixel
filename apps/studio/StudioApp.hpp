#pragma once

#include "FileDialogs.hpp"
#include "StudioSettings.hpp"

#include "px/core/Keys.hpp"
#include "px/core/Result.hpp"
#include "px/core/Time.hpp"
#include "px/core/FileSystem.hpp"
#include "px/editor/EditorProject.hpp"
#include "px/editor/Library.hpp"
#include "px/editor/LogBuffer.hpp"
#include "px/editor/PlayMode.hpp"
#include "px/editor/SceneDocument.hpp"
#include "px/platform_sdl/SdlRenderer2D.hpp"
#include "px/render/TextureCache.hpp"
#include "px/runtime/Input.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

union SDL_Event;
struct SDL_Window;
struct SDL_Renderer;
struct ImFont;

namespace px::studio {

class HierarchyPanel;
class InspectorPanel;
class ViewportPanel;
class AssetsPanel;
class ConsolePanel;
class ScriptEditorPanel;
class ProjectSettingsPanel;
class ToolboxPanel;

struct StudioOptions {
    std::filesystem::path projectDir; // open this project at start
    bool smokeTest = false;           // scripted UI run for automated tests (see docs/studio.md)
    std::filesystem::path screenshotPath;
    bool resetLayout = false;
    bool verbose = false;
};

[[nodiscard]] std::optional<StudioOptions> parseStudioArgs(int argc, char** argv, std::string& error);
[[nodiscard]] const char* studioUsage();

// Editing tools of the scene viewport.
enum class Tool { Select, Pan, Paint, Erase };

// Studio: the desktop editor. Owns the window, Dear ImGui, the open project
// and scene (editor::EditorProject / SceneDocument), play mode and the
// panels. Panels draw the UI and call the editor model; they keep only UI
// state (filters, text being typed), so the UI can be replaced without
// touching the model. Driven by SDL's main callbacks like px-player.
class StudioApp {
public:
    enum class Status { Continue, Success, Failure };

    [[nodiscard]] static Result<std::unique_ptr<StudioApp>> create(StudioOptions options);
    ~StudioApp();
    StudioApp(const StudioApp&) = delete;
    StudioApp& operator=(const StudioApp&) = delete;

    [[nodiscard]] Status iterate();
    [[nodiscard]] Status handleEvent(const SDL_Event& event);

    // --- State the panels use ------------------------------------------------------------
    [[nodiscard]] editor::EditorProject* project() { return m_project.get(); }
    [[nodiscard]] editor::SceneDocument* document() { return m_document.get(); }
    [[nodiscard]] editor::PlayMode* play() { return m_play.get(); }
    [[nodiscard]] bool playing() const { return m_play != nullptr; }
    // The scene shown and inspected: the running game's copy while playing.
    [[nodiscard]] scene::Scene* activeScene();
    [[nodiscard]] editor::LogBuffer& log() { return m_log; }
    [[nodiscard]] platform::SdlRenderer2D& renderer() { return *m_renderer2d; }
    [[nodiscard]] render::TextureCache* textures() { return m_textures.get(); }
    [[nodiscard]] SDL_Window* window() const { return m_window; }
    [[nodiscard]] StudioSettings& settings() { return m_settings; }
    [[nodiscard]] FileDialogs& dialogs() { return m_dialogs; }
    [[nodiscard]] ImFont* monoFont() const { return m_monoFont; }
    [[nodiscard]] float uiScale() const { return m_uiScale; }

    // Selected entities of the active scene. While playing, the editor
    // selection (kept by id) is mapped onto the running copy.
    [[nodiscard]] std::vector<scene::Entity> selection();
    [[nodiscard]] scene::Entity primarySelection();
    void select(scene::Entity entity, editor::SelectMode mode = editor::SelectMode::Replace);

    Tool tool = Tool::Select;
    int paintTile = 0;              // tile index used by the paint tool
    bool gameCameraInPlay = true;   // viewport shows the game camera while playing
    bool showProjectSettings = false;
    bool showDemoWindow = false;
    bool entityShortcuts = false;   // the focused panel wants Delete, Ctrl+D, Ctrl+C/V
    // Where new entities appear: the centre of the scene view, on the grid.
    [[nodiscard]] Vec2 newEntityPosition() const;
    // With snapping on: the centre of the grid cell containing `world`.
    [[nodiscard]] Vec2 placeOnGrid(Vec2 world) const;
    // Creates an entity from a preset as one undo step, at the centre of the
    // view (or on its parent), with a fitting image from the project.
    scene::Entity createEntity(editor::EntityPreset preset, scene::Entity parent = scene::kNullEntity);
    [[nodiscard]] assets::AssetId defaultTexture(editor::EntityPreset preset) const;
    void frameSelection();

    // --- Commands ------------------------------------------------------------------------
    void requestOpenProject(const std::filesystem::path& folder);
    // Creates a project of `type`: a copy of that type's example level, or an empty scene.
    void requestNewProject(const std::filesystem::path& folder, const std::string& name, project::GameType type,
                           bool fromExample);
    void requestCloseProject();
    void requestOpenScene(assets::AssetId scene);
    void requestQuit();
    // Saves the scene, unsaved scripts and project settings. Returns false on failure.
    bool saveAll();
    [[nodiscard]] bool hasUnsavedChanges() const;
    [[nodiscard]] std::string unsavedSummary() const;

    // Play and Stop as UI commands: applied between frames, so nothing drawn
    // later in the same frame uses a game that no longer exists.
    void requestStartPlay();
    void requestStopPlay();
    void togglePause();
    void stepOnce();

    void openScript(assets::AssetId script, int line = 0);
    void importFiles(const std::vector<std::filesystem::path>& files);
    void assetsChanged(); // textures and script info must be refreshed
    void notify(std::string text, bool error = false);
    void openPath(const std::filesystem::path& path); // in the system's file manager or editor

    // Keyboard state for the game, fed only while the viewport has focus.
    void setGameInputFocus(bool focused) { m_gameHasFocus = focused; }
    // The example project for a game type (empty if it was not found).
    [[nodiscard]] std::filesystem::path exampleFolder(project::GameType type) const;
    [[nodiscard]] project::GameType gameType() const {
        return m_project ? m_project->manifest().settings.gameType : project::GameType::Platformer;
    }

    // --- Starter library (Toolbox) ------------------------------------------------------
    [[nodiscard]] const editor::Library* library() const { return m_library ? &*m_library : nullptr; }
    [[nodiscard]] const std::string& libraryError() const { return m_libraryError; }
    [[nodiscard]] render::TextureCache* libraryTextures() { return m_libraryTextures.get(); }
    // Each inserts at `at` (or the middle of the view) as one undo step, selects
    // the result and reports problems as notifications.
    void insertLibraryObject(std::size_t index, std::optional<Vec2> at = std::nullopt);
    void insertLibraryImage(const assets::AssetId& image, std::optional<Vec2> at = std::nullopt);
    void attachLibraryScript(const assets::AssetId& script, scene::Entity entity);
    // The library's player object for the project's game type (-1 if none).
    [[nodiscard]] int libraryPlayerObject() const;

private:
    explicit StudioApp(StudioOptions options);
    Status initWindowAndUi();

    void beginFrame();
    void drawUi();
    void drawMenuBar();
    void drawToolbar();
    void drawWelcome();
    void drawStatusToasts();
    void drawModals();
    void buildDefaultLayout(unsigned int dockspace);
    void handleShortcuts();
    void handleDialogResults();
    void advancePlay(double elapsed);
    void updateTitle();
    void runSmokeTest();
    void saveScreenshot(const std::filesystem::path& path);

    void startPlay();
    void stopPlay();
    // Runs `action` between frames: changes that replace the project, scene
    // or game must not happen while panels are drawing them.
    void defer(std::function<void()> action) { m_deferred.push_back(std::move(action)); }
    void runDeferred();
    // Runs `action` (between frames), after asking to save unsaved changes if any.
    void runAfterSaveCheck(std::string actionLabel, std::function<void()> action);
    void openProjectNow(const std::filesystem::path& folder);
    void closeProjectNow();
    void openSceneNow(assets::AssetId scene);

    StudioOptions m_options;
    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_sdlRenderer = nullptr;
    std::unique_ptr<platform::SdlRenderer2D> m_renderer2d;
    bool m_imguiReady = false;
    ImFont* m_monoFont = nullptr;
    float m_uiScale = 1.0f;
    std::filesystem::path m_prefDir;
    std::filesystem::path m_settingsFile;
    std::string m_iniPath;
    std::filesystem::path m_examplesFolder;
    std::optional<editor::Library> m_library;
    std::string m_libraryError;
    std::unique_ptr<DirectoryFileSystem> m_libraryFiles;
    assets::AssetRegistry m_libraryRegistry;
    std::unique_ptr<render::TextureCache> m_libraryTextures;

    StudioSettings m_settings;
    FileDialogs m_dialogs;
    editor::LogBuffer m_log;

    std::unique_ptr<editor::EditorProject> m_project;
    std::unique_ptr<editor::SceneDocument> m_document;
    std::unique_ptr<render::TextureCache> m_textures;
    std::unique_ptr<editor::PlayMode> m_play;
    FixedStepClock m_playClock{1.0 / 60.0};
    runtime::KeyBindings m_bindings;
    KeyStates m_keysDown{};
    KeyStates m_keysPressed{};
    std::uint32_t m_previousHeld = 0;
    bool m_gameHasFocus = false;

    std::unique_ptr<HierarchyPanel> m_hierarchy;
    std::unique_ptr<InspectorPanel> m_inspector;
    std::unique_ptr<ViewportPanel> m_viewport;
    std::unique_ptr<AssetsPanel> m_assets;
    std::unique_ptr<ConsolePanel> m_console;
    std::unique_ptr<ScriptEditorPanel> m_scripts;
    std::unique_ptr<ProjectSettingsPanel> m_projectSettings;
    std::unique_ptr<ToolboxPanel> m_toolbox;

    struct Toast {
        std::string text;
        bool error = false;
        double until = 0.0;
    };
    std::vector<Toast> m_toasts;

    // "Save changes?" confirmation.
    struct PendingAction {
        std::string label;
        std::function<void()> action;
    };
    std::optional<PendingAction> m_pending;
    std::vector<std::function<void()>> m_deferred;
    bool m_openSaveModal = false;
    std::string m_errorModal;

    // Welcome screen inputs.
    std::string m_newName = "My Game";
    std::string m_newParent;
    bool m_newFromTemplate = true;
    project::GameType m_newGameType = project::GameType::Platformer;
    std::string m_openPath;

    bool m_layoutBuilt = false;
    bool m_quit = false;
    double m_lastTime = 0.0;
    double m_scriptPollTimer = 0.0;
    int m_frame = 0;
    int m_smokeStep = 0;
    std::string m_title;
};

} // namespace px::studio
