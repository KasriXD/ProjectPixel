#pragma once

#include "px/assets/AssetRegistry.hpp"
#include "px/core/FileSystem.hpp"
#include "px/core/Result.hpp"
#include "px/project/Project.hpp"
#include "px/scene/Scene.hpp"
#include "px/scripting/ScriptLibrary.hpp"
#include "px/scripting/ScriptSystem.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The editor model: everything Studio does to a project, without any UI.
// Studio's panels call these classes; tests drive them directly. See
// docs/studio.md and docs/adr/0015-studio-architecture.md.
namespace px::editor {

// Where an asset is used, for "find references" and safe deletion.
struct AssetUse {
    assets::AssetId scene; // the scene that uses it
    std::string sceneName;
    std::string entityName;
};

// A script's compile result and what it declares (for the inspector).
struct ScriptInfo {
    bool compiled = false;
    std::string error; // compile or load error, empty if fine
    scripting::BehaviourDescription behaviour;
};

// An open project folder.
//
// Asset operations (import, create, rename, delete) change files on disk
// and save project.json immediately, so the folder is always consistent.
// Settings edits (manifest()) are kept in memory until saveManifest().
class EditorProject {
public:
    // Opens a folder containing project.json.
    [[nodiscard]] static Result<std::unique_ptr<EditorProject>> open(const std::filesystem::path& folder);

    // Creates a new project in `folder`, which must not exist or be empty.
    // With a template folder (a project), its files are copied and the
    // project gets a new id and the given name; otherwise an empty project
    // with one scene containing a camera is created.
    [[nodiscard]] static Result<std::unique_ptr<EditorProject>> create(
        const std::filesystem::path& folder, std::string name,
        const std::optional<std::filesystem::path>& templateFolder = std::nullopt);

    [[nodiscard]] const std::filesystem::path& root() const { return m_root; }
    [[nodiscard]] const IFileSystem& files() const { return m_files; }
    [[nodiscard]] std::string displayName() const { return m_manifest.name; }

    // --- Manifest (settings) -----------------------------------------------------------
    [[nodiscard]] const project::ProjectManifest& manifest() const { return m_manifest; }
    // For settings edits; call markManifestDirty() after changing it.
    [[nodiscard]] project::ProjectManifest& manifestForEdit() { return m_manifest; }
    void markManifestDirty() { m_manifestDirty = true; }
    [[nodiscard]] bool manifestDirty() const { return m_manifestDirty; }
    // Validates (by parsing the written text back) and writes project.json.
    [[nodiscard]] Status saveManifest();
    // Re-reads project.json from disk, discarding unsaved settings edits.
    [[nodiscard]] Status reloadManifest();

    // --- Assets ----------------------------------------------------------------------------
    [[nodiscard]] const assets::AssetRegistry& assets() const { return m_manifest.assets; }
    [[nodiscard]] const assets::AssetEntry* asset(assets::AssetId id) const { return m_manifest.assets.find(id); }

    // Copies a file from outside the project into it and registers it.
    // The type comes from the extension: .png .jpg .jpeg .bmp .tga (texture),
    // .luau (script), .scene.json (scene). Images are decoded first, so a
    // broken image is refused.
    [[nodiscard]] Result<assets::AssetId> importFile(const std::filesystem::path& source);
    // Creates scripts/<name>.luau from a template behaviour.
    [[nodiscard]] Result<assets::AssetId> createScript(std::string_view name);
    // Creates scenes/<name>.scene.json containing a camera.
    [[nodiscard]] Result<assets::AssetId> createScene(std::string_view name);
    [[nodiscard]] Status renameAsset(assets::AssetId id, std::string name);
    // Removes an unused asset from the project and deletes its file.
    // Refused while scenes use it (see findUses) or for the start scene.
    [[nodiscard]] Status deleteAsset(assets::AssetId id, const std::vector<std::pair<assets::AssetId, const scene::Scene*>>&
                                                             openScenes = {});
    // Scenes and entities that use the asset. Scenes are read from disk
    // unless given in `openScenes` (unsaved editor state).
    [[nodiscard]] std::vector<AssetUse> findUses(
        assets::AssetId id, const std::vector<std::pair<assets::AssetId, const scene::Scene*>>& openScenes = {}) const;
    [[nodiscard]] Status setStartScene(assets::AssetId id);

    // Full path of an asset's file on disk (for "open in external editor").
    [[nodiscard]] std::filesystem::path absolutePath(assets::AssetId id) const;

    // --- Scenes ----------------------------------------------------------------------------
    [[nodiscard]] Result<scene::Scene> loadScene(assets::AssetId id, std::vector<std::string>* warnings = nullptr) const;
    // Validates and writes a scene file.
    [[nodiscard]] Status saveScene(assets::AssetId id, const scene::Scene& scene);

    // --- Scripts ---------------------------------------------------------------------------
    // Text of a script as Studio's script editor shows it: the unsaved
    // buffer if there is one, otherwise the file.
    [[nodiscard]] Result<std::string> scriptText(assets::AssetId id) const;
    // Keeps unsaved script text in memory; play mode and the inspector use it.
    void setScriptBuffer(assets::AssetId id, std::string text);
    void discardScriptBuffer(assets::AssetId id);
    [[nodiscard]] bool scriptBufferDirty(assets::AssetId id) const;
    [[nodiscard]] bool anyScriptBufferDirty() const;
    [[nodiscard]] Status saveScript(assets::AssetId id);
    [[nodiscard]] Status saveAllScripts();

    // Compiles every script (unsaved buffers included); one broken script
    // does not hide the others. Called on demand and cached.
    void refreshScripts();
    [[nodiscard]] const ScriptInfo* scriptInfo(assets::AssetId id);
    // True (once) if a script file was changed by another program since the
    // last refresh; the next scriptInfo() call recompiles.
    [[nodiscard]] bool checkScriptsChangedOnDisk();
    // All scripts compiled together, for play mode. Fails on the first error.
    [[nodiscard]] Result<std::shared_ptr<const scripting::ScriptLibrary>> buildScriptLibrary() const;

private:
    EditorProject(std::filesystem::path root, project::ProjectManifest manifest);

    [[nodiscard]] Result<std::string> uniquePath(std::string_view folder, std::string_view stem,
                                                 std::string_view extension) const;
    [[nodiscard]] Result<std::string> readSource(assets::AssetId id) const;
    [[nodiscard]] ScriptInfo describeScript(const assets::AssetEntry& entry) const;

    std::filesystem::path m_root;
    DirectoryFileSystem m_files;
    project::ProjectManifest m_manifest;
    bool m_manifestDirty = false;

    struct ScriptBuffer {
        std::string text;
        bool dirty = false;
    };
    std::map<assets::AssetId, ScriptBuffer> m_scriptBuffers;
    std::map<assets::AssetId, ScriptInfo> m_scriptInfo;
    std::map<assets::AssetId, std::filesystem::file_time_type> m_scriptTimes;
    std::set<assets::AssetId> m_staleScripts; // changed since last described
    bool m_scriptsStale = true;               // everything must be described again
};

// "Enemy Patrol" -> "enemy_patrol" (file names); empty if nothing usable.
[[nodiscard]] std::string toFileStem(std::string_view name);
// "enemy patrol" -> "EnemyPatrol" (Luau identifiers); "Behaviour" if nothing usable.
[[nodiscard]] std::string toIdentifier(std::string_view name);

} // namespace px::editor
