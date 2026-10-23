#include "Panels.hpp"
#include "StudioApp.hpp"

#include "px/editor/Picking.hpp"
#include "px/render/SceneRenderer.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace px::studio {

using namespace px::scene;

namespace {

// Zoom levels in framebuffer pixels per world unit. Whole numbers keep
// pixel art crisp; fractions are for overviews.
constexpr float kZoomLevels[] = {0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f, 10.0f, 12.0f, 16.0f, 24.0f, 32.0f};

float nextZoom(float zoom, int direction) {
    if (direction > 0) {
        for (const float z : kZoomLevels) {
            if (z > zoom + 1e-4f) return z;
        }
        return kZoomLevels[std::size(kZoomLevels) - 1];
    }
    for (auto it = std::rbegin(kZoomLevels); it != std::rend(kZoomLevels); ++it) {
        if (*it < zoom - 1e-4f) return *it;
    }
    return kZoomLevels[0];
}

ImU32 color(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

Entity selectedTilemap(StudioApp& app) {
    const Entity e = app.primarySelection();
    Scene* scene = app.activeScene();
    return scene != nullptr && scene->tryGet<Tilemap>(e) != nullptr ? e : kNullEntity;
}

// Cells on the line between two tile cells (inclusive), so fast strokes leave no gaps.
template <class Fn>
void forEachCellOnLine(int x0, int y0, int x1, int y1, Fn&& fn) {
    const int dx = std::abs(x1 - x0);
    const int dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 100000; ++guard) {
        fn(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void hudText(render::IRenderer2D& r, Vec2 p, const std::string& text, Color c) {
    r.drawDebugText({p.x + 1, p.y + 1}, text, Color{30, 24, 40, 200});
    r.drawDebugText(p, text, c);
}

} // namespace

Vec2 ViewportPanel::screenToWorld(ImVec2 screen, ImVec2 origin) const {
    return m_camera.screenToWorld({(screen.x - origin.x) * m_dpi, (screen.y - origin.y) * m_dpi});
}

ImVec2 ViewportPanel::worldToScreen(Vec2 world, ImVec2 origin) const {
    const Vec2 p = m_camera.worldToScreen(world);
    return {origin.x + p.x / m_dpi, origin.y + p.y / m_dpi};
}

void ViewportPanel::ensureTarget(StudioApp& app, Vec2i size, render::TextureHandle& target, Vec2i& current) {
    size.x = std::clamp(size.x, 1, 8192);
    size.y = std::clamp(size.y, 1, 8192);
    if (target != render::kNoTexture && current == size) return;
    if (target != render::kNoTexture) app.renderer().destroyTexture(target);
    target = app.renderer().createRenderTarget(size);
    current = size;
}

void ViewportPanel::frameSelection(StudioApp& app) {
    Scene* scene = app.activeScene();
    if (scene == nullptr) return;
    Rect bounds;
    bool any = false;
    std::vector<Entity> targets = app.selection();
    if (targets.empty()) targets = scene->entities();
    for (const Entity e : targets) {
        const Rect b = editor::entityBounds(*scene, e);
        if (b.empty()) continue;
        bounds = any ? unite(bounds, b) : b;
        any = true;
    }
    if (!any) return;
    m_camera.center = bounds.center();
    // Fit the bounds with some margin, using a whole-number zoom where possible.
    const float fit = std::min(m_camera.viewport.x / std::max(bounds.w * 1.3f, 32.0f),
                               m_camera.viewport.y / std::max(bounds.h * 1.3f, 32.0f));
    float zoom = kZoomLevels[0];
    for (const float z : kZoomLevels) {
        if (z <= fit) zoom = z;
    }
    m_camera.zoom = zoom;
}

void ViewportPanel::draw(StudioApp& app) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    const bool visible =
        ImGui::Begin(kSceneWindow, nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!visible) {
        app.setGameInputFocus(false);
        ImGui::End();
        return;
    }
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (focused && !app.playing()) app.entityShortcuts = true;
    app.setGameInputFocus(focused && app.playing());
    Scene* scene = app.activeScene();
    if (scene == nullptr) {
        ImGui::TextDisabled("No scene is open.");
        ImGui::End();
        return;
    }
    drawToolbar(app);
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 8.0f || size.y < 8.0f) {
        ImGui::End();
        return;
    }
    m_dpi = std::max(0.5f, ImGui::GetIO().DisplayFramebufferScale.x);
    if (app.playing() && app.gameCameraInPlay) {
        drawGameView(app, size);
    } else {
        drawSceneView(app, size);
    }
    ImGui::End();
}

void ViewportPanel::drawToolbar(StudioApp& app) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.0f, 3.0f});
    ImGui::SetCursorPos({ImGui::GetCursorPosX() + 6.0f, ImGui::GetCursorPosY() + 4.0f});
    const auto toolButton = [&](const char* label, Tool t, const char* tip) {
        const bool active = app.tool == t;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        if (ImGui::Button(label)) app.tool = t;
        if (active) ImGui::PopStyleColor();
        ui::tooltip(tip);
        ImGui::SameLine();
    };
    const bool playing = app.playing();
    if (!(playing && app.gameCameraInPlay)) {
        toolButton("Select", Tool::Select, "Select and move entities (Q). Drag empty space to select an area.");
        toolButton("Paint", Tool::Paint, "Paint tiles into the selected tilemap (B)");
        toolButton("Erase", Tool::Erase, "Erase tiles of the selected tilemap (E)");
        toolButton("Pan", Tool::Pan, "Drag to move the view (H). The middle mouse button always pans.");
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        auto& s = app.settings();
        ImGui::Checkbox("Grid", &s.showGrid);
        ImGui::SameLine();
        ImGui::Checkbox("Snap", &s.snapToGrid);
        ui::tooltip("Snap moved entities to the grid (hold Alt while dragging to move freely)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.5f);
        ImGui::DragFloat("##grid", &s.gridSize, 0.25f, 1.0f, 256.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
        ui::tooltip("Grid size in world units");
        ImGui::SameLine();
        ImGui::Checkbox("Colliders", &s.showColliders);
        ImGui::SameLine();
        ImGui::TextDisabled("%.0f%%", m_camera.zoom / m_dpi * 100.0f);
        ui::tooltip("Zoom (mouse wheel). F frames the selection.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Frame")) frameSelection(app);
    }
    if (playing) {
        ImGui::SameLine();
        ImGui::Checkbox("Game camera", &app.gameCameraInPlay);
        ui::tooltip("Show what the player sees, or a free editor camera to inspect the running game");
        ImGui::SameLine();
        ImGui::TextColored(ui::kPlayColor, "%s", app.play()->paused() ? "PAUSED" : "PLAYING");
    }
    ImGui::PopStyleVar();
    ImGui::Separator();
}

void ViewportPanel::drawSceneView(StudioApp& app, ImVec2 size) {
    Scene& scene = *app.activeScene();
    const Vec2i pixels{static_cast<int>(size.x * m_dpi), static_cast<int>(size.y * m_dpi)};
    ensureTarget(app, pixels, m_sceneTarget, m_sceneTargetSize);
    m_camera.viewport = {static_cast<float>(m_sceneTargetSize.x), static_cast<float>(m_sceneTargetSize.y)};
    m_camera.pixelSnap = true;
    // The first frames run before the docking layout settles the window
    // size, so the initial framing is repeated for a few frames.
    if (!m_cameraReady || m_initFrames > 0) {
        if (!m_cameraReady) m_initFrames = 3;
        m_cameraReady = true;
        --m_initFrames;
        m_camera.zoom = 2.0f * m_dpi;
        const auto* project = app.project();
        if (project != nullptr) {
            const Vec2i res = project->manifest().settings.display.virtualResolution;
            float zoom = kZoomLevels[0];
            for (const float z : kZoomLevels) {
                if (static_cast<float>(res.x) * z <= m_camera.viewport.x && static_cast<float>(res.y) * z <= m_camera.viewport.y) {
                    zoom = z;
                }
            }
            m_camera.zoom = std::max(zoom, 1.0f);
            m_camera.center = {static_cast<float>(res.x) * 0.5f, static_cast<float>(res.y) * 0.5f};
        }
        for (const Entity e : scene.entities()) {
            if (scene.tryGet<Camera>(e) != nullptr && scene.tryGet<Transform>(e) != nullptr) {
                m_camera.center = scene.worldPosition(e);
                break;
            }
        }
    }

    auto& renderer = app.renderer();
    if (m_sceneTarget != render::kNoTexture && app.textures() != nullptr) {
        renderer.setRenderTarget(m_sceneTarget);
        renderer.beginFrame(scene.settings().backgroundColor);
        render::renderScene(scene, m_camera, renderer, *app.textures());
        renderer.setRenderTarget(render::kNoTexture);
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddImage(ImTextureRef(ui::textureId(app, m_sceneTarget)), origin,
                 {origin.x + static_cast<float>(m_sceneTargetSize.x) / m_dpi,
                  origin.y + static_cast<float>(m_sceneTargetSize.y) / m_dpi});
    m_hovered = ImGui::IsItemHovered();
    handleDrop(app, origin);
    handleInput(app, scene, origin, size);
    dl->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
    drawOverlays(app, scene, origin, size);
    dl->PopClipRect();
}

void ViewportPanel::drawGameView(StudioApp& app, ImVec2 size) {
    auto* play = app.play();
    const auto* project = app.project();
    if (play == nullptr || project == nullptr) return;
    Scene& scene = play->session().scene();
    const Vec2i res = project->manifest().settings.display.virtualResolution;
    ensureTarget(app, res, m_gameTarget, m_gameTargetSize);
    const Vec2 view{static_cast<float>(res.x), static_cast<float>(res.y)};
    const render::Camera2D camera = render::cameraForScene(scene, view);
    auto& r = app.renderer();
    r.setRenderTarget(m_gameTarget);
    r.beginFrame(scene.settings().backgroundColor);
    render::renderScene(scene, camera, r, *app.textures());
    if (app.settings().showColliders && !app.gameCameraInPlay) render::renderCollisionOverlay(scene, camera, r);

    // A minimal HUD, like px-player's: objective, win banner, script errors.
    const auto& state = play->session().state();
    constexpr float g = render::IRenderer2D::kDebugGlyphSize;
    if (!state.objective.empty()) hudText(r, {6, 6}, state.objective, Color{255, 236, 140, 255});
    if (state.levelComplete) {
        std::string line = state.completionMessage.empty() ? std::string("LEVEL COMPLETE!") : state.completionMessage;
        const std::size_t maxChars = static_cast<std::size_t>(std::max(1.0f, (view.x - 32.0f) / g));
        if (line.size() > maxChars) line.resize(maxChars);
        const float w = g * static_cast<float>(line.size()) + 24.0f;
        const Rect box{(view.x - w) * 0.5f, view.y * 0.5f - 14.0f, w, 28.0f};
        r.fillRect(box, Color{20, 16, 40, 200});
        r.drawRect(box, Color{255, 236, 140, 255});
        hudText(r, {(view.x - g * static_cast<float>(line.size())) * 0.5f, box.y + 10.0f}, line,
                Color{255, 236, 140, 255});
    }
    if (state.scriptErrors > 0) {
        const std::string line = std::format("SCRIPT ERRORS: {} (SEE CONSOLE)", state.scriptErrors);
        r.fillRect({0, view.y - 12.0f, view.x, 12.0f}, Color{120, 20, 30, 220});
        r.drawDebugText({4, view.y - 10.0f}, line, colors::white);
    }
    r.setRenderTarget(render::kNoTexture);

    // Largest whole-number scale that fits, centred (crisp pixels, like px-player).
    const float fit = std::min(size.x * m_dpi / view.x, size.y * m_dpi / view.y);
    const float scale = fit >= 1.0f ? std::floor(fit) : fit;
    const ImVec2 shown{view.x * scale / m_dpi, view.y * scale / m_dpi};
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("game", size);
    const ImVec2 p0{cursor.x + (size.x - shown.x) * 0.5f, cursor.y + (size.y - shown.y) * 0.5f};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(cursor, {cursor.x + size.x, cursor.y + size.y}, color(12, 12, 16));
    dl->AddImage(ImTextureRef(ui::textureId(app, m_gameTarget)), p0, {p0.x + shown.x, p0.y + shown.y});
    if (ImGui::IsItemClicked()) ImGui::SetWindowFocus();
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const char* hint = focused ? "Keyboard goes to the game. F5 stops, F6 pauses."
                               : "Click the game to give it the keyboard.";
    dl->AddText({cursor.x + 8.0f, cursor.y + size.y - ImGui::GetFontSize() - 6.0f},
                focused ? color(160, 165, 175) : color(255, 200, 120), hint);
}

void ViewportPanel::handleDrop(StudioApp& app, ImVec2 origin) {
    if (app.playing() || !ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ui::kAssetPayload)) {
        assets::AssetId id;
        std::memcpy(&id, payload->Data, sizeof(id));
        auto* project = app.project();
        auto* doc = app.document();
        const auto* entry = project != nullptr ? project->asset(id) : nullptr;
        if (entry != nullptr && doc != nullptr) {
            const Vec2 world = app.placeOnGrid(screenToWorld(ImGui::GetMousePos(), origin));
            switch (entry->type) {
                case assets::AssetType::Texture: {
                    const Entity e = doc->createEntity(editor::EntityPreset::Sprite, entry->name, kNullEntity, world, id);
                    // Size the sprite like the image.
                    const Vec2i texSize = app.renderer().textureSize(app.textures()->get(id));
                    if (texSize.x > 0 && texSize.y > 0) {
                        doc->beginEdit("Create Sprite");
                        doc->sceneForEdit().tryGet<Sprite>(e)->size = {static_cast<float>(texSize.x),
                                                                       static_cast<float>(texSize.y)};
                        doc->markChanged();
                        doc->endEdit();
                    }
                    break;
                }
                case assets::AssetType::Script: {
                    const Entity target = editor::pick(doc->scene(), screenToWorld(ImGui::GetMousePos(), origin));
                    if (target.isNull()) {
                        app.notify("Drop a script onto an entity to attach it", true);
                    } else if (auto ok = doc->attachScript(target, id); !ok) {
                        app.notify(ok.error().message, true);
                    } else {
                        doc->select(target);
                        app.notify(std::format("Attached {} to {}", entry->name,
                                               doc->scene().tryGet<EntityInfo>(target)->name));
                    }
                    break;
                }
                case assets::AssetType::Scene: app.requestOpenScene(id); break;
            }
        }
    }
    ImGui::EndDragDropTarget();
}

void ViewportPanel::handleInput(StudioApp& app, Scene& scene, ImVec2 origin, ImVec2 size) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const Vec2 world = screenToWorld(mouse, origin);
    auto* doc = app.document();
    const bool playing = app.playing();

    // Zoom around the mouse.
    if (m_hovered && io.MouseWheel != 0.0f) {
        const Vec2 before = world;
        m_camera.zoom = nextZoom(m_camera.zoom, io.MouseWheel > 0.0f ? 1 : -1);
        const Vec2 after = screenToWorld(mouse, origin);
        m_camera.center += before - after;
    }

    // Keyboard (when the scene view has focus and nobody is typing).
    const bool keyboard = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput;
    if (keyboard && !playing) {
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) app.tool = Tool::Select;
        if (ImGui::IsKeyPressed(ImGuiKey_B, false)) app.tool = Tool::Paint;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) app.tool = Tool::Erase;
        if (ImGui::IsKeyPressed(ImGuiKey_H, false)) app.tool = Tool::Pan;
        const float step = io.KeyShift ? app.settings().gridSize : 1.0f;
        Vec2 nudge{};
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) nudge.x -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) nudge.x += step;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) nudge.y -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) nudge.y += step;
        if ((nudge.x != 0.0f || nudge.y != 0.0f) && doc != nullptr && m_drag == Drag::None) doc->translateSelection(nudge);
        if (m_drag == Drag::Move && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (doc != nullptr && doc->inEdit()) doc->cancelEdit();
            m_drag = Drag::None;
        }
    }
    if (keyboard && ImGui::IsKeyPressed(ImGuiKey_F, false)) frameSelection(app);

    // Panning: middle button, right button in the select tool, Space + left, or the pan tool.
    const bool spaceHeld = keyboard && ImGui::IsKeyDown(ImGuiKey_Space);
    if (m_hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
                      (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && app.tool != Tool::Paint && app.tool != Tool::Erase) ||
                      (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && (app.tool == Tool::Pan || spaceHeld)))) {
        m_drag = Drag::Pan;
    }
    if (m_drag == Drag::Pan) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
            ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_camera.center -= Vec2{io.MouseDelta.x, io.MouseDelta.y} * (m_dpi / m_camera.zoom);
        } else {
            m_drag = Drag::None;
        }
        return;
    }

    // Tile painting.
    if ((app.tool == Tool::Paint || app.tool == Tool::Erase) && !playing && doc != nullptr) {
        const Entity map = selectedTilemap(app);
        const bool leftPress = m_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        const bool rightPress = m_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        if (!map.isNull() && (leftPress || rightPress)) {
            const editor::TileCoord cell = editor::tileAt(scene, map, world);
            if (leftPress && io.KeyAlt) {
                const auto* tm = scene.tryGet<Tilemap>(map);
                if (cell.inside && tm->tileAt(cell.x, cell.y) >= 0) {
                    app.paintTile = tm->tileAt(cell.x, cell.y);
                    app.tool = Tool::Paint;
                }
            } else {
                m_drag = Drag::Paint;
                m_painted = rightPress || app.tool == Tool::Erase; // erasing stroke
                doc->beginEdit(m_painted ? "Erase Tiles" : "Paint Tiles");
                m_lastPaintX = cell.x;
                m_lastPaintY = cell.y;
                doc->setTile(map, cell.x, cell.y, m_painted ? -1 : app.paintTile);
            }
        }
        if (m_drag == Drag::Paint) {
            const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right);
            if (down && !map.isNull()) {
                const editor::TileCoord cell = editor::tileAt(scene, map, world);
                if (cell.x != m_lastPaintX || cell.y != m_lastPaintY) {
                    forEachCellOnLine(m_lastPaintX, m_lastPaintY, cell.x, cell.y, [&](int x, int y) {
                        doc->setTile(map, x, y, m_painted ? -1 : app.paintTile);
                    });
                    m_lastPaintX = cell.x;
                    m_lastPaintY = cell.y;
                }
            } else {
                doc->endEdit();
                m_drag = Drag::None;
            }
        }
        return;
    }

    // Selection and moving.
    const float handle = 10.0f * m_dpi / m_camera.zoom;
    if (m_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && app.tool == Tool::Select) {
        const auto hits = editor::pickAll(scene, world, handle);
        Entity hit = hits.empty() ? kNullEntity : hits.front();
        // Clicking the same spot again cycles through what is under the mouse.
        const ImVec2 last = worldToScreen(m_lastClickWorld, origin);
        const bool sameSpot = std::abs(last.x - mouse.x) < 3.0f && std::abs(last.y - mouse.y) < 3.0f;
        if (sameSpot && hits.size() > 1) {
            const auto current = std::find(hits.begin(), hits.end(), app.primarySelection());
            if (current != hits.end()) hit = *(std::next(current) == hits.end() ? hits.begin() : std::next(current));
        }
        m_lastClickWorld = world;
        if (io.KeyCtrl || io.KeyShift) {
            if (!hit.isNull()) app.select(hit, io.KeyCtrl ? editor::SelectMode::Toggle : editor::SelectMode::Add);
            else {
                m_drag = Drag::Marquee;
                m_dragStartScreen = mouse;
            }
        } else if (hit.isNull()) {
            app.select(kNullEntity);
            m_drag = Drag::Marquee;
            m_dragStartScreen = mouse;
        } else {
            const auto selected = app.selection();
            if (std::find(selected.begin(), selected.end(), hit) == selected.end()) app.select(hit);
            m_drag = Drag::Move;
            m_dragStartWorld = world;
            m_dragApplied = {};
            m_dragStartScreen = mouse;
            m_painted = false; // used as "the move has started" for moves
        }
    }
    if (m_drag == Drag::Move) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float moved = std::abs(mouse.x - m_dragStartScreen.x) + std::abs(mouse.y - m_dragStartScreen.y);
            if (!m_painted && moved > 3.0f) {
                m_painted = true;
                if (!playing && doc != nullptr) doc->beginEdit("Move");
            }
            if (m_painted) {
                Vec2 delta = world - m_dragStartWorld;
                // Snapping moves by whole grid steps, so entities keep their
                // place within a cell (sprites centred on tiles stay centred).
                if (app.settings().snapToGrid && !io.KeyAlt) delta = editor::snapToGrid(delta, app.settings().gridSize);
                const Vec2 step = delta - m_dragApplied;
                if (step.x != 0.0f || step.y != 0.0f) {
                    if (playing) {
                        for (const Entity e : app.selection()) {
                            if (scene.tryGet<Transform>(e) != nullptr) scene.setWorldPosition(e, scene.worldPosition(e) + step);
                        }
                    } else if (doc != nullptr) {
                        doc->translateSelection(step);
                    }
                    m_dragApplied = delta;
                }
            }
        } else {
            if (m_painted && !playing && doc != nullptr && doc->inEdit()) doc->endEdit();
            m_drag = Drag::None;
        }
    }
    if (m_drag == Drag::Marquee && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const Vec2 a = screenToWorld(m_dragStartScreen, origin);
        const Rect rect{std::min(a.x, world.x), std::min(a.y, world.y), std::abs(world.x - a.x), std::abs(world.y - a.y)};
        if (rect.w > 1.0f || rect.h > 1.0f) {
            const auto inside = editor::entitiesInside(scene, rect, handle);
            if (io.KeyShift || io.KeyCtrl) {
                for (const Entity e : inside) app.select(e, editor::SelectMode::Add);
            } else if (doc != nullptr) {
                if (playing) {
                    app.select(kNullEntity);
                    for (const Entity e : inside) app.select(e, editor::SelectMode::Add);
                } else {
                    doc->setSelection(inside);
                }
            }
        }
        m_drag = Drag::None;
    }
    (void)size;
}

void ViewportPanel::drawOverlays(StudioApp& app, Scene& scene, ImVec2 origin, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& s = app.settings();
    const Vec2 topLeft = screenToWorld(origin, origin);
    const Vec2 bottomRight = screenToWorld({origin.x + size.x, origin.y + size.y}, origin);

    // Grid.
    const float spacing = s.gridSize * m_camera.zoom / m_dpi;
    if (s.showGrid && spacing >= 6.0f) {
        const ImU32 line = color(255, 255, 255, spacing >= 16.0f ? 34 : 18);
        const float x0 = std::floor(topLeft.x / s.gridSize) * s.gridSize;
        for (float x = x0; x <= bottomRight.x; x += s.gridSize) {
            const float sx = worldToScreen({x, 0}, origin).x;
            dl->AddLine({sx, origin.y}, {sx, origin.y + size.y}, line);
        }
        const float y0 = std::floor(topLeft.y / s.gridSize) * s.gridSize;
        for (float y = y0; y <= bottomRight.y; y += s.gridSize) {
            const float sy = worldToScreen({0, y}, origin).y;
            dl->AddLine({origin.x, sy}, {origin.x + size.x, sy}, line);
        }
    }
    // World axes.
    const ImVec2 zero = worldToScreen({0, 0}, origin);
    dl->AddLine({zero.x, origin.y}, {zero.x, origin.y + size.y}, color(120, 200, 120, 60));
    dl->AddLine({origin.x, zero.y}, {origin.x + size.x, zero.y}, color(220, 110, 110, 60));

    const auto screenRect = [&](const Rect& r) {
        return std::pair{worldToScreen(r.position(), origin), worldToScreen({r.right(), r.bottom()}, origin)};
    };

    // Kill plane.
    if (scene.settings().killPlaneY) {
        const float y = worldToScreen({0, *scene.settings().killPlaneY}, origin).y;
        dl->AddLine({origin.x, y}, {origin.x + size.x, y}, color(255, 80, 80, 140), 1.5f);
        dl->AddText({origin.x + 6.0f, y - ImGui::GetFontSize() - 2.0f}, color(255, 120, 120, 200), "kill plane");
    }

    // What each camera will show at the start (virtual resolution / zoom).
    if (const auto* project = app.project()) {
        const Vec2i res = project->manifest().settings.display.virtualResolution;
        for (const Entity e : scene.entities()) {
            const auto* cam = scene.tryGet<Camera>(e);
            if (cam == nullptr || scene.tryGet<Transform>(e) == nullptr) continue;
            Vec2 center = scene.worldPosition(e);
            if (const Entity followed = scene.findById(cam->follow); !followed.isNull() && scene.tryGet<Transform>(followed)) {
                center = scene.worldPosition(followed);
            }
            const Rect view = Rect::fromCenter(center, {static_cast<float>(res.x) / cam->zoom, static_cast<float>(res.y) / cam->zoom});
            const auto [a, b] = screenRect(view);
            dl->AddRect(a, b, color(255, 255, 255, 90), 0.0f, 1.0f);
            dl->AddText({a.x + 4.0f, a.y + 2.0f}, color(255, 255, 255, 120), "camera view");
        }
    }

    // Colliders.
    if (s.showColliders) {
        for (const Entity e : scene.entities()) {
            const auto* c = scene.tryGet<BoxCollider>(e);
            if (c == nullptr || scene.tryGet<Transform>(e) == nullptr) continue;
            const auto [a, b] = screenRect(c->worldRect(scene.worldPosition(e)));
            dl->AddRect(a, b, c->isTrigger ? color(255, 220, 40, 200) : color(60, 230, 90, 200));
        }
    }

    // Hover and selection.
    const float handle = 10.0f * m_dpi / m_camera.zoom;
    const Vec2 mouseWorld = screenToWorld(ImGui::GetMousePos(), origin);
    if (m_hovered && app.tool == Tool::Select && m_drag == Drag::None) {
        const Entity hover = editor::pick(scene, mouseWorld, handle);
        if (!hover.isNull()) {
            const auto [a, b] = screenRect(editor::entityBounds(scene, hover, handle));
            dl->AddRect(a, b, color(255, 255, 255, 110));
        }
    }
    for (const Entity e : app.selection()) {
        const Rect bounds = editor::entityBounds(scene, e, handle);
        if (bounds.empty()) continue;
        const auto [a, b] = screenRect(bounds);
        dl->AddRect({a.x - 1, a.y - 1}, {b.x + 1, b.y + 1}, color(80, 170, 255, 255), 0.0f, 2.0f);
        const ImVec2 p = worldToScreen(scene.worldPosition(e), origin);
        dl->AddLine({p.x - 5, p.y}, {p.x + 5, p.y}, color(80, 170, 255, 255));
        dl->AddLine({p.x, p.y - 5}, {p.x, p.y + 5}, color(80, 170, 255, 255));
    }

    // Tile cursor.
    if ((app.tool == Tool::Paint || app.tool == Tool::Erase) && !app.playing()) {
        const Entity map = selectedTilemap(app);
        if (map.isNull()) {
            dl->AddText({origin.x + 10.0f, origin.y + 10.0f}, color(255, 200, 120),
                        "Select a tilemap (Hierarchy) to paint tiles. Its palette is in the Inspector.");
        } else if (m_hovered) {
            const editor::TileCoord cell = editor::tileAt(scene, map, mouseWorld);
            if (cell.inside) {
                const auto* tm = scene.tryGet<Tilemap>(map);
                const Vec2 o = scene.worldPosition(map);
                const Rect r{o.x + static_cast<float>(cell.x) * tm->tileSize.x, o.y + static_cast<float>(cell.y) * tm->tileSize.y,
                             tm->tileSize.x, tm->tileSize.y};
                const auto [a, b] = screenRect(r);
                dl->AddRect(a, b, app.tool == Tool::Erase ? color(255, 90, 90) : color(255, 220, 60), 0.0f, 2.0f);
            }
            const auto* tm = scene.tryGet<Tilemap>(map);
            const auto [a, b] = screenRect(tm->worldBounds(scene.worldPosition(map)));
            dl->AddRect(a, b, color(255, 220, 60, 80));
        }
    }

    // Marquee.
    if (m_drag == Drag::Marquee) {
        const ImVec2 m = ImGui::GetMousePos();
        dl->AddRectFilled(m_dragStartScreen, m, color(80, 170, 255, 40));
        dl->AddRect(m_dragStartScreen, m, color(80, 170, 255, 180));
    }

    // Mouse position.
    if (m_hovered) {
        const std::string text = std::format("{:.0f}, {:.0f}", mouseWorld.x, mouseWorld.y);
        dl->AddText({origin.x + size.x - ImGui::CalcTextSize(text.c_str()).x - 8.0f, origin.y + size.y - ImGui::GetFontSize() - 6.0f},
                    color(200, 205, 215, 200), text.c_str());
    }
    if (app.playing()) {
        dl->AddRect(origin, {origin.x + size.x, origin.y + size.y}, color(255, 150, 50, 200), 0.0f, 2.0f);
    }
}

} // namespace px::studio
