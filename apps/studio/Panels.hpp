#pragma once

#include "UiHelpers.hpp"

#include "px/assets/AssetId.hpp"
#include "px/core/Guid.hpp"
#include "px/project/Animation.hpp"
#include "px/project/Project.hpp"
#include "px/project/Tileset.hpp"
#include "px/scripting/Graph.hpp"
#include "px/render/Camera2D.hpp"
#include "px/render/Renderer2D.hpp"
#include "px/scene/ComponentInfo.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <string>
#include <vector>

// Studio's panels. Each draws one window with Dear ImGui and talks to the
// editor model through StudioApp; UI-only state lives here.
namespace px::studio {

class StudioApp;

// Window titles (also their ImGui ids, used to build the layouts). The
// names follow what many creators know from other UGC tools: the entity
// tree is the Explorer (HierarchyPanel), the selected entity's components
// are its Properties (InspectorPanel), and the log is the Output
// (ConsolePanel).
inline constexpr const char* kExplorerWindow = "Explorer";
inline constexpr const char* kPropertiesWindow = "Properties";
inline constexpr const char* kSceneWindow = "Scene";
inline constexpr const char* kAssetsWindow = "Assets";
inline constexpr const char* kOutputWindow = "Output";
inline constexpr const char* kToolboxWindow = "Toolbox";

class HierarchyPanel {
public:
    void draw(StudioApp& app);

private:
    void drawEntityMenu(StudioApp& app, scene::Entity entity);
    std::string m_filter;
    Guid m_renaming;
    std::string m_renameText;
    bool m_focusRename = false;
};

class InspectorPanel {
public:
    void draw(StudioApp& app);

private:
    void drawEntity(StudioApp& app, scene::Scene& scene, scene::Entity entity);
    void drawSceneSettings(StudioApp& app, scene::Scene& scene);
    void drawComponent(StudioApp& app, scene::Scene& scene, scene::Entity entity, const scene::ComponentInfo& info);
    bool drawField(StudioApp& app, scene::Scene& scene, const scene::ComponentInfo& info, const scene::FieldInfo& field,
                   void* component, bool readOnly);
    void drawTilemapTools(StudioApp& app, scene::Scene& scene, scene::Entity entity);
    void drawScripts(StudioApp& app, scene::Scene& scene, scene::Entity entity);
    void drawAddComponent(StudioApp& app, scene::Entity entity);
    void previewAnimator(StudioApp& app, scene::Scene& scene, scene::Entity entity);

    ui::EditSession m_edit;
    scene::Entity m_entity; // the entity whose component is being drawn
    std::string m_newTag;
    int m_resizeWidth = 0;
    int m_resizeHeight = 0;
    Guid m_resizeFor;
};

class ViewportPanel {
public:
    void draw(StudioApp& app);
    // Centres the editor camera on the selection (or the scene).
    void frameSelection(StudioApp& app);
    void resetCamera() { m_cameraReady = false; }
    [[nodiscard]] Vec2 viewCenter() const { return m_camera.center; }

private:
    void drawToolbar(StudioApp& app);
    void drawSceneView(StudioApp& app, ImVec2 size);
    void drawGameView(StudioApp& app, ImVec2 size);
    void drawOverlays(StudioApp& app, scene::Scene& scene, ImVec2 origin, ImVec2 size);
    void handleInput(StudioApp& app, scene::Scene& scene, ImVec2 origin, ImVec2 size);
    void handleDrop(StudioApp& app, ImVec2 origin);
    [[nodiscard]] Vec2 screenToWorld(ImVec2 screen, ImVec2 origin) const;
    [[nodiscard]] ImVec2 worldToScreen(Vec2 world, ImVec2 origin) const;
    void ensureTarget(StudioApp& app, Vec2i size, render::TextureHandle& target, Vec2i& current);

    render::Camera2D m_camera;   // editor camera (framebuffer pixels)
    float m_dpi = 1.0f;          // framebuffer pixels per ImGui pixel
    bool m_cameraReady = false;
    int m_initFrames = 0;
    render::TextureHandle m_sceneTarget = render::kNoTexture;
    Vec2i m_sceneTargetSize{};
    render::TextureHandle m_gameTarget = render::kNoTexture;
    Vec2i m_gameTargetSize{};

    enum class Drag { None, Move, Pan, Marquee, Paint, TileRect };
    Drag m_drag = Drag::None;
    Vec2 m_dragStartWorld{};
    Vec2 m_dragApplied{};       // offset already applied during a move
    ImVec2 m_dragStartScreen{};
    int m_lastPaintX = 0;
    int m_lastPaintY = 0;
    int m_rectStartX = 0; // TileRect: the cell where the drag began
    int m_rectStartY = 0;
    bool m_painted = false;
    Vec2 m_lastClickWorld{};
    std::size_t m_clickCycle = 0;
    bool m_hovered = false;
};

// Ready-made objects, images and behaviours from the starter library.
// Click to add one in the middle of the view; drag into the Scene view to
// place it; drag a behaviour onto an entity to attach it.
class ToolboxPanel {
public:
    void draw(StudioApp& app);

private:
    std::string m_filter;
    bool m_allTypes = false;
};

class AssetsPanel {
public:
    void draw(StudioApp& app);

private:
    void drawAssetMenu(StudioApp& app, const assets::AssetEntry& entry);
    std::string m_filter;
    int m_typeFilter = 0; // 0 all, 1 textures, 2 scripts, 3 scenes
    assets::AssetId m_renaming;
    std::string m_renameText;
    assets::AssetId m_deleting;
    assets::AssetId m_showUses;
    std::string m_newScriptName = "New Behaviour";
    std::string m_newSceneName = "New Scene";
    std::string m_newAnimationName = "Walk";
    assets::AssetId m_newAnimationImage;
    int m_newAnimationCell[2] = {16, 16};
    bool m_openNewAnimation = false;
    std::string m_newGraphName = "My Visual Script";
    bool m_openNewGraph = false;
    std::string m_importPath;
    bool m_openNewScript = false;
    bool m_openNewScene = false;
};

class ConsolePanel {
public:
    void draw(StudioApp& app);

private:
    std::string m_filter;
    bool m_showInfo = true;
    bool m_showWarnings = true;
    bool m_showErrors = true;
    bool m_autoScroll = true;
    std::uint64_t m_seenRevision = 0;
};

class ScriptEditorPanel {
public:
    void draw(StudioApp& app);
    void open(StudioApp& app, assets::AssetId script, int line);
    [[nodiscard]] bool hasOpenScripts() const { return !m_open.empty(); }
    // ImGui window ids of the open scripts ("###script-<id>"), so a layout
    // reset can dock them next to the Scene view again.
    [[nodiscard]] std::vector<std::string> windowIds() const;

private:
    struct OpenScript {
        assets::AssetId id;
        std::string text;
        int gotoLine = 0;
        bool focus = false;
        int cursorLine = 1;
        bool changedOnDisk = false;
    };
    std::vector<OpenScript> m_open;
};

// Edits one animation clip: frames picked from a sprite sheet on a grid,
// their order, speed and looping, with a live preview. Saves as it goes.
class AnimationEditorPanel {
public:
    void draw(StudioApp& app);
    void open(StudioApp& app, assets::AssetId clip);
    void close();
    [[nodiscard]] bool isOpen() const { return m_open; }
    [[nodiscard]] assets::AssetId clip() const { return m_id; }
    [[nodiscard]] static const char* windowId() { return "###animation-editor"; }

private:
    void drawContents(StudioApp& app);
    void changed();

    assets::AssetId m_id;
    project::AnimationClip m_clip;
    int m_selected = -1;
    bool m_open = false;
    bool m_focus = false;
    bool m_pendingSave = false;
    double m_previewStart = 0.0;
};

// Edits what the tiles of a tileset image are: collision (solid, none,
// one-way, slopes) and tags, tile by tile. Saves as it goes.
class TilesetEditorPanel {
public:
    void draw(StudioApp& app);
    void open(StudioApp& app, assets::AssetId tileset);
    void close();
    [[nodiscard]] static const char* windowId() { return "###tileset-editor"; }

private:
    void drawContents(StudioApp& app);

    assets::AssetId m_id;
    project::Tileset m_tileset;
    std::vector<int> m_selected; // tile indices
    std::string m_newTag;
    bool m_open = false;
    bool m_focus = false;
    bool m_pendingSave = false;
};

// Edits a visual script: nodes on a canvas connected pin to pin, the
// selected node's values, the graph's properties; shows problems and the
// Luau it compiles to. Saves as it goes; has its own undo.
class GraphEditorPanel {
public:
    void draw(StudioApp& app);
    void open(StudioApp& app, assets::AssetId graph);
    void close();
    [[nodiscard]] static const char* windowId() { return "###graph-editor"; }

private:
    struct Layout;
    enum class Drag { None, Pan, Nodes, Link, Box };

    void drawContents(StudioApp& app);
    void drawCanvas(StudioApp& app);
    void drawSidePanel(StudioApp& app);
    void drawAddMenu(StudioApp& app);
    void addNode(StudioApp& app, const scripting::NodeTypeInfo& type);
    void connect(StudioApp& app, const std::string& fromNode, const std::string& fromPin, const std::string& toNode,
                 const std::string& toPin);
    void deleteSelected();
    void duplicateSelected();
    void beginChange(); // records an undo step before a change
    void changed();     // marks for saving and recompiling
    void undo();
    void redo();
    [[nodiscard]] Layout layout(const scripting::GraphNode& node) const;
    [[nodiscard]] ImVec2 toScreen(ImVec2 graph) const;
    [[nodiscard]] ImVec2 toGraph(ImVec2 screen) const;
    [[nodiscard]] ImVec2 pinPosition(const scripting::GraphNode& node, const std::string& pin, bool output) const;

    assets::AssetId m_id;
    scripting::Graph m_graph;
    scripting::CompiledGraph m_compiled;
    std::vector<scripting::Graph> m_undo;
    std::vector<scripting::Graph> m_redo;
    std::vector<std::string> m_selected; // node ids
    ImVec2 m_origin{};      // screen position of the canvas
    ImVec2 m_pan{};         // graph offset
    float m_zoom = 1.0f;
    Drag m_drag = Drag::None;
    ImVec2 m_dragStart{};   // graph space
    bool m_dragMoved = false;
    float m_panMoved = 0.0f;
    std::string m_linkNode; // the pin a link is being dragged from
    std::string m_linkPin;
    bool m_linkFromOutput = true;
    ImVec2 m_addAt{};       // where the add menu puts the node (graph space)
    std::optional<std::tuple<std::string, std::string, bool>> m_addFrom; // node, pin, is output
    std::string m_search;
    bool m_openAdd = false;
    bool m_focusSearch = false;
    bool m_open = false;
    bool m_focus = false;
    bool m_pendingSave = false;
    bool m_recompile = true;
    bool m_showCode = false;
    bool m_editing = false; // a value field is being typed in (one undo step)
};

class ProjectSettingsPanel {
public:
    void draw(StudioApp& app, bool& open);

private:
    bool m_loaded = false;
    project::ProjectManifest m_edit;
    std::vector<std::string> m_keyTexts;
    std::string m_error;
};

} // namespace px::studio
