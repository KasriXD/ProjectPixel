#include "Panels.hpp"
#include "StudioApp.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <format>

// The tileset editor: pick tiles of a tileset image and say what they are
// (solid, not blocking, one-way floor, slope) and give them tags for
// scripts. Changes are saved as they are made; tilemaps painted with the
// image use them at once (the next time the game starts).
namespace px::studio {

namespace {

constexpr const char* kWindowId = "###tileset-editor"; // TilesetEditorPanel::windowId()

ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

struct CollisionChoice {
    project::TileCollision value;
    const char* label;
    const char* tip;
};

constexpr CollisionChoice kChoices[] = {
    {project::TileCollision::Solid, "Solid", "Blocks from every side (walls, ground)."},
    {project::TileCollision::None, "None", "Does not block: decoration, floors in a top-down game."},
    {project::TileCollision::OneWay, "One-way",
     "A floor you can jump up through and walk through sideways (platforms, bridges)."},
    {project::TileCollision::SlopeUpRight, "Slope /", "A 45-degree floor rising to the right."},
    {project::TileCollision::SlopeUpLeft, "Slope \\", "A 45-degree floor rising to the left."},
};

} // namespace

void TilesetEditorPanel::open(StudioApp& app, assets::AssetId tileset) {
    auto* project = app.project();
    if (project == nullptr) return;
    auto loaded = project->loadTileset(tileset);
    if (!loaded) {
        app.notify(loaded.error().message, true);
        return;
    }
    m_id = tileset;
    m_tileset = std::move(loaded.value());
    m_selected.clear();
    m_open = true;
    m_focus = true;
    m_pendingSave = false;
}

void TilesetEditorPanel::close() {
    m_open = false;
    m_id = {};
}

void TilesetEditorPanel::draw(StudioApp& app) {
    auto* project = app.project();
    if (!m_open || project == nullptr) return;
    const auto* entry = project->asset(m_id);
    if (entry == nullptr) {
        close();
        return;
    }
    if (m_focus) {
        ImGui::SetNextWindowFocus();
        m_focus = false;
    }
    if (ImGuiWindow* scene = ImGui::FindWindowByName(kSceneWindow); scene != nullptr && scene->DockId != 0) {
        ImGui::SetNextWindowDockID(scene->DockId, ImGuiCond_FirstUseEver);
    }
    bool open = true;
    const std::string title = std::format("{} (tiles){}", entry->name, kWindowId);
    if (ImGui::Begin(title.c_str(), &open)) drawContents(app);
    ImGui::End();
    if (m_pendingSave && !ImGui::IsAnyItemActive()) {
        m_pendingSave = false;
        if (auto saved = project->saveTileset(m_id, m_tileset); !saved) {
            app.notify(saved.error().message, true);
        } else {
            app.tilesetsChanged();
        }
    }
    if (!open) close();
}

void TilesetEditorPanel::drawContents(StudioApp& app) {
    auto* project = app.project();
    const float em = ImGui::GetFontSize();
    const auto* texture = project->asset(m_tileset.texture);
    if (texture == nullptr) {
        ImGui::TextColored(ui::kErrorColor, "The tileset's image is missing from the project.");
        return;
    }
    const render::TextureHandle handle = app.textures()->get(m_tileset.texture);
    const Vec2i size = app.renderer().textureSize(handle);
    if (size.x <= 0 || size.y <= 0) {
        ImGui::TextColored(ui::kErrorColor, "The image could not be loaded.");
        return;
    }
    const int columns = std::max(1, static_cast<int>(static_cast<float>(size.x) / m_tileset.tileSize.x));
    const int rows = std::max(1, static_cast<int>(static_cast<float>(size.y) / m_tileset.tileSize.y));
    ImGui::Text("%s: %d x %d tiles of %g x %g", texture->name.c_str(), columns, rows, m_tileset.tileSize.x,
                m_tileset.tileSize.y);
    ImGui::SameLine();
    ImGui::TextDisabled("(every tilemap painted with this image uses these settings)");

    const float sideWidth = std::max(em * 14.0f, ImGui::GetContentRegionAvail().x * 0.3f);
    ImGui::BeginChild("tiles", {ImGui::GetContentRegionAvail().x - sideWidth - em * 0.5f, 0}, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextDisabled("Click a tile to select it; Ctrl+click adds more, Shift+click a range.");
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float zoom = std::clamp(std::floor(avail.x / static_cast<float>(size.x)), 1.0f, 8.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 shown{static_cast<float>(size.x) * zoom, static_cast<float>(size.y) * zoom};
    ImGui::InvisibleButton("##sheet", shown);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, {origin.x + shown.x, origin.y + shown.y}, rgba(70, 72, 80));
    dl->AddImage(ImTextureRef(ui::textureId(app, handle)), origin, {origin.x + shown.x, origin.y + shown.y});
    const float cw = m_tileset.tileSize.x * zoom;
    const float ch = m_tileset.tileSize.y * zoom;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c) {
            const int index = r * columns + c;
            const ImVec2 a{origin.x + static_cast<float>(c) * cw, origin.y + static_cast<float>(r) * ch};
            const ImVec2 b{a.x + cw, a.y + ch};
            dl->AddRect(a, b, rgba(255, 255, 255, 40));
            ui::drawTileMarker(dl, a, b, m_tileset.info(index));
            if (std::find(m_selected.begin(), m_selected.end(), index) != m_selected.end()) {
                dl->AddRect({a.x + 1, a.y + 1}, {b.x - 1, b.y - 1}, rgba(110, 190, 255), 0.0f, 3.0f);
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        const ImVec2 m = ImGui::GetMousePos();
        const int c = std::clamp(static_cast<int>((m.x - origin.x) / cw), 0, columns - 1);
        const int r = std::clamp(static_cast<int>((m.y - origin.y) / ch), 0, rows - 1);
        const int index = r * columns + c;
        const auto& info = m_tileset.info(index);
        std::string tip = std::format("tile {}: {}", index, project::tileCollisionName(info.collision));
        for (const auto& tag : info.tags) tip += " #" + tag;
        ui::tooltip(tip);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.KeyShift && !m_selected.empty()) {
                const int from = std::min(m_selected.back(), index);
                const int to = std::max(m_selected.back(), index);
                for (int i = from; i <= to; ++i) {
                    if (std::find(m_selected.begin(), m_selected.end(), i) == m_selected.end()) m_selected.push_back(i);
                }
            } else if (io.KeyCtrl) {
                const auto it = std::find(m_selected.begin(), m_selected.end(), index);
                if (it == m_selected.end()) {
                    m_selected.push_back(index);
                } else {
                    m_selected.erase(it);
                }
            } else {
                m_selected = {index};
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("side", {0, 0}, ImGuiChildFlags_None);
    if (m_selected.empty()) {
        ImGui::TextWrapped("Select tiles to say what they are.");
        ImGui::Spacing();
        ImGui::TextDisabled("Markers:");
        ImGui::BulletText("bar on top: one-way floor");
        ImGui::BulletText("diagonal line: slope");
        ImGui::BulletText("cross: does not block");
        ImGui::BulletText("green dot: has tags");
        ImGui::TextDisabled("Unmarked tiles are solid.");
    } else {
        if (m_selected.size() == 1) {
            ImGui::Text("Tile %d", m_selected.front());
        } else {
            ImGui::Text("%zu tiles", m_selected.size());
        }
        // Preview of the first selected tile.
        const int first = m_selected.front();
        const Rect source{static_cast<float>(first % columns) * m_tileset.tileSize.x,
                          static_cast<float>(first / columns) * m_tileset.tileSize.y, m_tileset.tileSize.x,
                          m_tileset.tileSize.y};
        ui::textureImage(app, m_tileset.texture, {em * 4.0f, em * 4.0f}, source, nullptr);

        ImGui::SeparatorText("Collision");
        const project::TileCollision current = m_tileset.info(first).collision;
        for (const CollisionChoice& choice : kChoices) {
            if (ImGui::RadioButton(choice.label, current == choice.value)) {
                for (const int t : m_selected) {
                    project::TileInfo info = m_tileset.info(t);
                    info.collision = choice.value;
                    m_tileset.set(t, std::move(info));
                }
                m_pendingSave = true;
            }
            ui::tooltip(choice.tip);
        }
        ImGui::TextDisabled("Only in tilemaps with Solid on.");

        ImGui::SeparatorText("Tags");
        // Tags every selected tile has, with a button to remove each.
        std::vector<std::string> common = m_tileset.info(first).tags;
        std::erase_if(common, [&](const std::string& tag) {
            return std::any_of(m_selected.begin(), m_selected.end(), [&](int t) { return !m_tileset.info(t).hasTag(tag); });
        });
        for (const std::string& tag : common) {
            ImGui::PushID(tag.c_str());
            if (ImGui::SmallButton(std::format("{}  x", tag).c_str())) {
                for (const int t : m_selected) {
                    project::TileInfo info = m_tileset.info(t);
                    std::erase(info.tags, tag);
                    m_tileset.set(t, std::move(info));
                }
                m_pendingSave = true;
            }
            ui::tooltip("Remove the tag");
            ImGui::PopID();
            ImGui::SameLine();
        }
        ImGui::NewLine();
        ImGui::SetNextItemWidth(em * 8.0f);
        const bool enter = ImGui::InputTextWithHint("##tag", "water", &m_newTag, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("Add Tag") || enter) && !m_newTag.empty() && m_newTag.size() <= project::kMaxTileTagLength) {
            for (const int t : m_selected) {
                project::TileInfo info = m_tileset.info(t);
                if (!info.hasTag(m_newTag) && info.tags.size() < project::kMaxTileTags) info.tags.push_back(m_newTag);
                m_tileset.set(t, std::move(info));
            }
            m_newTag.clear();
            m_pendingSave = true;
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Scripts test tags with World.tileHasTag(position, \"water\").");
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
}

} // namespace px::studio
