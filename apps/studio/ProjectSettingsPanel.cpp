#include "Panels.hpp"
#include "StudioApp.hpp"

#include "px/core/Keys.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <format>
#include <sstream>

namespace px::studio {

namespace {

std::string keysToText(const std::vector<Key>& keys) {
    std::string out;
    for (const Key k : keys) out += (out.empty() ? "" : ", ") + std::string(keyName(k));
    return out;
}

// "Left, A" -> keys; returns the first unknown name in `bad`.
std::vector<Key> textToKeys(const std::string& text, std::string& bad) {
    std::vector<Key> keys;
    std::stringstream in(text);
    std::string token;
    while (std::getline(in, token, ',')) {
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        if (token.empty()) continue;
        const Key k = keyFromName(token);
        if (k == Key::Unknown) {
            if (bad.empty()) bad = token;
            continue;
        }
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
    }
    return keys;
}

} // namespace

void ProjectSettingsPanel::draw(StudioApp& app, bool& open) {
    auto* project = app.project();
    if (project == nullptr) {
        open = false;
        return;
    }
    if (!m_loaded) {
        m_edit = project->manifest();
        m_keyTexts.clear();
        for (const auto& a : m_edit.input.actions) m_keyTexts.push_back(keysToText(a.keys));
        m_error.clear();
        m_loaded = true;
    }
    ImGui::SetNextWindowSize({ImGui::GetFontSize() * 36.0f, ImGui::GetFontSize() * 34.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Project Settings", &open, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        if (!open) m_loaded = false;
        return;
    }
    const float em = ImGui::GetFontSize();
    ImGui::SeparatorText("Project");
    ImGui::SetNextItemWidth(em * 18.0f);
    ImGui::InputText("Name", &m_edit.name);
    const auto* start = m_edit.assets.find(m_edit.startScene);
    ImGui::SetNextItemWidth(em * 18.0f);
    if (ImGui::BeginCombo("Start scene", start != nullptr ? start->name.c_str() : "(none)")) {
        for (const auto& entry : m_edit.assets.entries()) {
            if (entry.type != assets::AssetType::Scene) continue;
            if (ImGui::Selectable(entry.name.c_str(), entry.id == m_edit.startScene)) m_edit.startScene = entry.id;
        }
        ImGui::EndCombo();
    }
    ui::helpMarker("The scene px-player opens first.");

    ImGui::SeparatorText("Display");
    int res[2] = {m_edit.settings.display.virtualResolution.x, m_edit.settings.display.virtualResolution.y};
    ImGui::SetNextItemWidth(em * 12.0f);
    if (ImGui::InputInt2("Resolution", res)) {
        m_edit.settings.display.virtualResolution = {std::clamp(res[0], 16, 7680), std::clamp(res[1], 16, 4320)};
    }
    ui::helpMarker("The game's own screen size in pixels. It is scaled up to fill the window.");
    int scaling = m_edit.settings.display.scaling == project::ScalingMode::Integer ? 0 : 1;
    ImGui::SetNextItemWidth(em * 12.0f);
    if (ImGui::Combo("Scaling", &scaling, "Whole numbers (crisp pixels)\0Fit window (letterbox)\0")) {
        m_edit.settings.display.scaling = scaling == 0 ? project::ScalingMode::Integer : project::ScalingMode::Letterbox;
    }
    ImGui::SetNextItemWidth(em * 12.0f);
    ImGui::SliderInt("Window scale", &m_edit.settings.display.windowScale, 1, 16);
    ui::helpMarker("Initial desktop window size = resolution x window scale.");

    ImGui::SeparatorText("Simulation");
    ImGui::SetNextItemWidth(em * 12.0f);
    ImGui::SliderInt("Ticks per second", &m_edit.settings.tickRate, 1, 240);
    ui::helpMarker("Fixed simulation steps per second. 60 is a good default.");
    ImGui::TextDisabled("Scripting API version %d", m_edit.settings.scripting.apiVersion);

    ImGui::SeparatorText("Input actions");
    ImGui::TextDisabled("Scripts use action names (Input.isDown(\"jump\")); players can rebind keys later.");
    int removeIndex = -1;
    if (ImGui::BeginTable("actions", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, em * 1.6f);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < m_edit.input.actions.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##name", &m_edit.input.actions[i].name, ImGuiInputTextFlags_CharsNoBlank);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##keys", "e.g. Left, A", &m_keyTexts[i]);
            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton("x")) removeIndex = static_cast<int>(i);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeIndex >= 0) {
        m_edit.input.actions.erase(m_edit.input.actions.begin() + removeIndex);
        m_keyTexts.erase(m_keyTexts.begin() + removeIndex);
    }
    if (m_edit.input.actions.size() < project::kMaxInputActions && ImGui::SmallButton("+ action")) {
        m_edit.input.actions.push_back({std::format("action_{}", m_edit.input.actions.size() + 1), {}});
        m_keyTexts.emplace_back();
    }
    ImGui::TextDisabled("Keys: A-Z, 0-9, Space, Enter, Escape, Left, Right, Up, Down, LeftShift, F1-F12, ...");

    ImGui::Separator();
    if (!m_error.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ui::kErrorColor, "%s", m_error.c_str());
        ImGui::PopTextWrapPos();
    }
    if (ImGui::Button("Apply and Save")) {
        m_error.clear();
        for (std::size_t i = 0; i < m_edit.input.actions.size(); ++i) {
            std::string bad;
            m_edit.input.actions[i].keys = textToKeys(m_keyTexts[i], bad);
            if (!bad.empty()) m_error = std::format("Unknown key '{}' for action '{}'", bad, m_edit.input.actions[i].name);
        }
        if (m_error.empty()) {
            project::ProjectManifest previous = project->manifest();
            project->manifestForEdit() = m_edit;
            project->markManifestDirty();
            if (auto saved = project->saveManifest(); !saved) {
                m_error = saved.error().message;
                project->manifestForEdit() = std::move(previous);
            } else {
                app.notify("Project settings saved");
                m_loaded = false;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard Changes")) m_loaded = false;
    ImGui::SameLine();
    ImGui::TextDisabled("Settings apply to px-player and the next Play.");
    ImGui::End();
    if (!open) m_loaded = false;
}

} // namespace px::studio
