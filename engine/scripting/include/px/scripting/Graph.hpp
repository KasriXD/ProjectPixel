#pragma once

#include "px/core/Math.hpp"
#include "px/core/Result.hpp"
#include "px/scene/Components.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Visual scripts ("*.graph.json"): behaviours made of connected nodes,
// compiled to Luau and run exactly like handwritten scripts (same VM,
// sandbox, limits and API). The node palette is generated from the
// scripting API reference. See docs/visual-scripting.md.
namespace px::scripting {

inline constexpr int kGraphFormatVersion = 1;
inline constexpr std::size_t kMaxGraphNodes = 2000;
inline constexpr std::size_t kMaxGraphLinks = 8000;

// --- Node types -------------------------------------------------------------------------------

enum class PinType { Exec, Number, Boolean, String, Vector, Entity, EntityList, Any };
[[nodiscard]] const char* pinTypeName(PinType type) noexcept; // "exec", "number", ...
// Whether an output of type `from` may feed an input of type `to`.
[[nodiscard]] bool pinTypesConnect(PinType from, PinType to) noexcept;

struct PinInfo {
    std::string name;      // identifier used in files ("amount", "then")
    PinType type = PinType::Any;
    bool optional = false; // data inputs that may stay empty (nil)
    bool literalOnly = false; // must be a value typed into the node, not a connection
};

enum class NodeKind {
    Event,  // starts a chain: a lifecycle event, a custom event, a timer
    Action, // runs in a chain: exec in, exec out(s)
    Data,   // computes values when they are needed; no exec pins
};

struct NodeTypeInfo {
    std::string id;          // "call.Game.addScore", "event.Tick", "flow.branch", ...
    std::string title;       // "Add Score"
    std::string category;    // palette group: "Events", "Game", "World", "Flow", "Math", ...
    std::string description;
    NodeKind kind = NodeKind::Action;
    std::vector<PinInfo> inputs;
    std::vector<PinInfo> outputs;
    bool authorityOnly = false; // uses API that only authority scripts have
    bool needsProperty = false; // property.get / property.set: names one of the graph's properties

    [[nodiscard]] const PinInfo* input(std::string_view name) const;
    [[nodiscard]] const PinInfo* output(std::string_view name) const;
};

// Every node type, API nodes generated from apiReference() first, then the
// built-in flow, math and logic nodes.
[[nodiscard]] const std::vector<NodeTypeInfo>& nodeCatalog();
[[nodiscard]] const NodeTypeInfo* findNodeType(std::string_view id);

// --- Graphs -------------------------------------------------------------------------------------

using GraphValue = scene::ScriptValue; // number, boolean, string or vector

struct GraphProperty {
    std::string name;  // a Luau identifier; exposed in the inspector like a script property
    GraphValue value;  // default
};

struct GraphNode {
    std::string id;          // unique in the graph, stable (files, diffs, error messages)
    std::string type;        // a NodeTypeInfo id
    Vec2 position{};         // where the editor shows it
    std::string property;    // property.get / property.set: which property
    std::map<std::string, GraphValue> inputs; // values of inputs that are not connected
};

struct GraphLink {
    std::string fromNode;
    std::string fromPin;
    std::string toNode;
    std::string toPin;
    bool operator==(const GraphLink&) const = default;
};

struct Graph {
    std::vector<GraphProperty> properties;
    std::vector<GraphNode> nodes;
    std::vector<GraphLink> links;

    [[nodiscard]] const GraphNode* findNode(std::string_view id) const;
    [[nodiscard]] GraphNode* findNode(std::string_view id);
    // A node id not used yet ("n1", "n2", ...).
    [[nodiscard]] std::string newNodeId() const;
};

[[nodiscard]] Result<Graph> parseGraph(std::string_view json, std::string_view sourceName);
[[nodiscard]] std::string serializeGraph(const Graph& graph);

// --- Compiling ----------------------------------------------------------------------------

struct GraphProblem {
    std::string node; // node id ("" for the whole graph)
    std::string message;
};

struct CompiledGraph {
    std::string source;                  // Luau
    std::vector<std::string> lineNodes;  // per source line (index = line - 1): node id, or ""
    std::vector<GraphProblem> problems;  // not empty: the graph cannot run
    [[nodiscard]] bool ok() const { return problems.empty(); }
};

// Compiles a graph to a Luau behaviour. `name` appears in the header comment.
[[nodiscard]] CompiledGraph compileGraph(const Graph& graph, std::string_view name);

// "node 'Add Score' (n4)": how messages name a node.
[[nodiscard]] std::string describeNode(const Graph& graph, std::string_view nodeId);

} // namespace px::scripting
