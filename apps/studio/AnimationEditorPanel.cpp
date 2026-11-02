#include "Panels.hpp"
#include "StudioApp.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <format>

// The animation clip editor: pick frames from a sprite sheet on a grid,
// reorder them, set the speed, and watch the result. Changes are saved as
// they are made (like prefabs), so Play always uses what is shown here.
namespace px::studio {

namespace {

constexpr const char* kWindowId = "###animation-editor"; // AnimationEditorPanel::windowId()

double nowSeconds() { return static_cast<double>(SDL_GetTicksNS()) * 1.0e-9; }

ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

} // namespace

void AnimationEditorPanel::open(StudioApp& app, assets::AssetId clip) {
    auto* project = app.project();
    if (project == nullptr) return;
    auto loaded = project->loadAnimation(clip);
    if (!loaded) {
        app.notify(loaded.error().message, true);
        return;
    }
    m_id = clip;
    m_clip = std::move(loaded.value());
    m_selected = -1;
    m_open = true;
    m_focus = true;
    m_pendingSave = false;
    m_previewStart = nowSeconds();
}

void AnimationEditorPanel::close() {
    m_open = false;
    m_id = {};
}

void AnimationEditorPanel::changed() {
    m_pendingSave = true;
    m_previewStart = nowSeconds();
}

void AnimationEditorPanel::draw(StudioApp& app) {
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
    const std::string title = std::format("{} (animation){}", entry->name, kWindowId);
    if (ImGui::Begin(title.c_str(), &open)) drawContents(app);
    ImGui::End();
    // Save once a widget is let go (a drag of the speed is one save).
    if (m_pendingSave && !ImGui::IsAnyItemActive()) {
        m_pendingSave = false;
        if (auto saved = project->saveAnimation(m_id, m_clip); !saved) app.notify(saved.error().message, true);
    }
    if (!open) close();
}

void AnimationEditorPanel::drawContents(StudioApp& app) {
    auto* project = app.project();
    const float em = ImGui::GetFontSize();

    // Settings.
    const auto* texture = project->asset(m_clip.texture);
    ImGui::SetNextItemWidth(em * 10.0f);
    if (ImGui::BeginCombo("Image", texture != nullptr ? texture->name.c_str() : "(none)")) {
        for (const auto& a : project->assets().entries()) {
            if (a.type != assets::AssetType::Texture) continue;
            if (ImGui::Selectable(a.name.c_str(), a.id == m_clip.texture) && a.id != m_clip.texture) {
                m_clip.texture = a.id;
                m_clip.frames.clear();
                changed();
            }
        }
        ImGui::EndCombo();
    }
    ui::tooltip("The sprite sheet the frames come from. Choosing another one clears the frames.");
    ImGui::SameLine();
    int cell[2] = {static_cast<int>(m_clip.cellSize.x), static_cast<int>(m_clip.cellSize.y)};
    ImGui::SetNextItemWidth(em * 7.0f);
    if (ImGui::DragInt2("Cell", cell, 0.2f, 1, 1024)) {
        m_clip.cellSize = {static_cast<float>(std::clamp(cell[0], 1, 1024)), static_cast<float>(std::clamp(cell[1], 1, 1024))};
        changed();
    }
    ui::tooltip("Size of one frame on the sheet; the grid below follows it.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 5.0f);
    float fps = m_clip.framesPerSecond;
    if (ImGui::DragFloat("Frames/s", &fps, 0.1f, 0.5f, 60.0f, "%.1f")) {
        m_clip.framesPerSecond = std::clamp(fps, 0.5f, 240.0f);
        changed();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Loop", &m_clip.loop)) changed();
    ui::tooltip("Off: the clip plays once and stays on its last frame (a script's one-shot clip then hands back to "
                "idle, run, ...).");

    if (texture == nullptr) {
        ImGui::TextColored(ui::kWarningColor, "Choose the image that holds the frames.");
        return;
    }
    const render::TextureHandle handle = app.textures()->get(m_clip.texture);
    const Vec2i size = app.renderer().textureSize(handle);
    if (size.x <= 0 || size.y <= 0) {
        ImGui::TextColored(ui::kErrorColor, "The image could not be loaded.");
        return;
    }

    // Preview and frame strip on the right, the sheet on the left.
    const float sideWidth = std::max(em * 12.0f, ImGui::GetContentRegionAvail().x * 0.32f);
    ImGui::BeginChild("sheet", {ImGui::GetContentRegionAvail().x - sideWidth - em * 0.5f, 0},
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextDisabled("Click a cell to add it as the next frame; right-click a cell to take it out.");
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float fit = std::min(avail.x / static_cast<float>(size.x), std::max(avail.y, em * 8.0f) / static_cast<float>(size.y));
    const float zoom = std::clamp(std::floor(fit), 1.0f, 12.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 shown{static_cast<float>(size.x) * zoom, static_cast<float>(size.y) * zoom};
    ImGui::InvisibleButton("##sheet", shown, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Checkerboard behind transparent pixels.
    const float check = 8.0f;
    for (float y = 0; y < shown.y; y += check) {
        for (float x = 0; x < shown.x; x += check) {
            const bool dark = (static_cast<int>(x / check) + static_cast<int>(y / check)) % 2 == 0;
            dl->AddRectFilled({origin.x + x, origin.y + y},
                              {origin.x + std::min(x + check, shown.x), origin.y + std::min(y + check, shown.y)},
                              dark ? rgba(70, 70, 78) : rgba(92, 92, 100));
        }
    }
    dl->AddImage(ImTextureRef(ui::textureId(app, handle)), origin, {origin.x + shown.x, origin.y + shown.y});
    const std::vector<Rect> cells = project::gridCells(size, m_clip.cellSize);
    for (const Rect& c : cells) {
        dl->AddRect({origin.x + c.x * zoom, origin.y + c.y * zoom},
                    {origin.x + (c.x + c.w) * zoom, origin.y + (c.y + c.h) * zoom}, rgba(255, 255, 255, 50));
    }
    // Frames already in the clip, numbered in playing order.
    for (std::size_t i = 0; i < m_clip.frames.size(); ++i) {
        const Rect& f = m_clip.frames[i];
        const ImVec2 a{origin.x + f.x * zoom, origin.y + f.y * zoom};
        const ImVec2 b{origin.x + (f.x + f.w) * zoom, origin.y + (f.y + f.h) * zoom};
        const bool selected = static_cast<int>(i) == m_selected;
        dl->AddRect(a, b, selected ? rgba(255, 210, 90) : rgba(110, 190, 255), 0.0f, selected ? 3.0f : 2.0f);
        dl->AddText({a.x + 3.0f, a.y + 2.0f + static_cast<float>(std::count_if(m_clip.frames.begin(),
                                                                                m_clip.frames.begin() + static_cast<std::ptrdiff_t>(i),
                                                                                [&](const Rect& o) { return o.x == f.x && o.y == f.y; })) * em},
                    rgba(255, 255, 255), std::to_string(i + 1).c_str());
    }
    if (ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const Vec2 texel{(mouse.x - origin.x) / zoom, (mouse.y - origin.y) / zoom};
        for (const Rect& c : cells) {
            if (!c.contains(texel)) continue;
            dl->AddRectFilled({origin.x + c.x * zoom, origin.y + c.y * zoom},
                              {origin.x + (c.x + c.w) * zoom, origin.y + (c.y + c.h) * zoom}, rgba(255, 255, 255, 40));
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_clip.frames.size() < project::kMaxAnimationClipFrames) {
                m_clip.frames.push_back(c);
                m_selected = static_cast<int>(m_clip.frames.size()) - 1;
                changed();
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                for (std::size_t i = m_clip.frames.size(); i-- > 0;) {
                    const Rect& f = m_clip.frames[i];
                    if (f.x == c.x && f.y == c.y && f.w == c.w && f.h == c.h) {
                        m_clip.frames.erase(m_clip.frames.begin() + static_cast<std::ptrdiff_t>(i));
                        m_selected = -1;
                        changed();
                        break;
                    }
                }
            }
            ui::tooltip(std::format("cell [{}, {}]", c.x, c.y));
            break;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("side", {0, 0}, ImGuiChildFlags_None);
    // Preview.
    ImGui::TextUnformatted("Preview");
    const float box = std::min(ImGui::GetContentRegionAvail().x, em * 10.0f);
    if (m_clip.frames.empty()) {
        ImGui::Dummy({box, box});
        ImGui::TextDisabled("No frames yet.");
    } else {
        float t = static_cast<float>(nowSeconds() - m_previewStart);
        if (!m_clip.loop) t = std::fmod(t, m_clip.duration() + 0.6f); // replay a one-shot clip after a pause
        const std::size_t frame = m_clip.frameAt(t);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(at, {at.x + box, at.y + box}, rgba(60, 64, 72), 4.0f);
        ui::textureImage(app, m_clip.texture, {box, box}, m_clip.frames[frame], nullptr);
        ImGui::TextDisabled("frame %zu of %zu, %.2f s%s", frame + 1, m_clip.frames.size(), m_clip.duration(),
                            m_clip.loop ? ", looping" : "");
    }
    ImGui::Separator();

    // Frames, in order.
    ImGui::TextUnformatted("Frames");
    ImGui::BeginDisabled(m_selected < 0 || m_selected >= static_cast<int>(m_clip.frames.size()));
    const auto moveSelected = [&](int by) {
        const int to = m_selected + by;
        if (to < 0 || to >= static_cast<int>(m_clip.frames.size())) return;
        std::swap(m_clip.frames[static_cast<std::size_t>(m_selected)], m_clip.frames[static_cast<std::size_t>(to)]);
        m_selected = to;
        changed();
    };
    if (ImGui::SmallButton("< Earlier")) moveSelected(-1);
    ImGui::SameLine();
    if (ImGui::SmallButton("Later >")) moveSelected(1);
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) {
        m_clip.frames.erase(m_clip.frames.begin() + m_selected);
        m_selected = std::min(m_selected, static_cast<int>(m_clip.frames.size()) - 1);
        changed();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_clip.frames.empty());
    if (ImGui::SmallButton("Clear")) {
        m_clip.frames.clear();
        m_selected = -1;
        changed();
    }
    ImGui::EndDisabled();
    const float thumb = em * 3.0f;
    const int perRow = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (thumb + em * 0.4f)));
    for (std::size_t i = 0; i < m_clip.frames.size(); ++i) {
        if (i % static_cast<std::size_t>(perRow) != 0) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const bool selected = static_cast<int>(i) == m_selected;
        ImGui::GetWindowDrawList()->AddRectFilled(at, {at.x + thumb, at.y + thumb},
                                                  selected ? rgba(95, 80, 40) : rgba(55, 58, 66), 3.0f);
        if (ui::textureImage(app, m_clip.texture, {thumb, thumb}, m_clip.frames[i], "frame")) {
            m_selected = selected ? -1 : static_cast<int>(i);
        }
        ImGui::GetWindowDrawList()->AddText({at.x + 2.0f, at.y + 1.0f}, rgba(255, 255, 255), std::to_string(i + 1).c_str());
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", app.project()->asset(m_id)->path.c_str());
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Use it with an Animator (Properties > Add Component > Animator) or from a script: "
                        "entity:playAnimation(\"%s\").",
                        app.project()->asset(m_id)->name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

} // namespace px::studio
