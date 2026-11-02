#include "Panels.hpp"
#include "StudioApp.hpp"

#include "px/scripting/Graph.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

// The visual script editor: nodes on a canvas, connected by dragging from
// pin to pin. Graphs compile to Luau as they are edited; problems are shown
// on the nodes. Saves as it goes, like the other asset editors.
namespace px::studio {

using scripting::GraphNode;
using scripting::NodeKind;
using scripting::NodeTypeInfo;
using scripting::PinInfo;
using scripting::PinType;

namespace {

constexpr const char* kWindowId = "###graph-editor"; // GraphEditorPanel::windowId()
constexpr float kRow = 22.0f;    // pin row height (graph units)
constexpr float kHeader = 24.0f; // title bar height
constexpr float kPinRadius = 5.5f;

ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

ImU32 pinColor(PinType t) {
    switch (t) {
        case PinType::Exec: return rgba(235, 235, 240);
        case PinType::Number: return rgba(120, 200, 120);
        case PinType::Boolean: return rgba(230, 90, 90);
        case PinType::String: return rgba(240, 120, 220);
        case PinType::Vector: return rgba(240, 200, 70);
        case PinType::Entity: return rgba(90, 170, 255);
        case PinType::EntityList: return rgba(60, 120, 220);
        case PinType::Any: return rgba(170, 170, 180);
    }
    return rgba(170, 170, 180);
}

ImU32 headerColor(const NodeTypeInfo& t) {
    if (t.kind == NodeKind::Event) return rgba(150, 50, 50);
    if (t.category == "Flow") return rgba(80, 80, 92);
    if (t.kind == NodeKind::Data) return rgba(45, 105, 70);
    return rgba(45, 80, 140);
}

std::string valueText(const scripting::GraphValue& v) {
    return std::visit(
        [](const auto& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, double>) return std::format("{}", x);
            else if constexpr (std::is_same_v<T, bool>) return x ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::string>) return "\"" + x + "\"";
            else return std::format("[{}, {}]", x.x, x.y);
        },
        v);
}

float textWidth(const std::string& s) { return ImGui::CalcTextSize(s.c_str()).x; }

} // namespace

struct GraphEditorPanel::Layout {
    const NodeTypeInfo* type = nullptr;
    ImVec2 min;  // graph space
    ImVec2 size;
    std::size_t rows = 0;
};

void GraphEditorPanel::open(StudioApp& app, assets::AssetId graph) {
    auto* project = app.project();
    if (project == nullptr) return;
    auto loaded = project->loadGraph(graph);
    if (!loaded) {
        app.notify(loaded.error().message, true);
        return;
    }
    m_id = graph;
    m_graph = std::move(loaded.value());
    m_selected.clear();
    m_undo.clear();
    m_redo.clear();
    m_open = true;
    m_focus = true;
    m_pendingSave = false;
    m_recompile = true;
    m_pan = {40.0f, 40.0f};
    m_zoom = 1.0f;
    m_drag = Drag::None;
}

void GraphEditorPanel::close() {
    m_open = false;
    m_id = {};
}

void GraphEditorPanel::beginChange() {
    m_undo.push_back(m_graph);
    if (m_undo.size() > 200) m_undo.erase(m_undo.begin());
    m_redo.clear();
}

void GraphEditorPanel::changed() {
    m_pendingSave = true;
    m_recompile = true;
}

void GraphEditorPanel::draw(StudioApp& app) {
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
    if (m_recompile) {
        m_compiled = scripting::compileGraph(m_graph, entry->path);
        m_recompile = false;
    }
    bool open = true;
    const std::string title = std::format("{} (visual script){}", entry->name, kWindowId);
    if (ImGui::Begin(title.c_str(), &open)) drawContents(app);
    ImGui::End();
    if (m_pendingSave && !ImGui::IsAnyItemActive() && m_drag == Drag::None) {
        m_pendingSave = false;
        if (auto saved = project->saveGraph(m_id, m_graph); !saved) app.notify(saved.error().message, true);
    }
    if (!open) close();
}

GraphEditorPanel::Layout GraphEditorPanel::layout(const GraphNode& n) const {
    Layout l;
    l.type = scripting::findNodeType(n.type);
    l.min = {n.position.x, n.position.y};
    if (l.type == nullptr) {
        l.size = {160.0f, kHeader + kRow};
        return l;
    }
    std::string title = l.type->title;
    if (!n.property.empty()) title += ": " + n.property;
    float left = 0.0f;
    float right = 0.0f;
    for (const PinInfo& p : l.type->inputs) {
        std::string label = p.type == PinType::Exec ? std::string() : p.name;
        if (const auto it = n.inputs.find(p.name); it != n.inputs.end()) label += " = " + valueText(it->second);
        left = std::max(left, textWidth(label));
    }
    for (const PinInfo& p : l.type->outputs) right = std::max(right, textWidth(p.type == PinType::Exec ? "" : p.name));
    l.rows = std::max(l.type->inputs.size(), l.type->outputs.size());
    const float width = std::max({150.0f, textWidth(title) + 24.0f, left + right + 48.0f});
    l.size = {std::min(width, 440.0f), kHeader + static_cast<float>(std::max<std::size_t>(l.rows, 1)) * kRow + 6.0f};
    return l;
}

ImVec2 GraphEditorPanel::toScreen(ImVec2 graph) const {
    return {m_origin.x + (graph.x + m_pan.x) * m_zoom, m_origin.y + (graph.y + m_pan.y) * m_zoom};
}

ImVec2 GraphEditorPanel::toGraph(ImVec2 screen) const {
    return {(screen.x - m_origin.x) / m_zoom - m_pan.x, (screen.y - m_origin.y) / m_zoom - m_pan.y};
}

ImVec2 GraphEditorPanel::pinPosition(const GraphNode& n, const std::string& pin, bool output) const {
    const Layout l = layout(n);
    if (l.type == nullptr) return toScreen(l.min);
    const auto& pins = output ? l.type->outputs : l.type->inputs;
    for (std::size_t i = 0; i < pins.size(); ++i) {
        if (pins[i].name == pin) {
            const ImVec2 g{output ? l.min.x + l.size.x : l.min.x, l.min.y + kHeader + (static_cast<float>(i) + 0.5f) * kRow};
            return toScreen(g);
        }
    }
    return toScreen(l.min);
}

void GraphEditorPanel::drawContents(StudioApp& app) {
    const float em = ImGui::GetFontSize();
    // Toolbar.
    if (ImGui::Button("+ Add Node")) {
        m_addAt = toGraph({m_origin.x + 200.0f, m_origin.y + 120.0f});
        m_addFrom = {};
        m_openAdd = true;
    }
    ui::tooltip("Or right-click the canvas. Drag from a pin to an empty spot to add a node already connected.");
    ImGui::SameLine();
    ImGui::BeginDisabled(m_undo.empty());
    if (ImGui::Button("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_redo.empty());
    if (ImGui::Button("Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Show Luau", &m_showCode);
    ui::tooltip("The Luau code this graph runs as (read only).");
    ImGui::SameLine();
    if (m_compiled.ok()) {
        ImGui::TextColored({0.5f, 0.85f, 0.5f, 1.0f}, "Compiles");
    } else {
        ImGui::TextColored(ui::kErrorColor, "%zu problem%s", m_compiled.problems.size(),
                           m_compiled.problems.size() == 1 ? "" : "s");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Wheel: zoom   Middle/right drag: pan   Del: delete   Alt+click pin: disconnect");

    const float sideWidth = std::max(em * 15.0f, ImGui::GetContentRegionAvail().x * 0.24f);
    const float problemsHeight = m_compiled.ok() ? 0.0f : std::min(em * 5.0f, em * 1.4f * static_cast<float>(m_compiled.problems.size()) + em);
    ImGui::BeginChild("canvas", {ImGui::GetContentRegionAvail().x - sideWidth - em * 0.5f, -problemsHeight},
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (m_showCode) {
        std::string code = m_compiled.source;
        ImGui::PushFont(app.monoFont(), 0.0f);
        ImGui::InputTextMultiline("##code", &code, {-FLT_MIN, -FLT_MIN}, ImGuiInputTextFlags_ReadOnly);
        ImGui::PopFont();
    } else {
        drawCanvas(app);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("side", {0, -problemsHeight}, ImGuiChildFlags_None);
    drawSidePanel(app);
    ImGui::EndChild();
    if (!m_compiled.ok()) {
        ImGui::BeginChild("problems", {0, 0}, ImGuiChildFlags_None);
        for (const auto& p : m_compiled.problems) {
            ImGui::PushID(&p);
            if (ImGui::Selectable(p.message.c_str()) && !p.node.empty()) m_selected = {p.node};
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    drawAddMenu(app);
}

void GraphEditorPanel::drawCanvas(StudioApp& app) {
    (void)app;
    ImGuiIO& io = ImGui::GetIO();
    m_origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##canvas", {std::max(size.x, 1.0f), std::max(size.y, 1.0f)},
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end{m_origin.x + size.x, m_origin.y + size.y};
    dl->PushClipRect(m_origin, end, true);
    dl->AddRectFilled(m_origin, end, rgba(30, 32, 38));
    // Grid.
    const float step = 32.0f * m_zoom;
    const float ox = std::fmod(m_pan.x * m_zoom, step);
    const float oy = std::fmod(m_pan.y * m_zoom, step);
    for (float x = m_origin.x + ox; x < end.x; x += step) dl->AddLine({x, m_origin.y}, {x, end.y}, rgba(255, 255, 255, 12));
    for (float y = m_origin.y + oy; y < end.y; y += step) dl->AddLine({m_origin.x, y}, {end.x, y}, rgba(255, 255, 255, 12));

    // Zoom around the mouse.
    if (hovered && io.MouseWheel != 0.0f) {
        const ImVec2 before = toGraph(io.MousePos);
        m_zoom = std::clamp(m_zoom * (io.MouseWheel > 0 ? 1.15f : 1.0f / 1.15f), 0.35f, 2.0f);
        const ImVec2 after = toGraph(io.MousePos);
        m_pan.x += after.x - before.x;
        m_pan.y += after.y - before.y;
    }

    std::set<std::string> problemNodes;
    for (const auto& p : m_compiled.problems) problemNodes.insert(p.node);

    // Links.
    const float fontSize = ImGui::GetFontSize() * m_zoom;
    ImFont* font = ImGui::GetFont();
    for (const scripting::GraphLink& l : m_graph.links) {
        const GraphNode* from = m_graph.findNode(l.fromNode);
        const GraphNode* to = m_graph.findNode(l.toNode);
        if (from == nullptr || to == nullptr) continue;
        const ImVec2 a = pinPosition(*from, l.fromPin, true);
        const ImVec2 b = pinPosition(*to, l.toPin, false);
        const auto* t = scripting::findNodeType(from->type);
        const PinInfo* pin = t != nullptr ? t->output(l.fromPin) : nullptr;
        const ImU32 color = pin != nullptr ? pinColor(pin->type) : rgba(200, 200, 200);
        const float dx = std::max(40.0f * m_zoom, std::abs(b.x - a.x) * 0.5f);
        dl->AddBezierCubic(a, {a.x + dx, a.y}, {b.x - dx, b.y}, b, color, (pin != nullptr && pin->type == PinType::Exec ? 3.0f : 2.0f) * m_zoom);
    }

    // Nodes (and which pin or node is under the mouse).
    struct PinHit {
        std::string node;
        std::string pin;
        bool output = false;
    };
    std::optional<PinHit> hoveredPin;
    std::string hoveredNode;
    for (const GraphNode& n : m_graph.nodes) {
        const Layout l = layout(n);
        const ImVec2 a = toScreen(l.min);
        const ImVec2 b = toScreen({l.min.x + l.size.x, l.min.y + l.size.y});
        if (b.x < m_origin.x || b.y < m_origin.y || a.x > end.x || a.y > end.y) continue;
        const bool selected = std::find(m_selected.begin(), m_selected.end(), n.id) != m_selected.end();
        const float round = 6.0f * m_zoom;
        dl->AddRectFilled(a, b, rgba(48, 50, 58, 240), round);
        dl->AddRectFilled(a, {b.x, a.y + kHeader * m_zoom}, l.type != nullptr ? headerColor(*l.type) : rgba(140, 40, 40), round,
                          ImDrawFlags_RoundCornersTop);
        const bool broken = problemNodes.contains(n.id);
        dl->AddRect(a, b, broken ? rgba(255, 90, 90) : selected ? rgba(255, 210, 90) : rgba(20, 20, 24), round,
                    (broken || selected ? 2.5f : 1.0f) * m_zoom);
        std::string title = l.type != nullptr ? l.type->title : n.type + " (unknown)";
        if (!n.property.empty()) title += ": " + n.property;
        dl->AddText(font, fontSize, {a.x + 8.0f * m_zoom, a.y + (kHeader * m_zoom - fontSize) * 0.5f}, rgba(240, 240, 245),
                    title.c_str());
        if (ImGui::IsMouseHoveringRect(a, b) && hovered) hoveredNode = n.id;
        if (l.type == nullptr) continue;
        const auto drawPins = [&](const std::vector<PinInfo>& pins, bool output) {
            for (std::size_t i = 0; i < pins.size(); ++i) {
                const PinInfo& p = pins[i];
                const ImVec2 c = pinPosition(n, p.name, output);
                const ImU32 color = pinColor(p.type);
                const bool connected = std::any_of(m_graph.links.begin(), m_graph.links.end(), [&](const scripting::GraphLink& k) {
                    return output ? (k.fromNode == n.id && k.fromPin == p.name) : (k.toNode == n.id && k.toPin == p.name);
                });
                const float r = kPinRadius * m_zoom;
                if (p.type == PinType::Exec) {
                    const ImVec2 t0{c.x - r, c.y - r};
                    const ImVec2 t1{c.x - r, c.y + r};
                    const ImVec2 t2{c.x + r, c.y};
                    if (connected) {
                        dl->AddTriangleFilled(t0, t1, t2, color);
                    } else {
                        dl->AddTriangle(t0, t1, t2, color, 1.5f * m_zoom);
                    }
                } else if (connected) {
                    dl->AddCircleFilled(c, r, color);
                } else {
                    dl->AddCircle(c, r, color, 0, 1.5f * m_zoom);
                }
                std::string label = p.type == PinType::Exec ? (p.name == "in" || p.name == "then" ? "" : p.name) : p.name;
                if (!output && !connected) {
                    if (const auto it = n.inputs.find(p.name); it != n.inputs.end()) label += " = " + valueText(it->second);
                }
                if (!label.empty()) {
                    const ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label.c_str());
                    const float x = output ? c.x - r * 1.8f - ts.x : c.x + r * 1.8f;
                    dl->AddText(font, fontSize, {x, c.y - ts.y * 0.5f}, p.optional ? rgba(170, 172, 180) : rgba(220, 222, 228),
                                label.c_str());
                }
                const float dx = io.MousePos.x - c.x;
                const float dy = io.MousePos.y - c.y;
                if (hovered && dx * dx + dy * dy <= (r * 2.2f) * (r * 2.2f)) {
                    hoveredPin = PinHit{n.id, p.name, output};
                    ImGui::SetTooltip("%s (%s)%s", p.name.c_str(), scripting::pinTypeName(p.type),
                                      p.optional ? ", optional" : "");
                }
            }
        };
        drawPins(l.type->inputs, false);
        drawPins(l.type->outputs, true);
    }
    if (!hoveredNode.empty() && !hoveredPin && m_drag == Drag::None) {
        if (const GraphNode* n = m_graph.findNode(hoveredNode)) {
            if (const auto* t = scripting::findNodeType(n->type); t != nullptr && !t->description.empty() &&
                                                                  ImGui::IsKeyDown(ImGuiKey_LeftShift)) {
                ImGui::SetTooltip("%s", t->description.c_str());
            }
        }
    }

    // --- Mouse ---
    const ImVec2 mouseGraph = toGraph(io.MousePos);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hoveredPin && io.KeyAlt) {
            beginChange();
            std::erase_if(m_graph.links, [&](const scripting::GraphLink& k) {
                return hoveredPin->output ? (k.fromNode == hoveredPin->node && k.fromPin == hoveredPin->pin)
                                          : (k.toNode == hoveredPin->node && k.toPin == hoveredPin->pin);
            });
            changed();
        } else if (hoveredPin) {
            m_drag = Drag::Link;
            m_linkNode = hoveredPin->node;
            m_linkPin = hoveredPin->pin;
            m_linkFromOutput = hoveredPin->output;
        } else if (!hoveredNode.empty()) {
            const bool isSelected = std::find(m_selected.begin(), m_selected.end(), hoveredNode) != m_selected.end();
            if (io.KeyCtrl) {
                if (isSelected) {
                    std::erase(m_selected, hoveredNode);
                } else {
                    m_selected.push_back(hoveredNode);
                }
            } else if (!isSelected) {
                m_selected = {hoveredNode};
            }
            m_drag = Drag::Nodes;
            m_dragStart = mouseGraph;
            m_dragMoved = false;
        } else {
            m_drag = Drag::Box;
            m_dragStart = mouseGraph;
            if (!io.KeyCtrl) m_selected.clear();
        }
    }
    if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        m_drag = Drag::Pan;
        m_panMoved = 0.0f;
    }
    switch (m_drag) {
        case Drag::Pan:
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                m_pan.x += io.MouseDelta.x / m_zoom;
                m_pan.y += io.MouseDelta.y / m_zoom;
                m_panMoved += std::abs(io.MouseDelta.x) + std::abs(io.MouseDelta.y);
            } else {
                if (m_panMoved < 4.0f && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                    // A right click without dragging: the add-node menu here.
                    m_addAt = mouseGraph;
                    m_addFrom = {};
                    m_openAdd = true;
                }
                m_drag = Drag::None;
            }
            break;
        case Drag::Nodes:
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const ImVec2 delta{mouseGraph.x - m_dragStart.x, mouseGraph.y - m_dragStart.y};
                if (delta.x != 0.0f || delta.y != 0.0f) {
                    if (!m_dragMoved) beginChange();
                    m_dragMoved = true;
                    for (const std::string& id : m_selected) {
                        if (GraphNode* n = m_graph.findNode(id)) {
                            n->position.x += delta.x;
                            n->position.y += delta.y;
                        }
                    }
                    m_dragStart = mouseGraph;
                }
            } else {
                if (m_dragMoved) {
                    // Rest on whole units so files stay tidy.
                    for (const std::string& id : m_selected) {
                        if (GraphNode* n = m_graph.findNode(id)) n->position = {std::round(n->position.x), std::round(n->position.y)};
                    }
                    changed();
                }
                m_drag = Drag::None;
            }
            break;
        case Drag::Box: {
            const ImVec2 a = toScreen(m_dragStart);
            dl->AddRectFilled(a, io.MousePos, rgba(90, 170, 255, 30));
            dl->AddRect(a, io.MousePos, rgba(90, 170, 255, 160));
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const ImVec2 lo{std::min(m_dragStart.x, mouseGraph.x), std::min(m_dragStart.y, mouseGraph.y)};
                const ImVec2 hi{std::max(m_dragStart.x, mouseGraph.x), std::max(m_dragStart.y, mouseGraph.y)};
                for (const GraphNode& n : m_graph.nodes) {
                    const Layout l = layout(n);
                    if (l.min.x < hi.x && l.min.x + l.size.x > lo.x && l.min.y < hi.y && l.min.y + l.size.y > lo.y &&
                        std::find(m_selected.begin(), m_selected.end(), n.id) == m_selected.end()) {
                        m_selected.push_back(n.id);
                    }
                }
                m_drag = Drag::None;
            }
            break;
        }
        case Drag::Link: {
            const GraphNode* from = m_graph.findNode(m_linkNode);
            if (from == nullptr) {
                m_drag = Drag::None;
                break;
            }
            const ImVec2 a = pinPosition(*from, m_linkPin, m_linkFromOutput);
            const ImVec2 b = io.MousePos;
            const ImVec2 out = m_linkFromOutput ? a : b;
            const ImVec2 in = m_linkFromOutput ? b : a;
            const float dx = std::max(40.0f * m_zoom, std::abs(in.x - out.x) * 0.5f);
            dl->AddBezierCubic(out, {out.x + dx, out.y}, {in.x - dx, in.y}, in, rgba(255, 220, 120), 2.5f * m_zoom);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (hoveredPin && hoveredPin->output != m_linkFromOutput) {
                    if (m_linkFromOutput) {
                        connect(app, m_linkNode, m_linkPin, hoveredPin->node, hoveredPin->pin);
                    } else {
                        connect(app, hoveredPin->node, hoveredPin->pin, m_linkNode, m_linkPin);
                    }
                } else if (!hoveredPin && hovered) {
                    // Dropped on empty space: add a node that fits, connected.
                    m_addAt = mouseGraph;
                    m_addFrom = {m_linkNode, m_linkPin, m_linkFromOutput};
                    m_openAdd = true;
                }
                m_drag = Drag::None;
            }
            break;
        }
        case Drag::None: break;
    }
    dl->PopClipRect();

    // Keyboard.
    if (focused && !io.WantTextInput) {
        if ((ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) && !m_selected.empty()) {
            deleteSelected();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) undo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) redo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) duplicateSelected();
    }
}

void GraphEditorPanel::connect(StudioApp& app, const std::string& fromNode, const std::string& fromPin,
                               const std::string& toNode, const std::string& toPin) {
    const GraphNode* a = m_graph.findNode(fromNode);
    const GraphNode* b = m_graph.findNode(toNode);
    const auto* ta = a != nullptr ? scripting::findNodeType(a->type) : nullptr;
    const auto* tb = b != nullptr ? scripting::findNodeType(b->type) : nullptr;
    const PinInfo* out = ta != nullptr ? ta->output(fromPin) : nullptr;
    const PinInfo* in = tb != nullptr ? tb->input(toPin) : nullptr;
    if (out == nullptr || in == nullptr || fromNode == toNode) return;
    if (!scripting::pinTypesConnect(out->type, in->type)) {
        app.notify(std::format("A {} output cannot go into a {} input", scripting::pinTypeName(out->type),
                               scripting::pinTypeName(in->type)),
                   true);
        return;
    }
    if (in->literalOnly) {
        app.notify(std::format("'{}' is typed into the node; it cannot be connected", in->name), true);
        return;
    }
    beginChange();
    // A data input takes one value; an exec output continues one chain (use Sequence for more).
    std::erase_if(m_graph.links, [&](const scripting::GraphLink& k) {
        return (in->type != PinType::Exec && k.toNode == toNode && k.toPin == toPin) ||
               (out->type == PinType::Exec && k.fromNode == fromNode && k.fromPin == fromPin);
    });
    m_graph.links.push_back({fromNode, fromPin, toNode, toPin});
    changed();
}

void GraphEditorPanel::deleteSelected() {
    if (m_selected.empty()) return;
    beginChange();
    std::erase_if(m_graph.nodes, [&](const GraphNode& n) {
        return std::find(m_selected.begin(), m_selected.end(), n.id) != m_selected.end();
    });
    std::erase_if(m_graph.links, [&](const scripting::GraphLink& k) {
        return m_graph.findNode(k.fromNode) == nullptr || m_graph.findNode(k.toNode) == nullptr;
    });
    m_selected.clear();
    changed();
}

void GraphEditorPanel::duplicateSelected() {
    if (m_selected.empty()) return;
    beginChange();
    std::map<std::string, std::string> ids;
    std::vector<GraphNode> copies;
    for (const std::string& id : m_selected) {
        const GraphNode* n = m_graph.findNode(id);
        if (n == nullptr) continue;
        GraphNode copy = *n;
        copy.id = m_graph.newNodeId();
        copy.position.x += 30.0f;
        copy.position.y += 30.0f;
        ids[id] = copy.id;
        m_graph.nodes.push_back(copy);
    }
    // Links between the copied nodes are copied too.
    const auto links = m_graph.links;
    for (const scripting::GraphLink& k : links) {
        if (ids.contains(k.fromNode) && ids.contains(k.toNode)) {
            m_graph.links.push_back({ids[k.fromNode], k.fromPin, ids[k.toNode], k.toPin});
        }
    }
    m_selected.clear();
    for (const auto& [from, to] : ids) m_selected.push_back(to);
    changed();
}

void GraphEditorPanel::undo() {
    if (m_undo.empty()) return;
    m_redo.push_back(m_graph);
    m_graph = std::move(m_undo.back());
    m_undo.pop_back();
    changed();
}

void GraphEditorPanel::redo() {
    if (m_redo.empty()) return;
    m_undo.push_back(m_graph);
    m_graph = std::move(m_redo.back());
    m_redo.pop_back();
    changed();
}

void GraphEditorPanel::addNode(StudioApp& app, const NodeTypeInfo& type) {
    beginChange();
    GraphNode n;
    n.id = m_graph.newNodeId();
    n.type = type.id;
    n.position = {std::round(m_addAt.x), std::round(m_addAt.y)};
    if (type.id == "property.get" || type.id == "property.set") {
        if (m_graph.properties.empty()) m_graph.properties.push_back({"value", 0.0});
        n.property = m_graph.properties.front().name;
    }
    for (const PinInfo& p : type.inputs) {
        if (p.literalOnly) n.inputs[p.name] = p.type == PinType::Number ? scripting::GraphValue(1.0)
                                                                          : scripting::GraphValue(std::string("my_event"));
    }
    m_graph.nodes.push_back(n);
    m_selected = {n.id};
    changed();
    // Connect it to the pin the link was dragged from: the first pin that fits.
    if (m_addFrom) {
        const auto& [fromNode, fromPin, fromOutput] = *m_addFrom;
        const GraphNode* other = m_graph.findNode(fromNode);
        const auto* ot = other != nullptr ? scripting::findNodeType(other->type) : nullptr;
        const PinInfo* op = ot == nullptr ? nullptr : fromOutput ? ot->output(fromPin) : ot->input(fromPin);
        if (op != nullptr) {
            const auto& pins = fromOutput ? type.inputs : type.outputs;
            for (const PinInfo& p : pins) {
                if (p.literalOnly) continue;
                const bool fits = fromOutput ? scripting::pinTypesConnect(op->type, p.type)
                                             : scripting::pinTypesConnect(p.type, op->type);
                if (!fits) continue;
                // connect() records its own undo step; fold it into this one.
                const std::size_t undoBefore = m_undo.size();
                if (fromOutput) {
                    connect(app, fromNode, fromPin, n.id, p.name);
                } else {
                    connect(app, n.id, p.name, fromNode, fromPin);
                }
                if (m_undo.size() > undoBefore) m_undo.pop_back();
                break;
            }
        }
    }
}

void GraphEditorPanel::drawAddMenu(StudioApp& app) {
    if (m_openAdd) {
        ImGui::OpenPopup("AddNode");
        m_openAdd = false;
        m_search.clear();
        m_focusSearch = true;
    }
    ImGui::SetNextWindowSizeConstraints({ImGui::GetFontSize() * 16.0f, 0}, {ImGui::GetFontSize() * 24.0f, ImGui::GetFontSize() * 28.0f});
    if (!ImGui::BeginPopup("AddNode")) return;
    if (m_focusSearch) {
        ImGui::SetKeyboardFocusHere();
        m_focusSearch = false;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool enter = ImGui::InputTextWithHint("##search", "Search nodes", &m_search, ImGuiInputTextFlags_EnterReturnsTrue);
    const auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string query = lower(m_search);
    // When a link was dragged out, only nodes with a pin it fits.
    const PinInfo* fromPin = nullptr;
    if (m_addFrom) {
        const GraphNode* other = m_graph.findNode(std::get<0>(*m_addFrom));
        const auto* ot = other != nullptr ? scripting::findNodeType(other->type) : nullptr;
        if (ot != nullptr) fromPin = std::get<2>(*m_addFrom) ? ot->output(std::get<1>(*m_addFrom)) : ot->input(std::get<1>(*m_addFrom));
    }
    const auto fits = [&](const NodeTypeInfo& t) {
        if (fromPin == nullptr) return true;
        const auto& pins = std::get<2>(*m_addFrom) ? t.inputs : t.outputs;
        return std::any_of(pins.begin(), pins.end(), [&](const PinInfo& p) {
            return !p.literalOnly && (std::get<2>(*m_addFrom) ? scripting::pinTypesConnect(fromPin->type, p.type)
                                                              : scripting::pinTypesConnect(p.type, fromPin->type));
        });
    };
    const NodeTypeInfo* chosen = nullptr;
    const NodeTypeInfo* firstMatch = nullptr;
    ImGui::BeginChild("list", {0, ImGui::GetFontSize() * 22.0f}, ImGuiChildFlags_None);
    std::vector<std::string> categories;
    for (const NodeTypeInfo& t : scripting::nodeCatalog()) {
        if (std::find(categories.begin(), categories.end(), t.category) == categories.end()) categories.push_back(t.category);
    }
    for (const std::string& category : categories) {
        std::vector<const NodeTypeInfo*> items;
        for (const NodeTypeInfo& t : scripting::nodeCatalog()) {
            if (t.category != category || !fits(t)) continue;
            // Titles (and type ids, for people who know them), not descriptions: those match too much.
            if (!query.empty() && lower(t.title).find(query) == std::string::npos &&
                lower(t.id).find(query) == std::string::npos) {
                continue;
            }
            items.push_back(&t);
        }
        if (items.empty()) continue;
        if (firstMatch == nullptr) firstMatch = items.front();
        const ImGuiTreeNodeFlags flags = query.empty() && fromPin == nullptr ? 0 : ImGuiTreeNodeFlags_DefaultOpen;
        ImGui::SetNextItemOpen(!query.empty() || fromPin != nullptr, query.empty() && fromPin == nullptr ? ImGuiCond_Appearing : ImGuiCond_Always);
        if (ImGui::TreeNodeEx(category.c_str(), flags)) {
            for (const NodeTypeInfo* t : items) {
                if (ImGui::Selectable(t->title.c_str())) chosen = t;
                if (ImGui::IsItemHovered() && !t->description.empty()) {
                    ImGui::SetTooltip("%s%s", t->description.c_str(), t->authorityOnly ? "\n(server/authority only)" : "");
                }
            }
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();
    if (enter && firstMatch != nullptr) chosen = firstMatch;
    if (chosen != nullptr) {
        addNode(app, *chosen);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void GraphEditorPanel::drawSidePanel(StudioApp& app) {
    (void)app;
    const float em = ImGui::GetFontSize();
    // The selected node's typed-in values.
    GraphNode* node = m_selected.size() == 1 ? m_graph.findNode(m_selected.front()) : nullptr;
    const NodeTypeInfo* type = node != nullptr ? scripting::findNodeType(node->type) : nullptr;
    if (type != nullptr) {
        ImGui::TextColored(ui::kAccentColor, "%s", type->title.c_str());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", type->description.c_str());
        ImGui::PopTextWrapPos();
        if (node->type == "property.get" || node->type == "property.set") {
            if (ImGui::BeginCombo("Property", node->property.c_str())) {
                for (const auto& p : m_graph.properties) {
                    if (ImGui::Selectable(p.name.c_str(), p.name == node->property)) {
                        beginChange();
                        node->property = p.name;
                        changed();
                    }
                }
                ImGui::EndCombo();
            }
        }
        for (const PinInfo& p : type->inputs) {
            if (p.type == PinType::Exec || p.type == PinType::Entity || p.type == PinType::EntityList) continue;
            const bool connected = std::any_of(m_graph.links.begin(), m_graph.links.end(), [&](const scripting::GraphLink& k) {
                return k.toNode == node->id && k.toPin == p.name;
            });
            ImGui::PushID(p.name.c_str());
            if (connected) {
                ImGui::TextDisabled("%s: connected", p.name.c_str());
                ImGui::PopID();
                continue;
            }
            auto it = node->inputs.find(p.name);
            const bool has = it != node->inputs.end();
            PinType shown = p.type;
            if (shown == PinType::Any) {
                // An "any" input: choose what kind of value to type in.
                int kind = has ? static_cast<int>(it->second.index()) : -1;
                const char* kinds[] = {"number", "true/false", "text", "vector"};
                ImGui::SetNextItemWidth(em * 6.0f);
                if (ImGui::Combo("##kind", &kind, kinds, 4)) {
                    beginChange();
                    const scripting::GraphValue defaults[] = {0.0, false, std::string(), Vec2{}};
                    node->inputs[p.name] = defaults[kind];
                    changed();
                }
                ImGui::SameLine();
                it = node->inputs.find(p.name);
                if (it == node->inputs.end()) {
                    ImGui::TextDisabled("%s: nothing", p.name.c_str());
                    ImGui::PopID();
                    continue;
                }
                const PinType byIndex[] = {PinType::Number, PinType::Boolean, PinType::String, PinType::Vector};
                shown = byIndex[it->second.index()];
            }
            scripting::GraphValue value = has || it != node->inputs.end() ? node->inputs[p.name] : scripting::GraphValue{};
            bool edited = false;
            ImGui::SetNextItemWidth(-em * 5.0f);
            switch (shown) {
                case PinType::Number: {
                    double v = std::holds_alternative<double>(value) ? std::get<double>(value) : 0.0;
                    edited = ImGui::InputDouble(p.name.c_str(), &v, 0.0, 0.0, "%.4g");
                    value = v;
                    break;
                }
                case PinType::Boolean: {
                    bool v = std::holds_alternative<bool>(value) && std::get<bool>(value);
                    edited = ImGui::Checkbox(p.name.c_str(), &v);
                    value = v;
                    break;
                }
                case PinType::String: {
                    std::string v = std::holds_alternative<std::string>(value) ? std::get<std::string>(value) : std::string();
                    edited = ImGui::InputText(p.name.c_str(), &v);
                    value = v;
                    break;
                }
                case PinType::Vector: {
                    Vec2 v = std::holds_alternative<Vec2>(value) ? std::get<Vec2>(value) : Vec2{};
                    float xy[2] = {v.x, v.y};
                    edited = ImGui::InputFloat2(p.name.c_str(), xy, "%.3g");
                    value = Vec2{xy[0], xy[1]};
                    break;
                }
                default: break;
            }
            if (edited) {
                if (!ImGui::IsItemActive() || !m_editing) {
                    beginChange();
                    m_editing = true;
                }
                node->inputs[p.name] = value;
                changed();
            }
            if (ImGui::IsItemDeactivated()) m_editing = false;
            if (p.optional && node->inputs.contains(p.name)) {
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    beginChange();
                    node->inputs.erase(p.name);
                    changed();
                }
                ui::tooltip("Leave it empty");
            }
            ImGui::PopID();
        }
        ImGui::Separator();
    } else if (m_selected.size() > 1) {
        ImGui::Text("%zu nodes selected", m_selected.size());
        ImGui::TextDisabled("Del deletes them, Ctrl+D duplicates them.");
        ImGui::Separator();
    } else {
        ImGui::TextDisabled("Select a node to type in its values.");
        ImGui::Separator();
    }

    // The graph's properties (variables, editable per entity in Properties).
    ImGui::TextUnformatted("Properties");
    ui::tooltip("Values each entity can set in Properties, and variables the graph changes with Set Property.");
    for (std::size_t i = 0; i < m_graph.properties.size(); ++i) {
        auto& prop = m_graph.properties[i];
        ImGui::PushID(static_cast<int>(i));
        std::string name = prop.name;
        ImGui::SetNextItemWidth(em * 6.0f);
        if (ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty()) {
            beginChange();
            for (GraphNode& n : m_graph.nodes) {
                if (n.property == prop.name) n.property = name;
            }
            prop.name = name;
            changed();
        }
        ImGui::SameLine();
        int kind = static_cast<int>(prop.value.index());
        const char* kinds[] = {"number", "true/false", "text", "vector"};
        ImGui::SetNextItemWidth(em * 5.5f);
        if (ImGui::Combo("##kind", &kind, kinds, 4)) {
            beginChange();
            const scripting::GraphValue defaults[] = {0.0, false, std::string(), Vec2{}};
            prop.value = defaults[kind];
            changed();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            beginChange();
            m_graph.properties.erase(m_graph.properties.begin() + static_cast<std::ptrdiff_t>(i));
            changed();
            ImGui::PopID();
            break;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        bool edited = false;
        std::visit(
            [&](auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, double>) edited = ImGui::InputDouble("##v", &v, 0.0, 0.0, "%.4g");
                else if constexpr (std::is_same_v<T, bool>) edited = ImGui::Checkbox("default##v", &v);
                else if constexpr (std::is_same_v<T, std::string>) edited = ImGui::InputText("##v", &v);
                else {
                    float xy[2] = {v.x, v.y};
                    edited = ImGui::InputFloat2("##v", xy, "%.3g");
                    v = Vec2{xy[0], xy[1]};
                }
            },
            prop.value);
        if (edited) changed();
        ImGui::PopID();
    }
    if (ImGui::SmallButton("+ Property")) {
        beginChange();
        std::string name = "value";
        for (int k = 2; std::any_of(m_graph.properties.begin(), m_graph.properties.end(),
                                    [&](const scripting::GraphProperty& p) { return p.name == name; });
             ++k) {
            name = std::format("value{}", k);
        }
        m_graph.properties.push_back({name, 0.0});
        changed();
    }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Attach this visual script to entities like a script (Properties > Add Script). It runs as "
                        "Luau: 'Show Luau' shows the code.");
    ImGui::PopTextWrapPos();
}

} // namespace px::studio
