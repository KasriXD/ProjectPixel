#include "Panels.hpp"
#include "StudioApp.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <cctype>
#include <format>

namespace px::studio {

namespace {

// Finds "some/script.luau:12" in a message.
bool findScriptLocation(const std::string& text, std::string& path, int& line) {
    const std::size_t at = text.find(".luau:");
    if (at == std::string::npos) return false;
    std::size_t start = at;
    while (start > 0 && !std::isspace(static_cast<unsigned char>(text[start - 1])) && text[start - 1] != '[' &&
           text[start - 1] != '(') {
        --start;
    }
    path = text.substr(start, at + 5 - start);
    std::size_t p = at + 6;
    line = 0;
    while (p < text.size() && std::isdigit(static_cast<unsigned char>(text[p])) != 0) {
        line = line * 10 + (text[p] - '0');
        ++p;
    }
    return line > 0;
}

} // namespace

void ConsolePanel::draw(StudioApp& app) {
    if (!ImGui::Begin(kOutputWindow)) {
        ImGui::End();
        return;
    }
    auto& log = app.log();
    if (ImGui::Button("Clear")) log.clear();
    ImGui::SameLine();
    ImGui::Checkbox(std::format("Info ({})", log.count(log::Level::Info)).c_str(), &m_showInfo);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kWarningColor);
    ImGui::Checkbox(std::format("Warnings ({})", log.count(log::Level::Warn)).c_str(), &m_showWarnings);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kErrorColor);
    ImGui::Checkbox(std::format("Errors ({})", log.count(log::Level::Error)).c_str(), &m_showErrors);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_autoScroll);
    ImGui::SameLine();
    ImGui::Checkbox("Clear on Play", &app.settings().clearConsoleOnPlay);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "Filter", &m_filter);

    ImGui::BeginChild("log", {0, 0}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const auto entries = log.entries();
    ImGui::PushFont(app.monoFont(), 0.0f);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (e.level <= log::Level::Info && !m_showInfo) continue;
        if (e.level == log::Level::Warn && !m_showWarnings) continue;
        if (e.level == log::Level::Error && !m_showErrors) continue;
        if (!m_filter.empty() && e.text.find(m_filter) == std::string::npos) continue;
        ImGui::PushID(static_cast<int>(i));
        ImVec4 colour = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        if (e.level == log::Level::Warn) colour = ui::kWarningColor;
        if (e.level == log::Level::Error) colour = ui::kErrorColor;
        if (e.level <= log::Level::Debug) colour = ui::kDimColor;
        ImGui::TextColored(ui::kDimColor, "%7.2f", e.seconds);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, colour);
        const std::string text = e.repeats > 1 ? std::format("{}  (x{})", e.text, e.repeats) : e.text;
        ImGui::Selectable(text.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
        ImGui::PopStyleColor();
        std::string path;
        int line = 0;
        const bool hasLocation = findScriptLocation(e.text, path, line);
        if (hasLocation) ui::tooltip(std::format("Double-click to open {} at line {}", path, line));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hasLocation &&
            app.project() != nullptr) {
            for (const auto& entry : app.project()->assets().entries()) {
                if (entry.type == assets::AssetType::Script && entry.path == path) app.openScript(entry.id, line);
            }
        }
        if (ImGui::BeginPopupContextItem("copy")) {
            if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(e.text.c_str());
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::PopFont();
    if (m_autoScroll && log.revision() != m_seenRevision) {
        ImGui::SetScrollHereY(1.0f);
        m_seenRevision = log.revision();
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace px::studio
