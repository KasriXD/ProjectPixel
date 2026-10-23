#include "Panels.hpp"
#include "StudioApp.hpp"
#include "Utf8.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <format>

namespace px::studio {

namespace {

bool matches(const assets::AssetEntry& entry, int typeFilter, const std::string& filter) {
    if (typeFilter == 1 && entry.type != assets::AssetType::Texture) return false;
    if (typeFilter == 2 && entry.type != assets::AssetType::Script) return false;
    if (typeFilter == 3 && entry.type != assets::AssetType::Scene) return false;
    if (filter.empty()) return true;
    const auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string f = lower(filter);
    return lower(entry.name).find(f) != std::string::npos || lower(entry.path).find(f) != std::string::npos;
}

const char* typeLabel(assets::AssetType type) {
    switch (type) {
        case assets::AssetType::Texture: return "image";
        case assets::AssetType::Script: return "script";
        case assets::AssetType::Scene: return "scene";
    }
    return "?";
}

} // namespace

void AssetsPanel::drawAssetMenu(StudioApp& app, const assets::AssetEntry& entry) {
    auto* project = app.project();
    if (entry.type == assets::AssetType::Scene) {
        if (ImGui::MenuItem("Open Scene", nullptr, false, !app.playing())) app.requestOpenScene(entry.id);
        if (ImGui::MenuItem("Make Start Scene", nullptr, false, entry.id != project->manifest().startScene)) {
            if (auto ok = project->setStartScene(entry.id); !ok) app.notify(ok.error().message, true);
        }
    }
    if (entry.type == assets::AssetType::Script) {
        if (ImGui::MenuItem("Edit Script")) app.openScript(entry.id);
    }
    if (ImGui::MenuItem("Open In System Editor")) app.openPath(project->absolutePath(entry.id));
    if (ImGui::MenuItem("Show In Folder")) app.openPath(project->absolutePath(entry.id).parent_path());
    ImGui::Separator();
    if (ImGui::MenuItem("Rename")) {
        m_renaming = entry.id;
        m_renameText = entry.name;
    }
    if (ImGui::MenuItem("Find Uses")) m_showUses = entry.id;
    if (ImGui::MenuItem("Copy Id")) ImGui::SetClipboardText(entry.id.toString().c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("Delete...", nullptr, false, !app.playing())) m_deleting = entry.id;
}

void AssetsPanel::draw(StudioApp& app) {
    if (!ImGui::Begin(kAssetsWindow)) {
        ImGui::End();
        return;
    }
    auto* project = app.project();
    if (project == nullptr) {
        ImGui::End();
        return;
    }

    // Toolbar.
    if (ImGui::Button("Import...")) app.dialogs().pickImportFiles(app.window());
    ui::tooltip("Copy images (.png .jpg .bmp .tga), Luau scripts or scenes into the project. You can also drop files "
                "onto the Studio window.");
    ImGui::SameLine();
    if (ImGui::Button("New Script...")) m_openNewScript = true;
    ImGui::SameLine();
    if (ImGui::Button("New Scene...")) m_openNewScene = true;
    ImGui::SameLine();
    const char* types[] = {"All", "Images", "Scripts", "Scenes"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    ImGui::Combo("##type", &m_typeFilter, types, 4);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    ImGui::InputTextWithHint("##filter", "Search", &m_filter);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##importPath", "or type a file path to import, then Enter", &m_importPath,
                                 ImGuiInputTextFlags_EnterReturnsTrue) &&
        !m_importPath.empty()) {
        app.importFiles({fromUtf8(m_importPath)});
        m_importPath.clear();
    }

    // Grid of assets.
    ImGui::BeginChild("grid", {0, 0}, ImGuiChildFlags_None);
    const float em = ImGui::GetFontSize();
    const float tile = em * 6.0f;
    const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (tile + em * 0.6f)));
    int column = 0;
    std::optional<assets::AssetId> openScene;
    for (const auto& entry : project->assets().entries()) {
        if (!matches(entry, m_typeFilter, m_filter)) continue;
        if (column > 0) ImGui::SameLine();
        ImGui::PushID(entry.id.toString().c_str());
        ImGui::BeginGroup();
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const bool current = app.document() != nullptr && app.document()->assetId() == entry.id;
        if (entry.type == assets::AssetType::Texture) {
            ui::textureImage(app, entry.id, {tile, tile * 0.75f}, {}, "thumb");
        } else {
            ImGui::InvisibleButton("thumb", {tile, tile * 0.75f});
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 bg = entry.type == assets::AssetType::Script ? IM_COL32(48, 70, 110, 255) : IM_COL32(60, 95, 60, 255);
            dl->AddRectFilled({cursor.x + 6, cursor.y + 4}, {cursor.x + tile - 6, cursor.y + tile * 0.75f - 4}, bg, 6.0f);
            const char* glyph = entry.type == assets::AssetType::Script ? "LUAU" : "SCENE";
            const ImVec2 ts = ImGui::CalcTextSize(glyph);
            dl->AddText({cursor.x + (tile - ts.x) * 0.5f, cursor.y + (tile * 0.75f - ts.y) * 0.5f},
                        IM_COL32(230, 235, 245, 255), glyph);
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (entry.type == assets::AssetType::Scene) openScene = entry.id;
            if (entry.type == assets::AssetType::Script) app.openScript(entry.id);
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(ui::kAssetPayload, &entry.id, sizeof(entry.id));
            ImGui::Text("%s (%s)", entry.name.c_str(), typeLabel(entry.type));
            if (entry.type == assets::AssetType::Texture) ImGui::TextDisabled("Drop in the Scene view to create a sprite");
            if (entry.type == assets::AssetType::Script) ImGui::TextDisabled("Drop on an entity to attach it");
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginPopupContextItem("menu")) {
            drawAssetMenu(app, entry);
            ImGui::EndPopup();
        }
        // Name, with markers.
        std::string label = entry.name;
        if (label.size() > 18) label = label.substr(0, 16) + "..";
        const editor::ScriptInfo* script =
            entry.type == assets::AssetType::Script ? project->scriptInfo(entry.id) : nullptr;
        if (script != nullptr && !script->error.empty()) {
            ImGui::TextColored(ui::kErrorColor, "%s", label.c_str());
        } else if (current) {
            ImGui::TextColored(ui::kAccentColor, "%s", label.c_str());
        } else {
            ImGui::TextUnformatted(label.c_str());
        }
        std::string tip = std::format("{}\n{} - {}", entry.name, typeLabel(entry.type), entry.path);
        if (entry.id == project->manifest().startScene) tip += "\nStart scene";
        if (project->scriptBufferDirty(entry.id)) tip += "\nUnsaved changes";
        if (script != nullptr && !script->error.empty()) tip += "\n" + script->error;
        if (hovered) ui::tooltip(tip);
        ImGui::EndGroup();
        ImGui::PopID();
        column = (column + 1) % columns;
    }
    ImGui::EndChild();
    if (openScene) app.requestOpenScene(*openScene);

    // Rename.
    if (!m_renaming.isNil()) ImGui::OpenPopup("Rename Asset");
    if (ImGui::BeginPopupModal("Rename Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("Changes the name shown in Studio; the file and id stay the same.");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##name", &m_renameText, ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || ImGui::Button("Rename")) {
            if (auto ok = project->renameAsset(m_renaming, m_renameText); !ok) app.notify(ok.error().message, true);
            m_renaming = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_renaming = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Find uses.
    if (!m_showUses.isNil()) ImGui::OpenPopup("Uses");
    if (ImGui::BeginPopupModal("Uses", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::vector<std::pair<assets::AssetId, const scene::Scene*>> open;
        if (app.document() != nullptr) open.emplace_back(app.document()->assetId(), &app.document()->scene());
        const auto uses = project->findUses(m_showUses, open);
        const auto* entry = project->asset(m_showUses);
        ImGui::Text("'%s' is used by %zu entit%s", entry != nullptr ? entry->name.c_str() : "?", uses.size(),
                    uses.size() == 1 ? "y" : "ies");
        for (const auto& use : uses) ImGui::BulletText("%s  (scene %s)", use.entityName.c_str(), use.sceneName.c_str());
        if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_showUses = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Delete.
    if (!m_deleting.isNil()) ImGui::OpenPopup("Delete Asset");
    if (ImGui::BeginPopupModal("Delete Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto* entry = project->asset(m_deleting);
        ImGui::Text("Delete '%s' and its file %s?", entry != nullptr ? entry->name.c_str() : "?",
                    entry != nullptr ? entry->path.c_str() : "");
        ImGui::TextDisabled("This cannot be undone. Assets that scenes still use cannot be deleted.");
        if (ImGui::Button("Delete")) {
            std::vector<std::pair<assets::AssetId, const scene::Scene*>> open;
            if (app.document() != nullptr) open.emplace_back(app.document()->assetId(), &app.document()->scene());
            if (auto ok = project->deleteAsset(m_deleting, open); !ok) {
                app.notify(ok.error().message, true);
            } else {
                app.assetsChanged();
            }
            m_deleting = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_deleting = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // New script / scene.
    if (m_openNewScript) {
        ImGui::OpenPopup("New Script");
        m_openNewScript = false;
    }
    if (ImGui::BeginPopupModal("New Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Name of the behaviour, e.g. \"Enemy Patrol\":");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##name", &m_newScriptName, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("Creates scripts/%s.luau from a template", editor::toFileStem(m_newScriptName).c_str());
        if (enter || ImGui::Button("Create")) {
            auto id = project->createScript(m_newScriptName);
            if (!id) {
                app.notify(id.error().message, true);
            } else {
                app.assetsChanged();
                app.openScript(id.value());
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (m_openNewScene) {
        ImGui::OpenPopup("New Scene");
        m_openNewScene = false;
    }
    if (ImGui::BeginPopupModal("New Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##name", &m_newSceneName, ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || ImGui::Button("Create")) {
            auto id = project->createScene(m_newSceneName);
            if (!id) {
                app.notify(id.error().message, true);
            } else {
                app.requestOpenScene(id.value());
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}

} // namespace px::studio
