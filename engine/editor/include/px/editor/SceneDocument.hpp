#pragma once

#include "px/assets/AssetRegistry.hpp"
#include "px/core/Guid.hpp"
#include "px/core/Result.hpp"
#include "px/scene/ComponentInfo.hpp"
#include "px/scene/Scene.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace px::editor {

class EditorProject;

// Starting points for new entities (Studio's "Create" menu).
enum class EntityPreset {
    Empty,      // Transform only
    Group,      // no components: organises children, which keep their own positions
    Sprite,     // Transform + Sprite
    Tilemap,    // Transform + Tilemap (20 x 10 empty tiles)
    SolidBlock, // Transform + Sprite (if a texture is given) + solid BoxCollider
    Trigger,    // Transform + trigger BoxCollider
    Player,     // a ready-to-play platformer character
    Camera,     // Transform + Camera (follows the player if there is one)
};
[[nodiscard]] std::string_view presetName(EntityPreset preset);

enum class SelectMode { Replace, Add, Toggle };

// One scene open in the editor: the scene, the selection, and undo/redo.
//
// Every change goes through an edit transaction: beginEdit() saves a
// snapshot of the scene, the change is made, markChanged() records that
// something changed, and endEdit() turns it into one undo step. The
// convenience operations below wrap themselves in a transaction; Studio's
// inspector and viewport drags open one when a widget becomes active and
// close it when it is released, so a whole drag is a single undo step.
//
// Undo stores whole-scene snapshots (scenes are copied cheaply; see ADR
// 0015). Their memory is bounded; the oldest steps are dropped first.
class SceneDocument {
public:
    SceneDocument(assets::AssetId sceneId, scene::Scene scene);

    [[nodiscard]] assets::AssetId assetId() const { return m_sceneId; }
    [[nodiscard]] const scene::Scene& scene() const { return m_scene; }
    // Mutable access for edits. Only use inside beginEdit()/endEdit().
    [[nodiscard]] scene::Scene& sceneForEdit() { return m_scene; }

    // --- Transactions -----------------------------------------------------------------------
    void beginEdit(std::string_view label);
    void markChanged() { ++m_changes; }
    void endEdit();
    // Ends the transaction and restores the snapshot (e.g. Escape during a drag).
    void cancelEdit();
    [[nodiscard]] bool inEdit() const { return m_editDepth > 0; }

    // --- Undo -----------------------------------------------------------------------------------
    [[nodiscard]] bool canUndo() const { return !m_undo.empty(); }
    [[nodiscard]] bool canRedo() const { return !m_redo.empty(); }
    [[nodiscard]] std::string undoLabel() const;
    [[nodiscard]] std::string redoLabel() const;
    void undo();
    void redo();
    [[nodiscard]] std::size_t undoSteps() const { return m_undo.size(); }
    // Upper bound for the memory of undo snapshots.
    void setUndoMemoryLimit(std::size_t bytes) { m_undoLimit = bytes; }

    // --- Saving -----------------------------------------------------------------------------------
    [[nodiscard]] bool isDirty() const { return m_version != m_savedVersion; }
    [[nodiscard]] Status save(EditorProject& project);
    // Problems that would stop the scene from loading, per entity (shown by Studio).
    [[nodiscard]] std::vector<std::pair<scene::Entity, scene::ComponentProblem>> problems(
        const assets::AssetRegistry* assets) const;

    // --- Selection (kept by id, so it survives undo and redo) -----------------------------
    [[nodiscard]] std::vector<scene::Entity> selection() const; // live entities, hierarchy order
    [[nodiscard]] scene::Entity primarySelection() const;       // the last one clicked
    [[nodiscard]] bool isSelected(scene::Entity entity) const;
    void select(scene::Entity entity, SelectMode mode = SelectMode::Replace);
    void setSelection(const std::vector<scene::Entity>& entities);
    void clearSelection();

    // --- Operations (each is one undo step) --------------------------------------------------
    scene::Entity createEntity(EntityPreset preset, std::string name = {}, scene::Entity parent = scene::kNullEntity,
                               Vec2 position = {}, assets::AssetId texture = {});
    void rename(scene::Entity entity, std::string name);
    // Deletes the selected entities (with their children). Clears entity
    // references to them (e.g. a camera following a deleted player).
    void deleteSelection();
    // Copies of the selected entities placed after their originals; the
    // copies become the selection.
    std::vector<scene::Entity> duplicateSelection();
    // Clipboard text ("px.entities" document) of the selected entities.
    [[nodiscard]] std::string copySelection() const;
    // Pastes clipboard text under `parent`; the pasted entities become the selection.
    [[nodiscard]] Result<std::vector<scene::Entity>> paste(std::string_view text, const assets::AssetRegistry* assets,
                                                          scene::Entity parent = scene::kNullEntity);
    [[nodiscard]] Status setParent(scene::Entity entity, scene::Entity parent, scene::Entity before = scene::kNullEntity);
    // Adds the component and any it requires (PlatformerController brings
    // Transform, BoxCollider and KinematicBody).
    [[nodiscard]] Status addComponent(scene::Entity entity, const scene::ComponentInfo& component);
    // Refused while another component on the entity requires it.
    [[nodiscard]] Status removeComponent(scene::Entity entity, const scene::ComponentInfo& component);
    [[nodiscard]] Status attachScript(scene::Entity entity, assets::AssetId script);
    void detachScript(scene::Entity entity, std::size_t index);
    void setScriptProperty(scene::Entity entity, std::size_t index, std::string name, scene::ScriptValue value);
    void resetScriptProperty(scene::Entity entity, std::size_t index, std::string_view name);
    // Moves every selected entity (not counting those whose ancestor is also
    // selected) by a world-space offset. Use inside a transaction for drags.
    void translateSelection(Vec2 worldDelta);
    // Resizes a tilemap, keeping the tiles at the top-left; new tiles are empty.
    void resizeTilemap(scene::Entity entity, int width, int height);
    // Sets one tile (no-op outside the map). Use inside a transaction for strokes.
    bool setTile(scene::Entity entity, int x, int y, int tile);

    // A name not used by any entity in the scene: "Coin 14" -> "Coin 15", "Player" -> "Player 2".
    [[nodiscard]] std::string uniqueName(std::string_view wanted) const;

private:
    struct Snapshot {
        scene::Scene scene;
        std::vector<Guid> selection;
        Guid primary;
        std::uint64_t version = 0;
        std::string label;
        std::size_t bytes = 0;
    };
    // Opens a transaction if none is open; closes it on destruction.
    class AutoEdit {
    public:
        AutoEdit(SceneDocument& doc, std::string_view label) : m_doc(doc) { m_doc.beginEdit(label); }
        ~AutoEdit() { m_doc.endEdit(); }
        AutoEdit(const AutoEdit&) = delete;
        AutoEdit& operator=(const AutoEdit&) = delete;

    private:
        SceneDocument& m_doc;
    };

    [[nodiscard]] Snapshot capture(std::string label) const;
    void restore(Snapshot& snapshot);
    void trimUndo();

    assets::AssetId m_sceneId;
    scene::Scene m_scene;
    std::vector<Guid> m_selection;
    Guid m_primary;

    std::vector<Snapshot> m_undo;
    std::vector<Snapshot> m_redo;
    std::unique_ptr<Snapshot> m_pending; // state when the open transaction began
    int m_editDepth = 0;
    std::uint64_t m_changes = 0;
    std::uint64_t m_changesAtBegin = 0;
    std::uint64_t m_version = 0;
    std::uint64_t m_nextVersion = 1;
    std::uint64_t m_savedVersion = 0;
    std::size_t m_undoBytes = 0;
    std::size_t m_undoLimit = 256u * 1024u * 1024u;
};

// Rough memory use of a scene, for the undo budget.
[[nodiscard]] std::size_t estimateSceneBytes(const scene::Scene& scene);

} // namespace px::editor
