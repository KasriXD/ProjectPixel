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

std::string fitText(std::string text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    while (!text.empty()) {
        // Drop one whole UTF-8 character: continuation bytes, then its first byte.
        unsigned char removed = 0;
        do {
            removed = static_cast<unsigned char>(text.back());
            text.pop_back();
        } while (!text.empty() && (removed & 0xC0u) == 0x80u);
        if (ImGui::CalcTextSize((text + "..").c_str()).x <= width) break;
    }
    return text + "..";
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

void drawTileMarker(ImDrawList* dl, ImVec2 a, ImVec2 b, const project::TileInfo& info, bool showSolid) {
    const ImU32 line = IM_COL32(255, 230, 80, 255);
    const ImU32 shadow = IM_COL32(0, 0, 0, 160);
    const float w = b.x - a.x;
    const float t = std::max(1.5f, w / 12.0f);
    using C = project::TileCollision;
    switch (info.collision) {
        case C::Solid:
            if (showSolid) dl->AddRect(a, b, IM_COL32(255, 120, 90, 160), 0.0f, t);
            break;
        case C::None: {
            const float m = w * 0.3f;
            dl->AddLine({a.x + m, a.y + m}, {b.x - m, b.y - m}, IM_COL32(150, 220, 255, 220), t);
            dl->AddLine({b.x - m, a.y + m}, {a.x + m, b.y - m}, IM_COL32(150, 220, 255, 220), t);
            break;
        }
        case C::OneWay:
            dl->AddRectFilled({a.x, a.y}, {b.x, a.y + t * 1.5f}, shadow);
            dl->AddRectFilled({a.x, a.y}, {b.x, a.y + t}, line);
            dl->AddTriangleFilled({(a.x + b.x) * 0.5f, a.y + w * 0.25f}, {(a.x + b.x) * 0.5f - w * 0.12f, a.y + w * 0.45f},
                                  {(a.x + b.x) * 0.5f + w * 0.12f, a.y + w * 0.45f}, line);
            break;
        case C::SlopeUpRight:
            dl->AddLine({a.x, b.y}, {b.x, a.y}, shadow, t * 1.8f);
            dl->AddLine({a.x, b.y}, {b.x, a.y}, line, t);
            break;
        case C::SlopeUpLeft:
            dl->AddLine({a.x, a.y}, {b.x, b.y}, shadow, t * 1.8f);
            dl->AddLine({a.x, a.y}, {b.x, b.y}, line, t);
            break;
    }
    if (!info.tags.empty()) {
        dl->AddCircleFilled({b.x - w * 0.18f, b.y - w * 0.18f}, std::max(2.0f, w * 0.1f), IM_COL32(120, 255, 160, 255));
    }
}

bool textureImage(StudioApp& app, assets::AssetId texture, ImVec2 size, Rect source, const char* id) {
    return textureImage(app, app.textures(), texture, size, source, id);
}

bool textureImage(StudioApp& app, render::TextureCache* cache, assets::AssetId texture, ImVec2 size, Rect source,
                  const char* id) {
    const auto reserve = [&] {
        if (id != nullptr) return ImGui::InvisibleButton(id, size);
        ImGui::Dummy(size);
        return false;
    };
    if (cache == nullptr || texture.isNil()) return reserve();
    const render::TextureHandle handle = cache->get(texture);
    const Vec2i texSize = app.renderer().textureSize(handle);
    if (texSize.x <= 0 || texSize.y <= 0) return reserve();
    const Rect src = source.empty() ? Rect{0, 0, static_cast<float>(texSize.x), static_cast<float>(texSize.y)} : source;
    const float scale = std::min(size.x / src.w, size.y / src.h);
    const ImVec2 drawn{src.w * scale, src.h * scale};
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool pressed = reserve(); // reserves the space and is the hover/drag target
    const ImVec2 p0{origin.x + (size.x - drawn.x) * 0.5f, origin.y + (size.y - drawn.y) * 0.5f};
    const ImVec2 uv0{src.x / static_cast<float>(texSize.x), src.y / static_cast<float>(texSize.y)};
    const ImVec2 uv1{(src.x + src.w) / static_cast<float>(texSize.x), (src.y + src.h) / static_cast<float>(texSize.y)};
    ImGui::GetWindowDrawList()->AddImage(ImTextureRef(textureId(app, handle)), p0, {p0.x + drawn.x, p0.y + drawn.y},
                                         uv0, uv1);
    return pressed;
}

} // namespace px::studio::ui
