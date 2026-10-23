#include "UiHelpers.hpp"

#include "StudioApp.hpp"

#include <algorithm>
#include <string>

namespace px::studio::ui {

void tooltip(std::string_view text) {
    if (text.empty() || !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void helpMarker(std::string_view text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    tooltip(text);
}

bool iconButton(const char* label, const char* tooltipText, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(label);
    ImGui::EndDisabled();
    if (tooltipText != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", tooltipText);
    }
    return pressed;
}

ImVec4 toImVec4(Color c) {
    return {static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f, static_cast<float>(c.b) / 255.0f,
            static_cast<float>(c.a) / 255.0f};
}

Color fromImVec4(const ImVec4& v) {
    const auto channel = [](float f) {
        return static_cast<std::uint8_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return {channel(v.x), channel(v.y), channel(v.z), channel(v.w)};
}

ImTextureID textureId(StudioApp& app, unsigned int handle) {
    return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(app.renderer().sdlTexture(handle)));
}

void textureImage(StudioApp& app, assets::AssetId texture, ImVec2 size, Rect source, const char* id) {
    const auto reserve = [&] {
        if (id != nullptr) {
            ImGui::InvisibleButton(id, size);
        } else {
            ImGui::Dummy(size);
        }
    };
    render::TextureCache* cache = app.textures();
    if (cache == nullptr || texture.isNil()) {
        reserve();
        return;
    }
    const render::TextureHandle handle = cache->get(texture);
    const Vec2i texSize = app.renderer().textureSize(handle);
    if (texSize.x <= 0 || texSize.y <= 0) {
        reserve();
        return;
    }
    const Rect src = source.empty() ? Rect{0, 0, static_cast<float>(texSize.x), static_cast<float>(texSize.y)} : source;
    const float scale = std::min(size.x / src.w, size.y / src.h);
    const ImVec2 drawn{src.w * scale, src.h * scale};
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    reserve(); // reserves the space and is the hover/drag target
    const ImVec2 p0{origin.x + (size.x - drawn.x) * 0.5f, origin.y + (size.y - drawn.y) * 0.5f};
    const ImVec2 uv0{src.x / static_cast<float>(texSize.x), src.y / static_cast<float>(texSize.y)};
    const ImVec2 uv1{(src.x + src.w) / static_cast<float>(texSize.x), (src.y + src.h) / static_cast<float>(texSize.y)};
    ImGui::GetWindowDrawList()->AddImage(ImTextureRef(textureId(app, handle)), p0, {p0.x + drawn.x, p0.y + drawn.y},
                                         uv0, uv1);
}

} // namespace px::studio::ui
