#include "Panels.hpp"
#include "StudioApp.hpp"

#include "px/editor/Picking.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <optional>
#include <unordered_map>

namespace px::studio {

using namespace px::scene;

namespace {

bool containsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != haystack.end();
}

bool isGroup(const Scene& scene, Entity e) {
    bool any = false;
    forEachComponent(scene, e, [&](const ComponentInfo&, const void*) { any = true; });
    return !any;
}

constexpr editor::EntityPreset kPresets[] = {
    editor::EntityPreset::Empty,      editor::EntityPreset::Group,   editor::EntityPreset::Sprite,
    editor::EntityPreset::Tilemap,    editor::EntityPreset::SolidBlock, editor::EntityPreset::Trigger,
    editor::EntityPreset::Player,     editor::EntityPreset::Camera,
};

void createUnder(StudioApp& app, editor::EntityPreset preset, Entity parent) { app.createEntity(preset, parent); }

} // namespace

void HierarchyPanel::drawEntityMenu(StudioApp& app, Entity entity) {
    auto* doc = app.document();
    const bool editable = !app.playing();
    const Scene& scene = *app.activeScene();
    if (ImGui::BeginMenu("Create Child", editable)) {
        for (const auto preset : kPresets) {
            if (ImGui::MenuItem(std::string(editor::presetName(preset)).c_str())) createUnder(app, preset, entity);
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Rename", "F2", false, editable)) {
        m_renaming = scene.tryGet<EntityInfo>(entity)->id;
        m_renameText = scene.tryGet<EntityInfo>(entity)->name;
        m_focusRename = true;
    }
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, editable)) doc->duplicateSelection();
    if (ImGui::MenuItem("Copy", "Ctrl+C", false, editable)) {
        const std::string text = doc->copySelection();
        if (!text.empty()) ImGui::SetClipboardText(text.c_str());
    }
    if (ImGui::MenuItem("Paste As Child", nullptr, false, editable)) {
        if (const char* text = ImGui::GetClipboardText()) {
            auto pasted = doc->paste(text, &app.project()->assets(), entity);
            if (!pasted) app.notify(pasted.error().message, true);
        }
    }
    if (ImGui::MenuItem("Move To Top Level", nullptr, false, editable && !scene.parent(entity).isNull())) {
        if (auto moved = doc->setParent(entity, kNullEntity); !moved) app.notify(moved.error().message, true);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Frame In Scene View", "F")) app.frameSelection();
    ImGui::Separator();
    if (ImGui::MenuItem("Delete", "Del", false, editable)) doc->deleteSelection();
}

void HierarchyPanel::draw(StudioApp& app) {
    const bool visible = ImGui::Begin(kHierarchyWindow);
    if (!visible) {
        ImGui::End();
        return;
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) app.entityShortcuts = true;
    Scene* scene = app.activeScene();
    auto* doc = app.document();
    if (scene == nullptr || doc == nullptr) {
        ImGui::TextDisabled("No scene is open.");
        ImGui::End();
        return;
    }
    const bool editable = !app.playing();

    // Toolbar: create, search.
    ImGui::BeginDisabled(!editable);
    if (ImGui::Button("+ Create")) ImGui::OpenPopup("CreateEntity");
    ImGui::EndDisabled();
    ui::tooltip("Create an entity in the middle of the Scene view. To create a child, right-click an entity.");
    if (ImGui::BeginPopup("CreateEntity")) {
        for (const auto preset : kPresets) {
            if (ImGui::MenuItem(std::string(editor::presetName(preset)).c_str())) createUnder(app, preset, kNullEntity);
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", "Search", &m_filter);

    const std::vector<Entity> selected = app.selection();
    const auto isSelected = [&](Entity e) { return std::find(selected.begin(), selected.end(), e) != selected.end(); };
    const auto clickSelect = [&](Entity e) {
        const ImGuiIO& io = ImGui::GetIO();
        app.select(e, io.KeyCtrl ? editor::SelectMode::Toggle
                                 : (io.KeyShift ? editor::SelectMode::Add : editor::SelectMode::Replace));
    };
    std::optional<std::pair<Guid, Entity>> dropRequest; // dragged entity id -> new parent
    bool dropBefore = false;

    ImGui::BeginChild("tree", {0, 0}, ImGuiChildFlags_None);
    if (!m_filter.empty()) {
        for (const Entity e : scene->entities()) {
            const auto* info = scene->tryGet<EntityInfo>(e);
            if (!containsNoCase(info->name, m_filter)) continue;
            ImGui::PushID(static_cast<int>(e.index));
            if (ImGui::Selectable(info->name.empty() ? "(unnamed)" : info->name.c_str(), isSelected(e))) clickSelect(e);
            ImGui::PopID();
        }
    } else {
        // Depth of every entity, from hierarchy order (parents come first).
        const auto& entities = scene->entities();
        std::unordered_map<std::uint32_t, int> depthOf;
        depthOf.reserve(entities.size());
        std::vector<int> depth(entities.size());
        for (std::size_t i = 0; i < entities.size(); ++i) {
            const Entity p = scene->parent(entities[i]);
            depth[i] = p.isNull() ? 0 : depthOf[p.index] + 1;
            depthOf[entities[i].index] = depth[i];
        }
        int openDepth = -1; // depth of the deepest open tree node
        std::size_t i = 0;
        while (i < entities.size()) {
            const Entity e = entities[i];
            while (openDepth >= depth[i]) {
                ImGui::TreePop();
                --openDepth;
            }
            const auto* info = scene->tryGet<EntityInfo>(e);
            const bool hasChildren = i + 1 < entities.size() && depth[i + 1] > depth[i];
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                                       ImGuiTreeNodeFlags_FramePadding;
            if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (isSelected(e)) flags |= ImGuiTreeNodeFlags_Selected;
            ImGui::PushID(static_cast<int>(e.index));
            const bool group = isGroup(*scene, e);
            // Entities that would stop the scene from saving are shown in red.
            const bool broken = editable && !validateEntity(*scene, e, &app.project()->assets()).empty();
            if (broken || group) ImGui::PushStyleColor(ImGuiCol_Text, broken ? ui::kErrorColor : ui::kDimColor);
            const bool open =
                ImGui::TreeNodeEx("##node", flags, "%s", info->name.empty() ? "(unnamed)" : info->name.c_str());
            if (broken || group) ImGui::PopStyleColor();
            if (broken) ui::tooltip("This entity has problems; select it to see them in the Inspector.");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) clickSelect(e);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) app.frameSelection();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !isSelected(e)) app.select(e);
            if (ImGui::BeginPopupContextItem("context")) {
                drawEntityMenu(app, e);
                ImGui::EndPopup();
            }
            if (editable && ImGui::BeginDragDropSource()) {
                const Guid id = info->id;
                ImGui::SetDragDropPayload(ui::kEntityPayload, &id, sizeof(Guid));
                ImGui::Text("%s", info->name.c_str());
                ImGui::TextDisabled("Drop on an entity to make it a child; hold Shift to put it before that entity.");
                ImGui::EndDragDropSource();
            }
            if (editable && ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ui::kEntityPayload)) {
                    Guid dragged;
                    std::memcpy(&dragged, payload->Data, sizeof(Guid));
                    dropRequest = std::make_pair(dragged, e);
                    dropBefore = ImGui::GetIO().KeyShift;
                }
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ui::kAssetPayload)) {
                    assets::AssetId asset;
                    std::memcpy(&asset, payload->Data, sizeof(asset));
                    const auto* entry = app.project()->asset(asset);
                    if (entry != nullptr && entry->type == assets::AssetType::Script) {
                        if (auto attached = app.document()->attachScript(e, asset); !attached) {
                            app.notify(attached.error().message, true);
                        } else {
                            app.select(e);
                            app.notify(std::format("Attached {} to {}", entry->name, info->name));
                        }
                    } else if (entry != nullptr) {
                        app.notify("Only scripts can be dropped onto entities here; drop images into the Scene view", true);
                    }
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::PopID();
            if (hasChildren && open) {
                openDepth = depth[i];
                ++i;
            } else {
                // Skip the descendants of a closed node.
                std::size_t j = i + 1;
                while (j < entities.size() && depth[j] > depth[i]) ++j;
                i = j;
            }
        }
        while (openDepth >= 0) {
            ImGui::TreePop();
            --openDepth;
        }
    }
    // The empty space below the tree: click to deselect, drop to move to the top level.
    const ImVec2 rest = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##empty", {std::max(rest.x, 1.0f), std::max(rest.y, ImGui::GetFontSize() * 2.0f)});
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) app.select(kNullEntity);
    if (editable && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ui::kEntityPayload)) {
            Guid dragged;
            std::memcpy(&dragged, payload->Data, sizeof(Guid));
            dropRequest = std::make_pair(dragged, kNullEntity);
        }
        ImGui::EndDragDropTarget();
    }
    if (editable && ImGui::BeginPopupContextItem("empty-context")) {
        for (const auto preset : kPresets) {
            if (ImGui::MenuItem(std::string(editor::presetName(preset)).c_str())) createUnder(app, preset, kNullEntity);
        }
        if (ImGui::MenuItem("Paste")) {
            if (const char* text = ImGui::GetClipboardText()) {
                auto pasted = doc->paste(text, &app.project()->assets());
                if (!pasted) app.notify(pasted.error().message, true);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    if (dropRequest) {
        const Entity dragged = doc->scene().findById(dropRequest->first);
        const Entity target = dropRequest->second;
        Status moved;
        if (dropBefore && !target.isNull()) {
            moved = doc->setParent(dragged, doc->scene().parent(target), target);
        } else {
            moved = doc->setParent(dragged, target);
        }
        if (!moved) app.notify(moved.error().message, true);
    }

    // F2: rename the selected entity.
    if (editable && app.entityShortcuts && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        const Entity e = doc->primarySelection();
        if (!e.isNull()) {
            m_renaming = doc->scene().tryGet<EntityInfo>(e)->id;
            m_renameText = doc->scene().tryGet<EntityInfo>(e)->name;
            m_focusRename = true;
        }
    }
    if (!m_renaming.isNil() && !ImGui::IsPopupOpen("Rename Entity")) ImGui::OpenPopup("Rename Entity");
    if (ImGui::BeginPopup("Rename Entity")) {
        if (m_focusRename) {
            ImGui::SetKeyboardFocusHere();
            m_focusRename = false;
        }
        const bool done = ImGui::InputText("##rename", &m_renameText, ImGuiInputTextFlags_EnterReturnsTrue);
        if (done) {
            const Entity e = doc->scene().findById(m_renaming);
            if (!e.isNull() && !m_renameText.empty()) doc->rename(e, m_renameText);
            m_renaming = Guid{};
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_renaming = Guid{};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else if (!m_renaming.isNil() && !m_focusRename) {
        m_renaming = Guid{}; // closed by clicking elsewhere
    }
    ImGui::End();
}

} // namespace px::studio
