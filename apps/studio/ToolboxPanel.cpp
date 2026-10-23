#include "Panels.hpp"
#include "StudioApp.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <vector>

namespace px::studio {

namespace {

bool containsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != haystack.end();
}

// Categories in the order creators look for them.
constexpr std::string_view kCategoryOrder[] = {"Characters", "Ground",  "Items",      "Hazards",
                                              "Enemies",    "Platforms", "Goals", "Decoration"};

int categoryRank(std::string_view category) {
    const auto it = std::find(std::begin(kCategoryOrder), std::end(kCategoryOrder), category);
    return static_cast<int>(it - std::begin(kCategoryOrder));
}

struct Tile {
    std::string name;
    std::string description;
    assets::AssetId icon;
    Rect iconSource{};
    ui::LibraryPayload payload;
};

// One clickable, draggable tile: icon above the name. Returns true when clicked.
bool drawTile(StudioApp& app, const Tile& tile, float size, bool enabled) {
    ImGui::PushID(tile.name.c_str());
    ImGui::BeginGroup();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    // A click inserts on release, so pressing a tile to drag it does not insert it as well.
    bool released = false;
    if (tile.icon.isNil()) {
        released = ImGui::InvisibleButton("icon", {size, size * 0.8f});
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled({cursor.x + 6, cursor.y + 4}, {cursor.x + size - 6, cursor.y + size * 0.8f - 4},
                          IM_COL32(48, 70, 110, 255), 6.0f);
        const char* glyph = "LUAU";
        const ImVec2 ts = ImGui::CalcTextSize(glyph);
        dl->AddText({cursor.x + (size - ts.x) * 0.5f, cursor.y + (size * 0.8f - ts.y) * 0.5f},
                    IM_COL32(230, 235, 245, 255), glyph);
    } else {
        ImGui::GetWindowDrawList()->AddRectFilled(cursor, {cursor.x + size, cursor.y + size * 0.8f},
                                                  IM_COL32(40, 46, 56, 255), 4.0f);
        released = ui::textureImage(app, app.libraryTextures(), tile.icon, {size, size * 0.8f}, tile.iconSource, "icon");
    }
    const bool hovered = ImGui::IsItemHovered();
    // A drag that comes back to the tile and is released there is not a click either.
    const ImGuiIO& io = ImGui::GetIO();
    const bool dragged = io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] >= io.MouseDragThreshold * io.MouseDragThreshold;
    bool clicked = enabled && released && !dragged;
    if (hovered) {
        ImGui::GetWindowDrawList()->AddRect(cursor, {cursor.x + size, cursor.y + size * 0.8f},
                                            IM_COL32(110, 170, 255, 255), 4.0f, 2.0f);
    }
    if (enabled && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(ui::kLibraryPayload, &tile.payload, sizeof(tile.payload));
        ImGui::TextUnformatted(tile.name.c_str());
        ImGui::TextDisabled(tile.payload.kind == ui::LibraryPayload::Kind::Behaviour
                                ? "Drop on an entity in the Scene view or Explorer"
                                : "Drop in the Scene view");
        ImGui::EndDragDropSource();
    }
    // The name, on up to two lines under the icon.
    const ImVec2 textPos = ImGui::GetCursorScreenPos();
    const float lineHeight = ImGui::GetTextLineHeight();
    ImGui::Dummy({size, lineHeight * 2.0f});
    // A word too long for the tile ("Checkpoint") is drawn a little smaller rather than split.
    float widestWord = 0.0f;
    for (std::size_t start = 0; start < tile.name.size();) {
        std::size_t end = tile.name.find_first_of(" -", start);
        end = end == std::string::npos ? tile.name.size() : end;
        // A hyphen stays with its word; a space does not count.
        const std::size_t wordEnd = end < tile.name.size() && tile.name[end] == '-' ? end + 1 : end;
        widestWord = std::max(widestWord, ImGui::CalcTextSize(tile.name.data() + start, tile.name.data() + wordEnd).x);
        start = end + 1;
    }
    const float fontSize = ImGui::GetFontSize() * std::clamp(size / std::max(widestWord, 1.0f), 0.75f, 1.0f);
    ImGui::PushClipRect(textPos, {textPos.x + size, textPos.y + lineHeight * 2.0f}, true);
    ImGui::GetWindowDrawList()->AddText(nullptr, fontSize, textPos, ImGui::GetColorU32(ImGuiCol_Text),
                                        tile.name.c_str(), nullptr, size);
    ImGui::PopClipRect();
    ImGui::EndGroup();
    if (hovered) {
        const char* how = tile.payload.kind == ui::LibraryPayload::Kind::Behaviour
                              ? "Click to add it to the selected entity, or drag it onto one."
                              : "Click to add it in the middle of the view, or drag it into the Scene view.";
        ui::tooltip(std::format("{}\n{}\n\n{}", tile.name, tile.description, enabled ? how : "Stop playing to add things."));
    }
    ImGui::PopID();
    return clicked;
}

void drawTiles(StudioApp& app, const std::vector<Tile>& tiles, bool enabled) {
    const float em = ImGui::GetFontSize();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((avail + spacing) / (em * 4.6f + spacing)));
    // The tiles share out the row, so names get as much room as the panel allows.
    const float size = std::clamp((avail - spacing * static_cast<float>(columns - 1)) / static_cast<float>(columns),
                                  em * 3.0f, em * 6.0f);
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        if (i % static_cast<std::size_t>(columns) != 0) ImGui::SameLine();
        if (!drawTile(app, tiles[i], size, enabled)) continue;
        const auto& p = tiles[i].payload;
        switch (p.kind) {
            case ui::LibraryPayload::Kind::Object: app.insertLibraryObject(static_cast<std::size_t>(p.object)); break;
            case ui::LibraryPayload::Kind::Image: app.insertLibraryImage(p.asset); break;
            case ui::LibraryPayload::Kind::Behaviour: app.attachLibraryScript(p.asset, app.primarySelection()); break;
        }
    }
}

} // namespace

void ToolboxPanel::draw(StudioApp& app) {
    if (!ImGui::Begin(kToolboxWindow)) {
        ImGui::End();
        return;
    }
    const editor::Library* library = app.library();
    if (app.project() == nullptr) {
        ImGui::End();
        return;
    }
    if (library == nullptr) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ui::kWarningColor, "The starter library is not available.");
        ImGui::TextDisabled("%s", app.libraryError().c_str());
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }
    const project::GameType type = app.gameType();
    const bool enabled = !app.playing() && app.document() != nullptr;

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search the toolbox", &m_filter);
    ImGui::Checkbox("All game types", &m_allTypes);
    ui::tooltip(std::format("Off: only what suits a {} game.",
                            type == project::GameType::TopDown ? "top-down" : "platformer"));
    ImGui::Separator();

    ImGui::BeginChild("items", {0, 0}, ImGuiChildFlags_None);
    const auto matches = [&](const std::string& name, const std::string& description, bool fits) {
        return (m_allTypes || fits) && (containsNoCase(name, m_filter) || containsNoCase(description, m_filter));
    };

    // Ready-made objects, by category.
    const auto& manifest = library->manifest();
    std::vector<std::string> categories;
    for (const auto& o : manifest.objects) {
        if (std::find(categories.begin(), categories.end(), o.category) == categories.end()) {
            categories.push_back(o.category);
        }
    }
    std::stable_sort(categories.begin(), categories.end(),
                     [](const std::string& a, const std::string& b) { return categoryRank(a) < categoryRank(b); });
    bool anything = false;
    for (const std::string& category : categories) {
        std::vector<Tile> tiles;
        for (std::size_t i = 0; i < manifest.objects.size(); ++i) {
            const auto& o = manifest.objects[i];
            if (o.category != category || !matches(o.name, o.description, o.fits(type))) continue;
            Tile tile{o.name, o.description, o.iconTexture, o.iconSource, {}};
            tile.payload.kind = ui::LibraryPayload::Kind::Object;
            tile.payload.object = static_cast<int>(i);
            tiles.push_back(std::move(tile));
        }
        if (tiles.empty()) continue;
        anything = true;
        if (ImGui::CollapsingHeader(category.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) drawTiles(app, tiles, enabled);
    }

    // Plain images and behaviours.
    std::vector<Tile> images;
    std::vector<Tile> behaviours;
    for (const auto& a : manifest.assets) {
        if (!matches(a.entry.name, a.description, a.fits(type))) continue;
        Tile tile{a.entry.name, a.description, {}, {}, {}};
        tile.payload.asset = a.entry.id;
        if (a.entry.type == assets::AssetType::Texture) {
            tile.icon = a.entry.id;
            tile.payload.kind = ui::LibraryPayload::Kind::Image;
            if (tile.description.empty()) {
                tile.description = a.isTileset() ? "A tileset: becomes a tilemap to paint with."
                                                 : "An image: becomes a sprite of its size.";
            }
            images.push_back(std::move(tile));
        } else {
            tile.payload.kind = ui::LibraryPayload::Kind::Behaviour;
            behaviours.push_back(std::move(tile));
        }
    }
    if (!images.empty()) {
        anything = true;
        if (ImGui::CollapsingHeader("Images and Tilesets")) drawTiles(app, images, enabled);
    }
    if (!behaviours.empty()) {
        anything = true;
        if (ImGui::CollapsingHeader("Behaviours (scripts)")) drawTiles(app, behaviours, enabled);
    }
    if (!anything) ImGui::TextDisabled("Nothing matches \"%s\".", m_filter.c_str());
    ImGui::EndChild();
    ImGui::End();
}

} // namespace px::studio
