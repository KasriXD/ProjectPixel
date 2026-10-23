#include "Panels.hpp"
#include "StudioApp.hpp"

#include "px/editor/Picking.hpp"
#include "px/scene/ComponentInfo.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cstring>
#include <format>

// The inspector is generated from the component descriptors
// (px/scene/ComponentInfo.hpp): every field kind has one widget here, so a
// field added to a descriptor appears in Studio without UI code.
namespace px::studio {

using namespace px::scene;

namespace {

// Two-column layout: label (with the descriptor's description as tooltip), widget.
bool beginFields(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.40f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.60f);
    return true;
}

void fieldLabel(const std::string& label, std::string_view description, bool overridden = false) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    if (overridden) {
        ImGui::TextColored(ui::kAccentColor, "%s", label.c_str());
    } else {
        ImGui::TextUnformatted(label.c_str());
    }
    ui::tooltip(description);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

float rangeLow(const NumberRange& r, float step) {
    if (r.min == -kUnbounded) return 0.0f;
    const auto lo = static_cast<float>(r.min);
    return r.minExclusive ? lo + std::max(step * 0.01f, 0.001f) : lo;
}

float rangeHigh(const NumberRange& r) { return r.max == kUnbounded ? 0.0f : static_cast<float>(r.max); }

ImGuiSliderFlags clampFlags(const NumberRange& r) {
    return r.bounded() ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
}

const char* valueTypeName(const ScriptValue& v) {
    switch (v.index()) {
        case 0: return "number";
        case 1: return "boolean";
        case 2: return "text";
        case 3: return "vector";
    }
    return "?";
}

// Widget for a script property value; returns true when `value` changed.
bool scriptValueWidget(ScriptValue& value) {
    switch (value.index()) {
        case 0: {
            double v = std::get<double>(value);
            if (ImGui::DragScalar("##v", ImGuiDataType_Double, &v, 0.1f, nullptr, nullptr, "%.4g")) {
                value = v;
                return true;
            }
            return false;
        }
        case 1: {
            bool v = std::get<bool>(value);
            if (ImGui::Checkbox("##v", &v)) {
                value = v;
                return true;
            }
            return false;
        }
        case 2: {
            std::string v = std::get<std::string>(value);
            if (ImGui::InputText("##v", &v)) {
                if (v.size() > kMaxScriptStringLength) v.resize(kMaxScriptStringLength);
                value = v;
                return true;
            }
            return false;
        }
        case 3: {
            Vec2 v = std::get<Vec2>(value);
            float xy[2] = {v.x, v.y};
            if (ImGui::DragFloat2("##v", xy, 0.5f)) {
                value = Vec2{xy[0], xy[1]};
                return true;
            }
            return false;
        }
    }
    return false;
}

} // namespace

void InspectorPanel::draw(StudioApp& app) {
    if (!ImGui::Begin(kInspectorWindow)) {
        ImGui::End();
        return;
    }
    Scene* scene = app.activeScene();
    m_edit.setDocument(app.playing() ? nullptr : app.document());
    if (scene == nullptr) {
        ImGui::TextDisabled("Nothing to inspect.");
        ImGui::End();
        return;
    }
    const auto selected = app.selection();
    if (selected.empty()) {
        drawSceneSettings(app, *scene);
    } else if (selected.size() > 1) {
        ImGui::Text("%zu entities selected", selected.size());
        ImGui::TextDisabled("Select a single entity to edit its components.");
        ImGui::BeginDisabled(app.playing());
        if (ImGui::Button("Duplicate")) app.document()->duplicateSelection();
        ImGui::SameLine();
        if (ImGui::Button("Delete")) app.document()->deleteSelection();
        ImGui::EndDisabled();
        ImGui::Separator();
        for (const Entity e : selected) ImGui::BulletText("%s", scene->get<EntityInfo>(e).name.c_str());
    } else {
        drawEntity(app, *scene, selected.front());
    }
    m_edit.endFrame();
    ImGui::End();
}

void InspectorPanel::drawSceneSettings(StudioApp& app, Scene& scene) {
    auto* project = app.project();
    auto* doc = app.document();
    ImGui::TextColored(ui::kAccentColor, "Scene");
    ImGui::SameLine();
    ImGui::TextDisabled("(nothing selected)");
    if (beginFields("scene")) {
        fieldLabel("Name", "The scene's display name.");
        std::string name = scene.name();
        m_edit.commit(ImGui::InputText("##name", &name), "Rename Scene", [&] { scene.setName(name); });

        fieldLabel("Background", "Colour behind everything.");
        ImVec4 color = ui::toImVec4(scene.settings().backgroundColor);
        m_edit.commit(ImGui::ColorEdit4("##bg", &color.x, ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_NoInputs),
                      "Background Colour",
                      [&] { scene.settings().backgroundColor = ui::fromImVec4(color); });

        fieldLabel("Kill Plane", "Players falling below this height (Y, pointing down) respawn at their start.");
        bool enabled = scene.settings().killPlaneY.has_value();
        m_edit.commit(ImGui::Checkbox("##killOn", &enabled), "Kill Plane", [&] {
            scene.settings().killPlaneY = enabled ? std::optional<float>(400.0f) : std::nullopt;
        });
        if (scene.settings().killPlaneY) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            float y = *scene.settings().killPlaneY;
            m_edit.commit(ImGui::DragFloat("##killY", &y, 1.0f), "Kill Plane", [&] { scene.settings().killPlaneY = y; });
        }
        ImGui::EndTable();
    }
    if (project != nullptr && doc != nullptr) {
        ImGui::Spacing();
        if (doc->assetId() == project->manifest().startScene) {
            ImGui::TextColored(ui::kDimColor, "This is the start scene: px-player opens it first.");
        } else if (ImGui::Button("Make This The Start Scene")) {
            if (auto ok = project->setStartScene(doc->assetId()); !ok) app.notify(ok.error().message, true);
        }
        ImGui::Text("%zu entities", scene.entityCount());
        if (!app.playing()) {
            const auto problems = doc->problems(&project->assets());
            if (!problems.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(ui::kErrorColor, "%zu problem%s must be fixed before saving:", problems.size(),
                                   problems.size() == 1 ? "" : "s");
                for (const auto& [entity, problem] : problems) {
                    const auto* info = scene.tryGet<EntityInfo>(entity);
                    const std::string text = std::format("{}: {}{}{} {}", info != nullptr ? info->name : "?",
                                                         problem.component, problem.field.empty() ? "" : ".",
                                                         problem.field, problem.message);
                    if (ImGui::Selectable(text.c_str())) app.select(entity);
                }
            }
        }
    }
    if (app.playing()) {
        const auto& session = app.play()->session();
        const auto& state = session.state();
        ImGui::SeparatorText("Running game");
        ImGui::Text("Time %.2f s   tick %llu", session.elapsedSeconds(), static_cast<unsigned long long>(session.tickCount()));
        ImGui::Text("Score %d   respawns %d", state.score, state.respawns);
        ImGui::Text("Objective: %s", state.objective.empty() ? "-" : state.objective.c_str());
        if (state.levelComplete) ImGui::TextColored(ui::kAccentColor, "Level complete: %s", state.completionMessage.c_str());
        const auto stats = app.play()->session().scripts().stats();
        ImGui::Text("Scripts: %zu instances, %zu with Tick, %zu disabled", stats.instances, stats.tickHandlers,
                    stats.disabledInstances);
        ImGui::Text("Luau memory: %zu KiB", stats.memoryBytes / 1024);
        if (state.scriptErrors > 0) {
            ImGui::TextColored(ui::kErrorColor, "%d script error%s (see Console)", state.scriptErrors,
                               state.scriptErrors == 1 ? "" : "s");
        }
    }
}

void InspectorPanel::drawEntity(StudioApp& app, Scene& scene, Entity entity) {
    auto* info = scene.tryGet<EntityInfo>(entity);
    if (info == nullptr) return;
    const bool playing = app.playing();
    if (playing) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ui::kPlayColor, "Playing: changes here are discarded when you stop.");
        ImGui::PopTextWrapPos();
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    std::string name = info->name;
    m_edit.commit(ImGui::InputText("##entityName", &name), "Rename", [&] { info->name = name; });
    const Entity parent = scene.parent(entity);
    if (!parent.isNull()) {
        ImGui::TextDisabled("Child of");
        ImGui::SameLine();
        if (ImGui::SmallButton(scene.get<EntityInfo>(parent).name.c_str())) app.select(parent);
    }
    ImGui::TextDisabled("id %s", info->id.toString().c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(info->id.toString().c_str());
    ui::tooltip("Copy the entity id (scripts can find it with World.findById)");

    if (!playing && app.project() != nullptr) {
        for (const auto& problem : validateEntity(scene, entity, &app.project()->assets())) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ui::kErrorColor, "%s%s%s: %s", problem.component.c_str(), problem.field.empty() ? "" : ".",
                               problem.field.c_str(), problem.message.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    const ComponentInfo* toRemove = nullptr;
    for (const ComponentInfo& c : componentTypes()) {
        if (!c.has(scene, entity)) continue;
        ImGui::PushID(c.name.data());
        bool keep = true;
        const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap;
        const bool open = ImGui::CollapsingHeader(std::string(c.name).c_str(), playing ? nullptr : &keep, flags);
        ui::tooltip(c.description);
        if (!keep) toRemove = &c;
        if (open) drawComponent(app, scene, entity, c);
        ImGui::PopID();
    }
    if (toRemove != nullptr) {
        if (auto removed = app.document()->removeComponent(entity, *toRemove); !removed) {
            app.notify(removed.error().message, true);
        }
    }
    ImGui::Spacing();
    if (!playing) drawAddComponent(app, entity);
}

void InspectorPanel::drawAddComponent(StudioApp& app, Entity entity) {
    const float width = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("Add Component", {width * 0.5f - 4.0f, 0})) ImGui::OpenPopup("AddComponent");
    ImGui::SameLine();
    if (ImGui::Button("Add Script", {-FLT_MIN, 0})) ImGui::OpenPopup("AddScript");
    ui::tooltip("Attach a behaviour. You can also drop a script from Assets here or onto the entity.");
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ui::kAssetPayload)) {
            assets::AssetId asset;
            std::memcpy(&asset, payload->Data, sizeof(asset));
            const auto* entry = app.project()->asset(asset);
            if (entry != nullptr && entry->type == assets::AssetType::Script) {
                if (auto added = app.document()->attachScript(entity, asset); !added) {
                    app.notify(added.error().message, true);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopup("AddComponent")) {
        const Scene& scene = app.document()->scene();
        std::vector<std::string_view> categories;
        for (const ComponentInfo& c : componentTypes()) {
            if (std::find(categories.begin(), categories.end(), c.category) == categories.end()) {
                categories.push_back(c.category);
            }
        }
        for (const std::string_view category : categories) {
            bool header = false;
            for (const ComponentInfo& c : componentTypes()) {
                if (c.category != category || c.name == "Scripts" || c.has(scene, entity)) continue;
                if (!header) {
                    ImGui::SeparatorText(std::string(category).c_str());
                    header = true;
                }
                if (ImGui::MenuItem(std::string(c.name).c_str())) {
                    if (auto added = app.document()->addComponent(entity, c); !added) {
                        app.notify(added.error().message, true);
                    }
                }
                ui::tooltip(c.description);
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("AddScript")) {
        bool any = false;
        for (const auto& entry : app.project()->assets().entries()) {
            if (entry.type != assets::AssetType::Script) continue;
            any = true;
            if (ImGui::MenuItem(entry.name.c_str())) {
                if (auto added = app.document()->attachScript(entity, entry.id); !added) {
                    app.notify(added.error().message, true);
                }
            }
            ui::tooltip(entry.path);
        }
        if (!any) ImGui::TextDisabled("The project has no scripts yet.");
        ImGui::Separator();
        ImGui::TextUnformatted("New script:");
        static std::string newName = "New Behaviour";
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        ImGui::InputText("##newScript", &newName);
        ImGui::SameLine();
        if (ImGui::Button("Create")) {
            auto created = app.project()->createScript(newName);
            if (!created) {
                app.notify(created.error().message, true);
            } else {
                app.assetsChanged();
                (void)app.document()->attachScript(entity, created.value());
                app.openScript(created.value());
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

void InspectorPanel::drawComponent(StudioApp& app, Scene& scene, Entity entity, const ComponentInfo& info) {
    void* data = info.get(scene, entity);
    if (data == nullptr) return;
    const bool playing = app.playing();
    if (info.name == "Scripts") {
        drawScripts(app, scene, entity);
        return;
    }
    if (beginFields("fields")) {
        for (const FieldInfo& f : info.fields) {
            if (f.kind == FieldKind::TileGrid || f.kind == FieldKind::ScriptList) continue;
            if (f.isRuntime() && !playing) continue;
            ImGui::PushID(f.name.data());
            drawField(app, scene, info, f, data, f.isRuntime());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (info.name == "Tilemap") drawTilemapTools(app, scene, entity);
}

bool InspectorPanel::drawField(StudioApp& app, Scene& scene, const ComponentInfo& info, const FieldInfo& f,
                               void* data, bool readOnly) {
    const std::string label = f.displayLabel();
    fieldLabel(label, f.description);
    const std::string undoLabel = std::format("Edit {} {}", info.name, label);
    ImGui::BeginDisabled(readOnly);
    bool changed = false;
    switch (f.kind) {
        case FieldKind::Float: {
            float v = f.ref<float>(data);
            changed = m_edit.commit(ImGui::DragFloat("##v", &v, f.step, rangeLow(f.range, f.step), rangeHigh(f.range),
                                                     "%.4g", clampFlags(f.range)),
                                    undoLabel, [&] { f.ref<float>(data) = v; });
            break;
        }
        case FieldKind::Int: {
            int v = f.ref<int>(data);
            const int lo = f.range.min == -kUnbounded ? 0 : static_cast<int>(f.range.min) + (f.range.minExclusive ? 1 : 0);
            const int hi = f.range.max == kUnbounded ? 0 : static_cast<int>(f.range.max);
            changed = m_edit.commit(ImGui::DragInt("##v", &v, 0.2f, lo, hi, "%d", clampFlags(f.range)), undoLabel,
                                    [&] { f.ref<int>(data) = v; });
            break;
        }
        case FieldKind::Bool: {
            bool v = f.ref<bool>(data);
            changed = m_edit.commit(ImGui::Checkbox("##v", &v), undoLabel, [&] { f.ref<bool>(data) = v; });
            break;
        }
        case FieldKind::String: {
            std::string v = f.ref<std::string>(data);
            changed = m_edit.commit(ImGui::InputText("##v", &v), undoLabel, [&] { f.ref<std::string>(data) = v; });
            break;
        }
        case FieldKind::Vec2: {
            const Vec2 current = f.ref<Vec2>(data);
            float xy[2] = {current.x, current.y};
            changed = m_edit.commit(ImGui::DragFloat2("##v", xy, f.step, rangeLow(f.range, f.step), rangeHigh(f.range),
                                                      "%.4g", clampFlags(f.range)),
                                    undoLabel, [&] { f.ref<Vec2>(data) = {xy[0], xy[1]}; });
            break;
        }
        case FieldKind::Rect: {
            const Rect current = f.ref<Rect>(data);
            float r[4] = {current.x, current.y, current.w, current.h};
            changed = m_edit.commit(ImGui::DragFloat4("##v", r, 0.5f, 0.0f, 0.0f, "%g"), undoLabel, [&] {
                f.ref<Rect>(data) = {r[0], r[1], std::max(0.0f, r[2]), std::max(0.0f, r[3])};
            });
            break;
        }
        case FieldKind::Color: {
            ImVec4 c = ui::toImVec4(f.ref<Color>(data));
            changed = m_edit.commit(ImGui::ColorEdit4("##v", &c.x,
                                                      ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_NoInputs |
                                                          ImGuiColorEditFlags_AlphaBar),
                                    undoLabel,
                                    [&] { f.ref<Color>(data) = ui::fromImVec4(c); });
            break;
        }
        case FieldKind::Asset: {
            auto* project = app.project();
            assets::AssetId v = f.ref<assets::AssetId>(data);
            const auto* entry = project != nullptr ? project->asset(v) : nullptr;
            const std::string preview = v.isNil() ? std::string("(none)") : (entry != nullptr ? entry->name : "(missing)");
            bool picked = false;
            if (ImGui::BeginCombo("##v", preview.c_str())) {
                if (project != nullptr) {
                    for (const auto& a : project->assets().entries()) {
                        if (a.type != f.assetType) continue;
                        if (ImGui::Selectable(a.name.c_str(), a.id == v)) {
                            v = a.id;
                            picked = true;
                        }
                    }
                }
                ImGui::EndCombo();
            }
            changed = m_edit.commit(picked, undoLabel, [&] { f.ref<assets::AssetId>(data) = v; });
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ui::kAssetPayload)) {
                    assets::AssetId dropped;
                    std::memcpy(&dropped, payload->Data, sizeof(dropped));
                    const auto* d = project != nullptr ? project->asset(dropped) : nullptr;
                    if (d != nullptr && d->type == f.assetType) {
                        changed = m_edit.commit(true, undoLabel, [&] { f.ref<assets::AssetId>(data) = dropped; });
                    } else {
                        app.notify(std::format("{} needs a {} asset", label, assets::assetTypeName(f.assetType)), true);
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (f.assetType == assets::AssetType::Texture && !f.ref<assets::AssetId>(data).isNil()) {
                const float h = ImGui::GetFontSize() * 3.0f;
                Rect source;
                if (info.name == "Sprite") source = static_cast<const Sprite*>(data)->source;
                ui::textureImage(app, f.ref<assets::AssetId>(data), {h * 2.0f, h}, source);
            }
            break;
        }
        case FieldKind::EntityRef: {
            Guid v = f.ref<Guid>(data);
            const Entity target = scene.findById(v);
            const std::string preview =
                v.isNil() ? std::string("(none)") : (target.isNull() ? "(missing)" : scene.get<EntityInfo>(target).name);
            bool picked = false;
            if (ImGui::BeginCombo("##v", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
                if (ImGui::Selectable("(none)", v.isNil())) {
                    v = Guid{};
                    picked = true;
                }
                for (const Entity e : scene.entities()) {
                    const auto* ei = scene.tryGet<EntityInfo>(e);
                    ImGui::PushID(static_cast<int>(e.index));
                    if (ImGui::Selectable(ei->name.c_str(), ei->id == v)) {
                        v = ei->id;
                        picked = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            changed = m_edit.commit(picked, undoLabel, [&] { f.ref<Guid>(data) = v; });
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ui::kEntityPayload)) {
                    Guid dropped;
                    std::memcpy(&dropped, payload->Data, sizeof(Guid));
                    changed = m_edit.commit(true, undoLabel, [&] { f.ref<Guid>(data) = dropped; });
                }
                ImGui::EndDragDropTarget();
            }
            break;
        }
        case FieldKind::StringList: {
            auto& list = f.ref<std::vector<std::string>>(data);
            int removeIndex = -1;
            for (std::size_t i = 0; i < list.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::SmallButton((list[i] + "  x").c_str())) removeIndex = static_cast<int>(i);
                ui::tooltip("Remove");
                ImGui::SameLine();
                ImGui::PopID();
            }
            if (removeIndex >= 0) {
                changed = m_edit.commit(true, undoLabel, [&] {
                    list.erase(list.begin() + removeIndex);
                });
            }
            ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 6.0f));
            if (ImGui::InputTextWithHint("##add", "add, then Enter", &m_newTag, ImGuiInputTextFlags_EnterReturnsTrue)) {
                const std::string tag = m_newTag;
                if (!tag.empty() && tag.size() <= f.maxLength && list.size() < f.maxCount &&
                    std::find(list.begin(), list.end(), tag) == list.end()) {
                    changed = m_edit.commit(true, undoLabel, [&] { list.push_back(tag); });
                }
                m_newTag.clear();
                ImGui::SetKeyboardFocusHere(-1);
            }
            break;
        }
        case FieldKind::RectList: {
            auto& list = f.ref<std::vector<Rect>>(data);
            int removeIndex = -1;
            for (std::size_t i = 0; i < list.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                float r[4] = {list[i].x, list[i].y, list[i].w, list[i].h};
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 1.8f);
                changed |= m_edit.commit(ImGui::DragFloat4("##frame", r, 0.5f, 0.0f, 0.0f, "%g"), undoLabel, [&] {
                    list[i] = {r[0], r[1], std::max(0.0f, r[2]), std::max(0.0f, r[3])};
                });
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) removeIndex = static_cast<int>(i);
                ImGui::PopID();
            }
            if (removeIndex >= 0 && list.size() > 1) {
                changed = m_edit.commit(true, undoLabel, [&] { list.erase(list.begin() + removeIndex); });
            }
            if (list.size() < f.maxCount && ImGui::SmallButton("+ frame")) {
                changed = m_edit.commit(true, undoLabel, [&] {
                    Rect next = list.empty() ? Rect{0, 0, 16, 16} : list.back();
                    if (!list.empty()) next.x += next.w;
                    list.push_back(next);
                });
            }
            ui::tooltip("Adds the next frame to the right of the last one");
            break;
        }
        case FieldKind::TileGrid:
        case FieldKind::ScriptList: break;
    }
    ImGui::EndDisabled();
    return changed;
}

void InspectorPanel::drawTilemapTools(StudioApp& app, Scene& scene, Entity entity) {
    auto* map = scene.tryGet<Tilemap>(entity);
    if (map == nullptr) return;
    const bool playing = app.playing();
    const Guid id = scene.get<EntityInfo>(entity).id;
    if (m_resizeFor != id) {
        m_resizeFor = id;
        m_resizeWidth = map->width;
        m_resizeHeight = map->height;
    }
    ImGui::BeginDisabled(playing);
    ImGui::TextUnformatted("Size");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    int size[2] = {m_resizeWidth, m_resizeHeight};
    if (ImGui::InputInt2("##size", size)) {
        m_resizeWidth = std::clamp(size[0], 1, kMaxTilemapDimension);
        m_resizeHeight = std::clamp(size[1], 1, kMaxTilemapDimension);
    }
    ImGui::SameLine();
    const bool sizeChanged = m_resizeWidth != map->width || m_resizeHeight != map->height;
    ImGui::BeginDisabled(!sizeChanged);
    if (ImGui::Button("Resize")) app.document()->resizeTilemap(entity, m_resizeWidth, m_resizeHeight);
    ImGui::EndDisabled();
    ui::tooltip("Width and height in tiles. Tiles are kept at the top-left; new ones are empty.");
    if (!sizeChanged && (m_resizeWidth != map->width || m_resizeHeight != map->height)) {
        m_resizeWidth = map->width;
        m_resizeHeight = map->height;
    }

    // Palette: click a tile, then paint in the Scene view.
    render::TextureCache* cache = app.textures();
    if (cache != nullptr && !map->tileset.isNil() && map->tileSize.x > 0 && map->tileSize.y > 0) {
        const render::TextureHandle handle = cache->get(map->tileset);
        const Vec2i tex = app.renderer().textureSize(handle);
        const int columns = std::max(1, map->tilesetColumns);
        const int rows = std::max(1, static_cast<int>(static_cast<float>(tex.y) / map->tileSize.y));
        const int expectedColumns = static_cast<int>(static_cast<float>(tex.x) / map->tileSize.x);
        if (expectedColumns >= 1 && expectedColumns != columns) {
            ImGui::TextColored(ui::kWarningColor, "The tileset is %d tiles wide, not %d.", expectedColumns, columns);
            ImGui::SameLine();
            if (ImGui::SmallButton("Fix")) {
                m_edit.commit(true, "Edit Tilemap Columns", [&] { map->tilesetColumns = expectedColumns; });
            }
        }
        ImGui::SeparatorText("Paint tiles");
        if (ImGui::RadioButton("Paint (B)", app.tool == Tool::Paint)) app.tool = Tool::Paint;
        ImGui::SameLine();
        if (ImGui::RadioButton("Erase (E)", app.tool == Tool::Erase)) app.tool = Tool::Erase;
        ImGui::SameLine();
        if (ImGui::RadioButton("Select (Q)", app.tool == Tool::Select)) app.tool = Tool::Select;
        ImGui::TextDisabled("Left drag paints, right drag erases, Alt+click picks a tile.");

        const float cell = std::clamp(ImGui::GetContentRegionAvail().x / static_cast<float>(columns), 16.0f,
                                      ImGui::GetFontSize() * 3.0f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 area{cell * static_cast<float>(columns), cell * static_cast<float>(rows)};
        ImGui::InvisibleButton("palette", area);
        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin, {origin.x + area.x, origin.y + area.y}, IM_COL32(40, 40, 48, 255));
        const ImTextureRef texRef(ui::textureId(app, handle));
        const float tw = static_cast<float>(std::max(1, tex.x));
        const float th = static_cast<float>(std::max(1, tex.y));
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < columns; ++c) {
                const ImVec2 p0{origin.x + static_cast<float>(c) * cell, origin.y + static_cast<float>(r) * cell};
                const ImVec2 p1{p0.x + cell, p0.y + cell};
                const ImVec2 uv0{static_cast<float>(c) * map->tileSize.x / tw, static_cast<float>(r) * map->tileSize.y / th};
                const ImVec2 uv1{(static_cast<float>(c) + 1.0f) * map->tileSize.x / tw,
                                 (static_cast<float>(r) + 1.0f) * map->tileSize.y / th};
                dl->AddImage(texRef, p0, p1, uv0, uv1);
                const int index = r * columns + c;
                if (index == app.paintTile) dl->AddRect(p0, p1, IM_COL32(255, 220, 60, 255), 0.0f, 2.0f);
            }
        }
        if (clicked) {
            const ImVec2 m = ImGui::GetMousePos();
            const int c = std::clamp(static_cast<int>((m.x - origin.x) / cell), 0, columns - 1);
            const int r = std::clamp(static_cast<int>((m.y - origin.y) / cell), 0, rows - 1);
            app.paintTile = r * columns + c;
            app.tool = Tool::Paint;
        }
        ImGui::Text("Tile %d", app.paintTile);
    } else {
        ImGui::TextDisabled("Choose a tileset image to paint tiles.");
    }
    ImGui::EndDisabled();
}

void InspectorPanel::drawScripts(StudioApp& app, Scene& scene, Entity entity) {
    auto* scripts = scene.tryGet<Scripts>(entity);
    auto* project = app.project();
    if (scripts == nullptr || project == nullptr) return;
    const bool playing = app.playing();
    if (scripts->items.empty()) ImGui::TextDisabled("No scripts. Use Add Script below.");
    int removeIndex = -1;
    for (std::size_t i = 0; i < scripts->items.size(); ++i) {
        AttachedScript& attached = scripts->items[i];
        ImGui::PushID(static_cast<int>(i));
        const auto* entry = project->asset(attached.script);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ui::kAccentColor, "%s", entry != nullptr ? entry->name.c_str() : "(missing script)");
        ImGui::SameLine();
        if (entry != nullptr && ImGui::SmallButton("Edit")) app.openScript(attached.script);
        ImGui::SameLine();
        ImGui::BeginDisabled(playing);
        if (ImGui::SmallButton("Remove")) removeIndex = static_cast<int>(i);
        ImGui::EndDisabled();
        const editor::ScriptInfo* script = entry != nullptr ? project->scriptInfo(attached.script) : nullptr;
        if (script == nullptr) {
            ImGui::TextColored(ui::kErrorColor, "This script is not part of the project.");
        } else if (!script->error.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ui::kErrorColor, "%s", script->error.c_str());
            ImGui::PopTextWrapPos();
        } else {
            if (playing) ImGui::TextDisabled("Property values apply when the game starts.");
            ImGui::BeginDisabled(playing);
            if (beginFields("props")) {
                for (const auto& [propName, defaultValue] : script->behaviour.properties) {
                    ImGui::PushID(propName.c_str());
                    const ScriptProperty* set = attached.findProperty(propName);
                    const bool overridden = set != nullptr;
                    ScriptValue value = overridden ? set->value : defaultValue;
                    fieldLabel(propName, overridden ? "Set on this entity (click Reset to use the script's default)"
                                                    : "The script's default value",
                               overridden);
                    if (overridden && value.index() != defaultValue.index()) {
                        ImGui::TextColored(ui::kErrorColor, "is a %s, must be a %s", valueTypeName(value),
                                           valueTypeName(defaultValue));
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Reset")) app.document()->resetScriptProperty(entity, i, propName);
                        ImGui::PopID();
                        continue;
                    }
                    if (overridden) ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 3.2f);
                    const bool edited = scriptValueWidget(value);
                    m_edit.commit(edited, "Set Script Property", [&] {
                        auto& props = attached.properties;
                        const auto it = std::find_if(props.begin(), props.end(),
                                                     [&](const ScriptProperty& p) { return p.name == propName; });
                        if (it != props.end()) {
                            it->value = value;
                        } else {
                            props.push_back({propName, value});
                        }
                    });
                    if (overridden) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Reset")) app.document()->resetScriptProperty(entity, i, propName);
                    }
                    ImGui::PopID();
                }
                // Values for properties the script does not declare (would fail to load).
                std::string undeclaredToRemove;
                for (const ScriptProperty& p : attached.properties) {
                    const bool declared = std::any_of(script->behaviour.properties.begin(),
                                                      script->behaviour.properties.end(),
                                                      [&](const auto& d) { return d.first == p.name; });
                    if (declared) continue;
                    ImGui::PushID(p.name.c_str());
                    fieldLabel(p.name, "Not declared by the script");
                    ImGui::TextColored(ui::kErrorColor, "not in the script");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) undeclaredToRemove = p.name;
                    ImGui::PopID();
                }
                ImGui::EndTable();
                if (!undeclaredToRemove.empty()) app.document()->resetScriptProperty(entity, i, undeclaredToRemove);
            }
            ImGui::EndDisabled();
            if (!script->behaviour.events.empty()) {
                std::string events;
                for (const auto& e : script->behaviour.events) events += (events.empty() ? "" : ", ") + e;
                ImGui::TextDisabled("Handles %s", events.c_str());
            }
        }
        ImGui::Separator();
        ImGui::PopID();
        // removing invalidates `attached`; done after the loop
    }
    if (removeIndex >= 0) app.document()->detachScript(entity, static_cast<std::size_t>(removeIndex));
}

} // namespace px::studio
