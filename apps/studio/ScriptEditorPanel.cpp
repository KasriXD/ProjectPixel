#include "Panels.hpp"
#include "StudioApp.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <format>

// A simple text editor for Luau scripts, so creators can stay in Studio.
// It edits the project's unsaved script buffer (used by Play right away);
// Save writes the file. Syntax highlighting and autocomplete are planned.
namespace px::studio {

namespace {

struct CallbackState {
    int* cursorLine = nullptr;
    int* gotoLine = nullptr;
};

int editorCallback(ImGuiInputTextCallbackData* data) {
    auto* state = static_cast<CallbackState*>(data->UserData);
    if (*state->gotoLine > 0) {
        int line = 1;
        int offset = 0;
        while (offset < data->BufTextLen && line < *state->gotoLine) {
            if (data->Buf[offset] == '\n') ++line;
            ++offset;
        }
        data->CursorPos = data->SelectionStart = data->SelectionEnd = offset;
        *state->gotoLine = 0;
    }
    int line = 1;
    for (int i = 0; i < data->CursorPos && i < data->BufTextLen; ++i) {
        if (data->Buf[i] == '\n') ++line;
    }
    *state->cursorLine = line;
    return 0;
}

std::string windowId(const assets::AssetId& script) { return "###script-" + script.toString(); }

// "scripts/x.luau:12: message" -> 12
int errorLine(const std::string& error) {
    const std::size_t at = error.find(".luau:");
    if (at == std::string::npos) return 0;
    int line = 0;
    for (std::size_t p = at + 6; p < error.size() && error[p] >= '0' && error[p] <= '9'; ++p) line = line * 10 + (error[p] - '0');
    return line;
}

} // namespace

std::vector<std::string> ScriptEditorPanel::windowIds() const {
    std::vector<std::string> ids;
    for (const OpenScript& script : m_open) ids.push_back(windowId(script.id));
    return ids;
}

void ScriptEditorPanel::open(StudioApp& app, assets::AssetId script, int line) {
    auto* project = app.project();
    if (project == nullptr || project->asset(script) == nullptr) return;
    auto it = std::find_if(m_open.begin(), m_open.end(), [&](const OpenScript& s) { return s.id == script; });
    if (it == m_open.end()) {
        auto text = project->scriptText(script);
        if (!text) {
            app.notify(text.error().message, true);
            return;
        }
        m_open.push_back({script, std::move(text.value())});
        it = std::prev(m_open.end());
    }
    it->focus = true;
    if (line > 0) it->gotoLine = line;
}

void ScriptEditorPanel::draw(StudioApp& app) {
    auto* project = app.project();
    if (project == nullptr) {
        m_open.clear();
        return;
    }
    static int frame = 0;
    ++frame;
    std::optional<std::size_t> closeIndex;
    for (std::size_t i = 0; i < m_open.size(); ++i) {
        OpenScript& script = m_open[i];
        const auto* entry = project->asset(script.id);
        if (entry == nullptr) {
            closeIndex = i;
            continue;
        }
        const bool dirty = project->scriptBufferDirty(script.id);
        // Pick up edits made by other programs while there are no unsaved changes here.
        if (!dirty && frame % 60 == 0) {
            if (auto disk = project->scriptText(script.id); disk && disk.value() != script.text) {
                script.text = disk.value();
            }
        }
        const std::string title = std::format("{}{}{}", entry->name, dirty ? " *" : "", windowId(script.id));
        if (script.focus) {
            ImGui::SetNextWindowFocus();
            script.focus = false;
        }
        if (ImGuiWindow* scene = ImGui::FindWindowByName(kSceneWindow); scene != nullptr && scene->DockId != 0) {
            ImGui::SetNextWindowDockID(scene->DockId, ImGuiCond_FirstUseEver);
        }
        bool open = true;
        if (ImGui::Begin(title.c_str(), &open)) {
            if (ImGui::Button("Save")) {
                if (auto ok = project->saveScript(script.id); !ok) app.notify(ok.error().message, true);
            }
            ui::tooltip("Write this script to disk (Ctrl+S saves everything). Play uses unsaved changes too.");
            ImGui::SameLine();
            ImGui::BeginDisabled(!dirty);
            if (ImGui::Button("Revert")) {
                project->discardScriptBuffer(script.id);
                if (auto disk = project->scriptText(script.id)) script.text = disk.value();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Open In System Editor")) app.openPath(project->absolutePath(script.id));
            ImGui::SameLine();
            ImGui::TextDisabled("%s   Ln %d", entry->path.c_str(), script.cursorLine);

            const editor::ScriptInfo* info = project->scriptInfo(script.id);
            const float statusHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f;
            CallbackState state{&script.cursorLine, &script.gotoLine};
            ImGui::PushFont(app.monoFont(), 0.0f);
            if (script.gotoLine > 0) ImGui::SetKeyboardFocusHere();
            const bool changed = ImGui::InputTextMultiline(
                "##code", &script.text, {-FLT_MIN, ImGui::GetContentRegionAvail().y - statusHeight},
                ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackAlways, editorCallback, &state);
            ImGui::PopFont();
            if (changed) project->setScriptBuffer(script.id, script.text);

            if (info != nullptr && !info->error.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ui::kErrorColor, "%s", info->error.c_str());
                ImGui::PopTextWrapPos();
                const int line = errorLine(info->error);
                if (line > 0) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Go to line")) script.gotoLine = line;
                }
            } else if (info != nullptr) {
                std::string summary = "Compiles. ";
                if (!info->behaviour.properties.empty()) {
                    summary += "Properties:";
                    for (const auto& [name, value] : info->behaviour.properties) summary += " " + name;
                    summary += ". ";
                }
                if (!info->behaviour.events.empty()) {
                    summary += "Handles:";
                    for (const auto& e : info->behaviour.events) summary += " " + e;
                }
                ImGui::TextColored(ui::kDimColor, "%s", summary.c_str());
            }
        }
        ImGui::End();
        if (!open) closeIndex = i;
    }
    if (closeIndex) {
        // Unsaved text stays in the project (marked unsaved) and is saved by Save All.
        if (project->scriptBufferDirty(m_open[*closeIndex].id)) {
            app.notify("The script has unsaved changes; File > Save All saves them");
        }
        m_open.erase(m_open.begin() + static_cast<std::ptrdiff_t>(*closeIndex));
    }
}

} // namespace px::studio
