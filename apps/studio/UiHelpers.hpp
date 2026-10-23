#pragma once

#include "px/assets/AssetId.hpp"
#include "px/core/Color.hpp"
#include "px/core/Math.hpp"
#include "px/editor/SceneDocument.hpp"
#include "px/render/TextureCache.hpp"

#include <imgui.h>

#include <string>
#include <string_view>

namespace px::studio {

class StudioApp;

namespace ui {

inline constexpr ImVec4 kErrorColor{1.0f, 0.42f, 0.42f, 1.0f};
inline constexpr ImVec4 kWarningColor{1.0f, 0.78f, 0.35f, 1.0f};
inline constexpr ImVec4 kDimColor{0.6f, 0.62f, 0.66f, 1.0f};
inline constexpr ImVec4 kAccentColor{0.45f, 0.75f, 1.0f, 1.0f};
inline constexpr ImVec4 kPlayColor{1.0f, 0.6f, 0.2f, 1.0f};

// Drag-and-drop payload types.
inline constexpr const char* kAssetPayload = "PX_ASSET";   // assets::AssetId
inline constexpr const char* kEntityPayload = "PX_ENTITY"; // Guid
inline constexpr const char* kLibraryPayload = "PX_LIBRARY"; // LibraryPayload

// Something dragged from the Toolbox.
struct LibraryPayload {
    enum class Kind : int { Object, Image, Behaviour } kind = Kind::Object;
    int object = -1;         // Object: index in the library
    assets::AssetId asset;   // Image, Behaviour
};

// Connects inspector widgets to the document's undo transactions.
//
// Widgets edit a copy of the value; when the copy changed, commit() opens a
// transaction (before the real value is touched), applies the change and
// marks the document changed. The transaction is closed when the widget is
// released, so a whole drag or a typed word is one undo step. While playing
// there is no document: changes go straight into the running game.
class EditSession {
public:
    void setDocument(editor::SceneDocument* doc) {
        if (doc != m_doc) close();
        m_doc = doc;
    }
    template <class Apply>
    bool commit(bool changed, std::string_view label, Apply&& apply) {
        if (changed) {
            if (!m_open) {
                if (m_doc != nullptr) m_doc->beginEdit(label);
                m_open = true;
            }
            apply();
            if (m_doc != nullptr) m_doc->markChanged();
        }
        if (m_open && (ImGui::IsItemDeactivatedAfterEdit() || (changed && !ImGui::IsItemActive()))) close();
        return changed;
    }
    // Closes a transaction left open when no widget is active any more.
    void endFrame() {
        if (m_open && !ImGui::IsAnyItemActive()) close();
    }
    void close() {
        if (m_open && m_doc != nullptr) m_doc->endEdit();
        m_open = false;
    }

private:
    editor::SceneDocument* m_doc = nullptr;
    bool m_open = false;
};

void helpMarker(std::string_view text);
// `text`, shortened with ".." (by whole UTF-8 characters) to fit `width`.
[[nodiscard]] std::string fitText(std::string text, float width);
void tooltip(std::string_view text);
bool iconButton(const char* label, const char* tooltipText, bool enabled = true);
ImVec4 toImVec4(Color c);
Color fromImVec4(const ImVec4& v);
// ImGui texture id for an SDL texture handle of the app's renderer.
ImTextureID textureId(StudioApp& app, unsigned int handle);
// Draws a texture asset (or part of it) fitted into `size`, keeping its aspect ratio.
// With an `id` the area is an invisible button (a drag source or click target), and
// the result is true when it was clicked (pressed and released on it).
bool textureImage(StudioApp& app, assets::AssetId texture, ImVec2 size, Rect source = {}, const char* id = nullptr);
// The same for a texture of another cache (the starter library's).
bool textureImage(StudioApp& app, render::TextureCache* cache, assets::AssetId texture, ImVec2 size, Rect source = {},
                  const char* id = nullptr);

} // namespace ui
} // namespace px::studio
