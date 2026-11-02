#include "px/scripting/Graph.hpp"

#include "px/scripting/ApiReference.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <functional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace px::scripting {

using Json = nlohmann::ordered_json;

// --- Node catalog ---------------------------------------------------------------------------

const char* pinTypeName(PinType type) noexcept {
    switch (type) {
        case PinType::Exec: return "exec";
        case PinType::Number: return "number";
        case PinType::Boolean: return "boolean";
        case PinType::String: return "string";
        case PinType::Vector: return "vector";
        case PinType::Entity: return "Entity";
        case PinType::EntityList: return "{Entity}";
        case PinType::Any: return "any";
    }
    return "any";
}

bool pinTypesConnect(PinType from, PinType to) noexcept {
    if (from == PinType::Exec || to == PinType::Exec) return from == to;
    return from == to || from == PinType::Any || to == PinType::Any;
}

const PinInfo* NodeTypeInfo::input(std::string_view name) const {
    for (const PinInfo& p : inputs) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

const PinInfo* NodeTypeInfo::output(std::string_view name) const {
    for (const PinInfo& p : outputs) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

namespace {

PinType pinTypeFrom(std::string_view t) {
    if (t == "number") return PinType::Number;
    if (t == "boolean") return PinType::Boolean;
    if (t == "string") return PinType::String;
    if (t == "vector") return PinType::Vector;
    if (t == "Entity") return PinType::Entity;
    if (t == "{Entity}") return PinType::EntityList;
    return PinType::Any;
}

// "addScore" -> "Add Score", "TriggerEnter" -> "Trigger Enter".
std::string titleCase(std::string_view name) {
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (i == 0) {
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            continue;
        }
        const bool upper = std::isupper(static_cast<unsigned char>(c)) != 0;
        const bool prevLower = std::islower(static_cast<unsigned char>(name[i - 1])) != 0;
        if (upper && prevLower) out += ' ';
        out += c;
    }
    return out;
}

PinInfo execIn() { return {"in", PinType::Exec}; }
PinInfo execOut(std::string name = "then") { return {std::move(name), PinType::Exec}; }

std::vector<NodeTypeInfo> buildCatalog() {
    std::vector<NodeTypeInfo> out;
    for (const ApiMember& m : apiReference()) {
        const ApiSignature sig = parseApiSignature(m);
        if (sig.takesFunction) continue; // Events.on: the "On Custom Event" node instead
        const bool authority = m.availability == ApiAvailability::AuthorityOnly;
        NodeTypeInfo t;
        t.description = std::string(m.description);
        t.authorityOnly = authority;
        const std::string group(m.group);
        const std::string name(m.name);
        switch (m.kind) {
            case ApiKind::Event:
                t.id = "event." + name;
                t.title = titleCase(name);
                t.category = "Events";
                t.kind = NodeKind::Event;
                t.outputs.push_back(execOut());
                for (const ApiParam& p : sig.params) t.outputs.push_back({p.name, pinTypeFrom(p.type), p.optional});
                break;
            case ApiKind::Function:
            case ApiKind::Global:
            case ApiKind::Method: {
                if (m.kind == ApiKind::Method) {
                    t.id = "method." + name;
                    t.category = "Entity";
                    t.title = titleCase(name);
                } else if (m.kind == ApiKind::Global) {
                    t.id = "call." + name;
                    t.category = "Utility";
                    t.title = name == "Vec2" ? "Make Vector" : titleCase(name);
                } else {
                    t.id = "call." + group + "." + name;
                    t.category = group;
                    t.title = std::format("{} {}", group, titleCase(name));
                }
                t.kind = m.pure ? NodeKind::Data : NodeKind::Action;
                if (t.kind == NodeKind::Action) t.inputs.push_back(execIn());
                if (m.kind == ApiKind::Method) t.inputs.push_back({"self", PinType::Entity, true});
                for (const ApiParam& p : sig.params) t.inputs.push_back({p.name, pinTypeFrom(p.type), p.optional});
                if (sig.variadic) t.inputs.push_back({"value", PinType::Any, true});
                if (t.kind == NodeKind::Action) t.outputs.push_back(execOut());
                if (sig.result) t.outputs.push_back({"result", pinTypeFrom(sig.result->type), sig.result->optional});
                break;
            }
            case ApiKind::Property: {
                const PinType type = sig.result ? pinTypeFrom(sig.result->type) : PinType::Any;
                NodeTypeInfo get;
                get.id = "get." + name;
                get.title = "Get " + titleCase(name);
                get.category = "Entity";
                get.description = t.description;
                get.kind = NodeKind::Data;
                get.inputs.push_back({"self", PinType::Entity, true});
                get.outputs.push_back({"value", type, sig.result && sig.result->optional});
                out.push_back(std::move(get));
                if (!m.writable) continue;
                t.id = "set." + name;
                t.title = "Set " + titleCase(name);
                t.category = "Entity";
                t.kind = NodeKind::Action;
                t.authorityOnly = true;
                t.inputs = {execIn(), {"self", PinType::Entity, true}, {"value", type}};
                t.outputs = {execOut()};
                break;
            }
        }
        out.push_back(std::move(t));
    }

    const auto add = [&out](std::string id, std::string title, std::string category, NodeKind kind,
                            std::vector<PinInfo> inputs, std::vector<PinInfo> outputs, std::string description) {
        NodeTypeInfo t;
        t.id = std::move(id);
        t.title = std::move(title);
        t.category = std::move(category);
        t.kind = kind;
        t.inputs = std::move(inputs);
        t.outputs = std::move(outputs);
        t.description = std::move(description);
        out.push_back(std::move(t));
        return &out.back();
    };
    using K = NodeKind;
    using P = PinType;
    add("event.custom", "On Custom Event", "Events", K::Event, {{"name", P::String, false, true}},
        {execOut(), {"value", P::Any, true}}, "Runs when any script emits the event with this name (Events.emit).");
    add("event.every", "Every N Seconds", "Events", K::Event, {{"seconds", P::Number, false, true}}, {execOut()},
        "Runs again and again, this many seconds apart (in simulated time).");
    add("flow.branch", "Branch", "Flow", K::Action, {execIn(), {"condition", P::Boolean}},
        {execOut("true"), execOut("false")}, "Continues on 'true' or on 'false' depending on the condition.");
    add("flow.sequence", "Sequence", "Flow", K::Action, {execIn()},
        {execOut("first"), execOut("second"), execOut("third")}, "Runs the first chain, then the second, then the third.");
    add("flow.forEach", "For Each Entity", "Flow", K::Action, {execIn(), {"list", P::EntityList}},
        {execOut("body"), {"item", P::Entity}, execOut("done")},
        "Runs 'body' once for every entity in the list (e.g. from World Find With Tag), then 'done'.");
    add("flow.repeat", "Repeat", "Flow", K::Action, {execIn(), {"count", P::Number}},
        {execOut("body"), {"index", P::Number}, execOut("done")}, "Runs 'body' this many times (index 1, 2, ...), then 'done'.");
    add("flow.delay", "Delay", "Flow", K::Action, {execIn(), {"seconds", P::Number}}, {execOut()},
        "Waits this many seconds (simulated time), then continues. Values are read when it continues.");
    add("data.self", "This Entity", "Utility", K::Data, {}, {{"entity", P::Entity}}, "The entity this behaviour is on.");
    add("property.get", "Get Property", "Properties", K::Data, {}, {{"value", P::Any}},
        "A property of this behaviour (set per entity in Properties, or by Set Property).");
    add("property.set", "Set Property", "Properties", K::Action, {execIn(), {"value", P::Any}}, {execOut()},
        "Changes a property of this behaviour (for this entity only). Use properties as variables.");
    for (const auto& [id, title, op] :
         {std::tuple{"math.add", "Add", "+"}, std::tuple{"math.subtract", "Subtract", "-"},
          std::tuple{"math.multiply", "Multiply", "*"}, std::tuple{"math.divide", "Divide", "/"}}) {
        add(id, title, "Math", K::Data, {{"a", P::Any}, {"b", P::Any}}, {{"result", P::Any}},
            std::format("a {} b, for numbers and vectors.", op));
    }
    add("math.random", "Random Number", "Math", K::Data, {{"min", P::Number}, {"max", P::Number}},
        {{"result", P::Number}}, "A random number from min to max (deterministic per session).");
    for (const auto& [id, title, desc] :
         {std::tuple{"compare.equal", "Equal", "True if a and b are the same value or entity."},
          std::tuple{"compare.notEqual", "Not Equal", "True if a and b differ."},
          std::tuple{"compare.less", "Less Than", "True if a < b."},
          std::tuple{"compare.greater", "Greater Than", "True if a > b."}}) {
        const bool numeric = std::string_view(id) == "compare.less" || std::string_view(id) == "compare.greater";
        add(id, title, "Logic", K::Data, {{"a", numeric ? P::Number : P::Any}, {"b", numeric ? P::Number : P::Any}},
            {{"result", P::Boolean}}, desc);
    }
    add("logic.and", "And", "Logic", K::Data, {{"a", P::Boolean}, {"b", P::Boolean}}, {{"result", P::Boolean}},
        "True if both are true.");
    add("logic.or", "Or", "Logic", K::Data, {{"a", P::Boolean}, {"b", P::Boolean}}, {{"result", P::Boolean}},
        "True if either is true.");
    add("logic.not", "Not", "Logic", K::Data, {{"value", P::Boolean}}, {{"result", P::Boolean}}, "The opposite.");
    add("vector.split", "Split Vector", "Math", K::Data, {{"vector", P::Vector}}, {{"x", P::Number}, {"y", P::Number}},
        "The x and y of a vector.");
    add("vector.length", "Vector Length", "Math", K::Data, {{"vector", P::Vector}}, {{"result", P::Number}},
        "How long a vector is (e.g. a distance: the length of a minus b).");
    add("text.join", "Join Text", "Utility", K::Data, {{"a", P::Any}, {"b", P::Any}}, {{"result", P::String}},
        "a and b written one after the other, as text.");
    return out;
}

bool isIdentifier(std::string_view s) {
    static const std::set<std::string_view> keywords{"and",   "break", "do",     "else", "elseif", "end",
                                                     "false", "for",   "function", "if", "in",     "local",
                                                     "nil",   "not",   "or",     "repeat", "return", "then",
                                                     "true",  "until", "while",  "continue", "export", "type"};
    if (s.empty() || s.size() > 64 || keywords.contains(s)) return false;
    if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

bool isNodeId(std::string_view s) {
    if (s.empty() || s.size() > 32) return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; });
}

// --- JSON -----------------------------------------------------------------------------------

[[noreturn]] void fail(const std::string& path, std::string_view message) {
    throw std::runtime_error(path.empty() ? std::string(message) : std::format("{}: {}", path, message));
}

GraphValue readValue(const Json& j, const std::string& path) {
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number()) {
        const double d = j.get<double>();
        if (!std::isfinite(d)) fail(path, "numbers must be finite");
        return d;
    }
    if (j.is_string()) {
        std::string s = j.get<std::string>();
        if (s.size() > 1000) fail(path, "text is limited to 1000 characters");
        return s;
    }
    if (j.is_array() && j.size() == 2 && j[0].is_number() && j[1].is_number()) {
        const double x = j[0].get<double>();
        const double y = j[1].get<double>();
        if (!std::isfinite(x) || !std::isfinite(y)) fail(path, "numbers must be finite");
        return Vec2{static_cast<float>(x), static_cast<float>(y)};
    }
    fail(path, "expected a number, true/false, text or [x, y]");
}

Json writeValue(const GraphValue& v) {
    return std::visit(
        [](const auto& x) -> Json {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, Vec2>) {
                return Json::array({x.x, x.y});
            } else {
                return x;
            }
        },
        v);
}

void checkKeys(const Json& j, const std::string& path, std::initializer_list<std::string_view> allowed) {
    if (!j.is_object()) fail(path, "expected an object");
    for (const auto& [key, value] : j.items()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            fail(path, std::format("unknown field '{}'", key));
        }
    }
}

std::pair<std::string, std::string> splitPin(const std::string& text, const std::string& path) {
    const std::size_t dot = text.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == text.size()) fail(path, "expected \"node.pin\"");
    return {text.substr(0, dot), text.substr(dot + 1)};
}

} // namespace

const std::vector<NodeTypeInfo>& nodeCatalog() {
    static const std::vector<NodeTypeInfo> catalog = buildCatalog();
    return catalog;
}

const NodeTypeInfo* findNodeType(std::string_view id) {
    static const std::unordered_map<std::string, const NodeTypeInfo*> index = [] {
        std::unordered_map<std::string, const NodeTypeInfo*> m;
        for (const NodeTypeInfo& t : nodeCatalog()) m.emplace(t.id, &t);
        return m;
    }();
    const auto it = index.find(std::string(id));
    return it == index.end() ? nullptr : it->second;
}

const GraphNode* Graph::findNode(std::string_view id) const {
    for (const GraphNode& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

GraphNode* Graph::findNode(std::string_view id) {
    for (GraphNode& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

std::string Graph::newNodeId() const {
    for (std::size_t i = nodes.size() + 1;; ++i) {
        std::string id = std::format("n{}", i);
        if (findNode(id) == nullptr) return id;
    }
}

Result<Graph> parseGraph(std::string_view text, std::string_view sourceName) {
    try {
        const Json root = Json::parse(text.begin(), text.end());
        checkKeys(root, "", {"format", "formatVersion", "properties", "nodes", "links"});
        if (!root.contains("format") || root["format"] != "px.graph") {
            fail("format", "expected \"format\": \"px.graph\" (is this a visual script?)");
        }
        if (!root.contains("formatVersion") || !root["formatVersion"].is_number_integer()) {
            fail("formatVersion", "missing or not a whole number");
        }
        const int version = root["formatVersion"].get<int>();
        if (version < 1) fail("formatVersion", "not a valid version");
        if (version > kGraphFormatVersion) {
            fail("", std::format("this visual script uses format version {}, but this engine supports up to version "
                                 "{}; update the engine",
                                 version, kGraphFormatVersion));
        }
        Graph g;
        if (root.contains("properties")) {
            const Json& props = root["properties"];
            if (!props.is_array()) fail("properties", "expected an array");
            if (props.size() > 64) fail("properties", "at most 64 properties");
            for (std::size_t i = 0; i < props.size(); ++i) {
                const std::string path = std::format("properties[{}]", i);
                checkKeys(props[i], path, {"name", "value"});
                if (!props[i].contains("name") || !props[i]["name"].is_string()) fail(path, "needs a \"name\"");
                if (!props[i].contains("value")) fail(path, "needs a \"value\"");
                g.properties.push_back({props[i]["name"].get<std::string>(), readValue(props[i]["value"], path + ".value")});
            }
        }
        if (root.contains("nodes")) {
            const Json& nodes = root["nodes"];
            if (!nodes.is_array()) fail("nodes", "expected an array");
            if (nodes.size() > kMaxGraphNodes) fail("nodes", std::format("at most {} nodes", kMaxGraphNodes));
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                const std::string path = std::format("nodes[{}]", i);
                const Json& j = nodes[i];
                checkKeys(j, path, {"id", "type", "position", "property", "inputs"});
                GraphNode n;
                if (!j.contains("id") || !j["id"].is_string()) fail(path, "needs an \"id\"");
                n.id = j["id"].get<std::string>();
                if (!j.contains("type") || !j["type"].is_string()) fail(path, "needs a \"type\"");
                n.type = j["type"].get<std::string>();
                if (j.contains("position")) {
                    const GraphValue p = readValue(j["position"], path + ".position");
                    if (!std::holds_alternative<Vec2>(p)) fail(path + ".position", "expected [x, y]");
                    n.position = std::get<Vec2>(p);
                }
                if (j.contains("property")) {
                    if (!j["property"].is_string()) fail(path + ".property", "expected a property name");
                    n.property = j["property"].get<std::string>();
                }
                if (j.contains("inputs")) {
                    if (!j["inputs"].is_object()) fail(path + ".inputs", "expected an object");
                    for (const auto& [pin, value] : j["inputs"].items()) {
                        n.inputs.emplace(pin, readValue(value, std::format("{}.inputs.{}", path, pin)));
                    }
                }
                g.nodes.push_back(std::move(n));
            }
        }
        if (root.contains("links")) {
            const Json& links = root["links"];
            if (!links.is_array()) fail("links", "expected an array");
            if (links.size() > kMaxGraphLinks) fail("links", std::format("at most {} links", kMaxGraphLinks));
            for (std::size_t i = 0; i < links.size(); ++i) {
                const std::string path = std::format("links[{}]", i);
                checkKeys(links[i], path, {"from", "to"});
                if (!links[i].contains("from") || !links[i]["from"].is_string() || !links[i].contains("to") ||
                    !links[i]["to"].is_string()) {
                    fail(path, "needs \"from\" and \"to\" (\"node.pin\")");
                }
                const auto [fromNode, fromPin] = splitPin(links[i]["from"].get<std::string>(), path + ".from");
                const auto [toNode, toPin] = splitPin(links[i]["to"].get<std::string>(), path + ".to");
                g.links.push_back({fromNode, fromPin, toNode, toPin});
            }
        }
        return g;
    } catch (const Json::exception& e) {
        return makeError(ErrorCode::InvalidData, "{}: not valid JSON: {}", sourceName, e.what());
    } catch (const std::runtime_error& e) {
        return makeError(ErrorCode::InvalidData, "{}: {}", sourceName, e.what());
    }
}

std::string serializeGraph(const Graph& g) {
    // Written by hand (not Json::dump) so files are compact and diff well:
    // one line per node and per link.
    const auto str = [](const std::string& s) { return Json(s).dump(); };
    const auto value = [](const GraphValue& v) { return writeValue(v).dump(); };
    const auto num = [](float f) { return Json(f).dump(); };
    std::string out = "{\n  \"format\": \"px.graph\",\n  \"formatVersion\": 1,\n  \"properties\": [";
    for (std::size_t i = 0; i < g.properties.size(); ++i) {
        out += std::format("{}\n    {{\"name\": {}, \"value\": {}}}", i == 0 ? "" : ",", str(g.properties[i].name),
                           value(g.properties[i].value));
    }
    out += g.properties.empty() ? "],\n" : "\n  ],\n";
    out += "  \"nodes\": [";
    for (std::size_t i = 0; i < g.nodes.size(); ++i) {
        const GraphNode& n = g.nodes[i];
        out += std::format("{}\n    {{\"id\": {}, \"type\": {}, \"position\": [{}, {}]", i == 0 ? "" : ",", str(n.id),
                           str(n.type), num(n.position.x), num(n.position.y));
        if (!n.property.empty()) out += std::format(", \"property\": {}", str(n.property));
        if (!n.inputs.empty()) {
            out += ", \"inputs\": {";
            bool first = true;
            for (const auto& [pin, v] : n.inputs) {
                out += std::format("{}{}: {}", first ? "" : ", ", str(pin), value(v));
                first = false;
            }
            out += "}";
        }
        out += "}";
    }
    out += g.nodes.empty() ? "],\n" : "\n  ],\n";
    out += "  \"links\": [";
    for (std::size_t i = 0; i < g.links.size(); ++i) {
        const GraphLink& l = g.links[i];
        out += std::format("{}\n    {{\"from\": {}, \"to\": {}}}", i == 0 ? "" : ",", str(l.fromNode + "." + l.fromPin),
                           str(l.toNode + "." + l.toPin));
    }
    out += g.links.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

std::string describeNode(const Graph& graph, std::string_view nodeId) {
    const GraphNode* n = graph.findNode(nodeId);
    if (n == nullptr) return std::format("node {}", nodeId);
    const NodeTypeInfo* t = findNodeType(n->type);
    std::string title = t != nullptr ? t->title : n->type;
    if (!n->property.empty()) title += " " + n->property;
    return std::format("node '{}' ({})", title, nodeId);
}

// --- Compiler -------------------------------------------------------------------------------

namespace {

std::string luaString(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 32) {
                    out += std::format("\\{:03}", static_cast<int>(static_cast<unsigned char>(c)));
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

std::string luaNumber(double d) { return std::format("{}", d); }

// `expr` usable before ':' or '.': names and field paths as they are, anything
// else in parentheses.
std::string prefix(const std::string& expr) {
    const bool simple = !expr.empty() && (std::isalpha(static_cast<unsigned char>(expr[0])) || expr[0] == '_') &&
                        std::all_of(expr.begin(), expr.end(), [](char c) {
                            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
                        });
    return simple ? expr : "(" + expr + ")";
}

std::string luaValue(const GraphValue& v) {
    return std::visit(
        [](const auto& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, double>) return luaNumber(x);
            else if constexpr (std::is_same_v<T, bool>) return x ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::string>) return luaString(x);
            else return std::format("Vec2({}, {})", luaNumber(x.x), luaNumber(x.y));
        },
        v);
}

bool valueFits(const GraphValue& v, PinType type) {
    switch (type) {
        case PinType::Number: return std::holds_alternative<double>(v);
        case PinType::Boolean: return std::holds_alternative<bool>(v);
        case PinType::String: return std::holds_alternative<std::string>(v);
        case PinType::Vector: return std::holds_alternative<Vec2>(v);
        case PinType::Any: return true;
        default: return false; // entities and exec have no typed-in values
    }
}

class Compiler {
public:
    Compiler(const Graph& graph, std::string_view name) : m_graph(graph), m_name(name) {}

    CompiledGraph run() {
        validate();
        if (m_problems.empty()) emitModule();
        CompiledGraph out;
        out.problems = std::move(m_problems);
        if (out.problems.empty()) {
            for (std::size_t i = 0; i < m_lines.size(); ++i) {
                out.source += m_lines[i];
                out.source += '\n';
            }
            out.lineNodes = std::move(m_lineNodes);
        }
        return out;
    }

private:
    using PinRef = std::pair<std::size_t, std::string>; // node index, pin name

    void problem(std::size_t node, std::string message) {
        m_problems.push_back({node == npos ? std::string() : m_graph.nodes[node].id,
                              node == npos ? std::move(message)
                                           : std::format("{}: {}", describeNode(m_graph, m_graph.nodes[node].id), message)});
    }
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    const NodeTypeInfo& type(std::size_t i) const { return *m_types[i]; }

    // --- validation ---
    void validate() {
        if (m_graph.nodes.size() > kMaxGraphNodes) problem(npos, std::format("too many nodes (at most {})", kMaxGraphNodes));
        if (m_graph.links.size() > kMaxGraphLinks) problem(npos, std::format("too many links (at most {})", kMaxGraphLinks));
        std::set<std::string> names;
        for (const GraphProperty& p : m_graph.properties) {
            if (!isIdentifier(p.name) || p.name == "entity" || p.name == "properties" || p.name.starts_with("__")) {
                problem(npos, std::format("'{}' cannot be a property name: use letters, digits and _ (not starting "
                                          "with a digit), and not entity or properties",
                                          p.name));
            } else if (!names.insert(p.name).second) {
                problem(npos, std::format("there are two properties called '{}'", p.name));
            }
        }
        for (std::size_t i = 0; i < m_graph.nodes.size(); ++i) {
            const GraphNode& n = m_graph.nodes[i];
            m_types.push_back(findNodeType(n.type));
            if (!isNodeId(n.id)) problem(npos, std::format("'{}' is not a valid node id", n.id));
            if (!m_index.emplace(n.id, i).second) problem(npos, std::format("two nodes have the id '{}'", n.id));
            if (m_types.back() == nullptr) {
                m_problems.push_back({n.id, std::format("node {}: unknown node type '{}'", n.id, n.type)});
                continue;
            }
            const NodeTypeInfo& t = *m_types.back();
            if (n.type == "property.get" || n.type == "property.set") {
                if (std::none_of(m_graph.properties.begin(), m_graph.properties.end(),
                                 [&](const GraphProperty& p) { return p.name == n.property; })) {
                    problem(i, std::format("there is no property '{}'", n.property));
                }
            }
            for (const auto& [pin, value] : n.inputs) {
                const PinInfo* in = t.input(pin);
                if (in == nullptr || in->type == PinType::Exec) {
                    problem(i, std::format("has no input '{}'", pin));
                } else if (!valueFits(value, in->type)) {
                    problem(i, std::format("input '{}' needs a {}", pin, pinTypeName(in->type)));
                }
            }
        }
        if (!m_problems.empty()) return;
        for (const GraphLink& l : m_graph.links) {
            const auto from = m_index.find(l.fromNode);
            const auto to = m_index.find(l.toNode);
            if (from == m_index.end() || to == m_index.end()) {
                problem(npos, std::format("a link connects a node that does not exist ({} -> {})", l.fromNode, l.toNode));
                continue;
            }
            const PinInfo* out = type(from->second).output(l.fromPin);
            const PinInfo* in = type(to->second).input(l.toPin);
            if (out == nullptr) {
                problem(from->second, std::format("has no output '{}'", l.fromPin));
                continue;
            }
            if (in == nullptr) {
                problem(to->second, std::format("has no input '{}'", l.toPin));
                continue;
            }
            if (!pinTypesConnect(out->type, in->type)) {
                problem(to->second, std::format("input '{}' ({}) cannot take '{}' ({}) of {}", l.toPin,
                                                pinTypeName(in->type), l.fromPin, pinTypeName(out->type),
                                                describeNode(m_graph, l.fromNode)));
                continue;
            }
            if (in->literalOnly) {
                problem(to->second, std::format("input '{}' must be typed into the node, not connected", l.toPin));
                continue;
            }
            if (out->type == PinType::Exec) {
                m_execOut[{from->second, l.fromPin}].push_back(to->second);
            } else if (!m_dataIn.emplace(PinRef{to->second, l.toPin}, PinRef{from->second, l.fromPin}).second) {
                problem(to->second, std::format("input '{}' has two connections", l.toPin));
            } else {
                m_outputUsed.insert({from->second, l.fromPin});
            }
        }
        // Inputs that need a connection or a value.
        for (std::size_t i = 0; i < m_graph.nodes.size(); ++i) {
            const GraphNode& n = m_graph.nodes[i];
            for (const PinInfo& in : type(i).inputs) {
                if (in.type == PinType::Exec || in.optional || m_dataIn.contains({i, in.name})) continue;
                if (in.literalOnly && !n.inputs.contains(in.name)) {
                    problem(i, std::format("type a value into '{}'", in.name));
                } else if ((in.type == PinType::Entity || in.type == PinType::EntityList) && !n.inputs.contains(in.name)) {
                    problem(i, std::format("input '{}' needs a connection ({})", in.name, pinTypeName(in.type)));
                }
            }
            if (n.type == "event.every") {
                const auto it = n.inputs.find("seconds");
                if (it != n.inputs.end() && std::holds_alternative<double>(it->second) && !(std::get<double>(it->second) > 0.0)) {
                    problem(i, "'seconds' must be more than 0");
                }
            }
        }
    }

    // --- output ---
    void line(std::string text, std::size_t node = npos) {
        m_lines.push_back(std::string(static_cast<std::size_t>(m_indent) * 4, ' ') + std::move(text));
        m_lineNodes.push_back(node == npos ? std::string() : m_graph.nodes[node].id);
        if (m_lines.size() > 100000 && !m_tooLong) {
            m_tooLong = true;
            problem(npos, "the compiled script is too long (chains that join again are copied; simplify the graph)");
        }
    }

    std::string local(std::size_t node, std::string_view pin) const { return std::format("n{}_{}", node, pin); }

    // The Luau expression for input `pin` of node `node`.
    std::string inputExpr(std::size_t node, const PinInfo& pin) {
        if (const auto it = m_dataIn.find({node, pin.name}); it != m_dataIn.end()) {
            return outputExpr(it->second.first, it->second.second, node);
        }
        const GraphNode& n = m_graph.nodes[node];
        if (const auto it = n.inputs.find(pin.name); it != n.inputs.end()) return luaValue(it->second);
        if (pin.name == "self" && pin.type == PinType::Entity) return "self.entity";
        if (pin.optional) return "nil";
        switch (pin.type) {
            case PinType::Number: return "0";
            case PinType::Boolean: return "false";
            case PinType::String: return "\"\"";
            case PinType::Vector: return "Vec2(0, 0)";
            default: return "nil";
        }
    }

    std::string args(std::size_t node, bool skipSelf) {
        std::vector<std::string> values;
        for (const PinInfo& in : type(node).inputs) {
            if (in.type == PinType::Exec || (skipSelf && in.name == "self")) continue;
            values.push_back(inputExpr(node, in));
        }
        while (!values.empty() && values.back() == "nil") values.pop_back(); // trailing optional arguments
        std::string out;
        for (std::size_t i = 0; i < values.size(); ++i) out += (i == 0 ? "" : ", ") + values[i];
        return out;
    }

    std::string callee(std::size_t node) const {
        const std::string& id = type(node).id;
        return id.substr(id.find('.') + 1); // "call.Game.addScore" -> "Game.addScore", "call.Vec2" -> "Vec2"
    }

    // The expression for output `pin` of node `node`, used by node `user`.
    std::string outputExpr(std::size_t node, const std::string& pin, std::size_t user) {
        const NodeTypeInfo& t = type(node);
        const std::string& id = t.id;
        if (t.kind != NodeKind::Data) {
            // Values that exist only where the node has run: event arguments, loop items, results.
            if (!m_inScope.contains({node, pin})) {
                problem(user, std::format("uses '{}' of {}, which is only available in the chain after that node",
                                          pin, describeNode(m_graph, m_graph.nodes[node].id)));
                return "nil";
            }
            return m_inScope.at({node, pin});
        }
        if (m_visiting.contains(node)) {
            problem(node, "its inputs come from its own output (a loop of values)");
            return "nil";
        }
        m_visiting.insert(node);
        std::string e = dataExpr(node, pin, id);
        m_visiting.erase(node);
        return e;
    }

    std::string in(std::size_t node, std::string_view name) {
        const PinInfo* p = type(node).input(name);
        return p != nullptr ? inputExpr(node, *p) : std::string("nil");
    }

    std::string dataExpr(std::size_t node, const std::string& pin, const std::string& id) {
        const GraphNode& n = m_graph.nodes[node];
        if (id.starts_with("call.")) return std::format("{}({})", callee(node), args(node, false));
        if (id.starts_with("method.")) return std::format("{}:{}({})", prefix(in(node, "self")), callee(node), args(node, true));
        if (id.starts_with("get.")) return std::format("{}.{}", prefix(in(node, "self")), id.substr(4));
        if (id == "data.self") return "self.entity";
        if (id == "property.get") return "self." + n.property;
        if (id == "math.add") return std::format("({} + {})", in(node, "a"), in(node, "b"));
        if (id == "math.subtract") return std::format("({} - {})", in(node, "a"), in(node, "b"));
        if (id == "math.multiply") return std::format("({} * {})", in(node, "a"), in(node, "b"));
        if (id == "math.divide") return std::format("({} / {})", in(node, "a"), in(node, "b"));
        if (id == "math.random") {
            return std::format("(function(lo, hi) return lo + math.random() * (hi - lo) end)({}, {})", in(node, "min"),
                               in(node, "max"));
        }
        if (id == "compare.equal") return std::format("({} == {})", in(node, "a"), in(node, "b"));
        if (id == "compare.notEqual") return std::format("({} ~= {})", in(node, "a"), in(node, "b"));
        if (id == "compare.less") return std::format("({} < {})", in(node, "a"), in(node, "b"));
        if (id == "compare.greater") return std::format("({} > {})", in(node, "a"), in(node, "b"));
        if (id == "logic.and") return std::format("({} and {})", in(node, "a"), in(node, "b"));
        if (id == "logic.or") return std::format("({} or {})", in(node, "a"), in(node, "b"));
        if (id == "logic.not") return std::format("(not {})", in(node, "value"));
        if (id == "vector.split") return std::format("{}.{}", prefix(in(node, "vector")), pin == "x" ? "x" : "y");
        if (id == "vector.length") return std::format("vector.magnitude({})", in(node, "vector"));
        if (id == "text.join") return std::format("(tostring({}) .. tostring({}))", in(node, "a"), in(node, "b"));
        problem(node, "cannot be compiled (unknown data node)");
        return "nil";
    }

    // Emits the chains connected to exec output `pin` of `node`.
    void chain(std::size_t node, const std::string& pin) {
        const auto it = m_execOut.find({node, pin});
        if (it == m_execOut.end()) return;
        for (const std::size_t target : it->second) {
            if (m_tooLong) return;
            statement(target);
        }
    }

    // Opens a block: values made available inside it are forgotten at its end.
    template <class Fn>
    void block(Fn&& body) {
        ++m_indent;
        scope(std::forward<Fn>(body));
        --m_indent;
    }
    // The same without indenting (each event's chain in a shared handler).
    template <class Fn>
    void scope(Fn&& body) {
        const auto saved = m_inScope;
        body();
        m_inScope = saved;
    }

    void statement(std::size_t node) {
        if (std::find(m_execStack.begin(), m_execStack.end(), node) != m_execStack.end()) {
            problem(node, "the chain comes back to this node; use Repeat or For Each for loops");
            return;
        }
        if (type(node).kind != NodeKind::Action) {
            problem(node, "only action nodes can be in a chain");
            return;
        }
        m_execStack.push_back(node);
        const NodeTypeInfo& t = type(node);
        const std::string& id = t.id;
        const GraphNode& n = m_graph.nodes[node];
        if (id == "flow.branch") {
            line(std::format("if {} then", in(node, "condition")), node);
            block([&] { chain(node, "true"); });
            if (m_execOut.contains({node, "false"})) {
                line("else", node);
                block([&] { chain(node, "false"); });
            }
            line("end", node);
        } else if (id == "flow.sequence") {
            chain(node, "first");
            chain(node, "second");
            chain(node, "third");
        } else if (id == "flow.forEach") {
            line(std::format("for _, {} in ipairs({}) do", local(node, "item"), in(node, "list")), node);
            block([&] {
                m_inScope[{node, "item"}] = local(node, "item");
                chain(node, "body");
            });
            line("end", node);
            chain(node, "done");
        } else if (id == "flow.repeat") {
            line(std::format("for {} = 1, {} do", local(node, "index"), in(node, "count")), node);
            block([&] {
                m_inScope[{node, "index"}] = local(node, "index");
                chain(node, "body");
            });
            line("end", node);
            chain(node, "done");
        } else if (id == "flow.delay") {
            m_needsTimers = true;
            line(std::format("__after(self, {}, function()", in(node, "seconds")), node);
            block([&] { chain(node, "then"); });
            line("end)", node);
        } else {
            std::string code;
            if (id == "property.set") {
                code = std::format("self.{} = {}", n.property, in(node, "value"));
            } else if (id.starts_with("set.")) {
                code = std::format("{}.{} = {}", prefix(in(node, "self")), id.substr(4), in(node, "value"));
            } else if (id.starts_with("method.")) {
                code = std::format("{}:{}({})", prefix(in(node, "self")), callee(node), args(node, true));
            } else if (id.starts_with("call.")) {
                code = std::format("{}({})", callee(node), args(node, false));
            } else {
                problem(node, "cannot be compiled (unknown action)");
            }
            if (t.output("result") != nullptr && m_outputUsed.contains({node, "result"})) {
                line(std::format("local {} = {}", local(node, "result"), code), node);
                m_inScope[{node, "result"}] = local(node, "result");
            } else {
                // A statement starting with '(' would continue the previous line's call in Luau.
                line(code.starts_with("(") ? ";" + code : code, node);
            }
            chain(node, "then");
        }
        m_execStack.pop_back();
    }

    // An event handler: `function Graph:Name(params)` running the chains of
    // every event node in `events`, plus `extra` (timers) first.
    void handler(const std::string& event, const std::vector<std::string>& params, const std::vector<std::size_t>& events,
                 const std::function<void()>& extra = {}) {
        std::string list;
        for (std::size_t i = 0; i < params.size(); ++i) list += (i == 0 ? "" : ", ") + params[i];
        line(std::format("function Graph:{}({})", event, list));
        block([&] {
            if (extra) extra();
            for (const std::size_t e : events) {
                scope([&] {
                    for (const PinInfo& out : type(e).outputs) {
                        if (out.type != PinType::Exec) m_inScope[{e, out.name}] = out.name;
                    }
                    line(std::format("-- {}", describeNode(m_graph, m_graph.nodes[e].id)), e);
                    chain(e, "then");
                });
            }
        });
        line("end");
        line("");
    }

    void emitModule() {
        line(std::format("-- {}: compiled from a visual script by Project Pixel.", m_name));
        line("-- Do not edit: change the graph in Studio, this code is made from it.");
        line("");
        line("local Graph = {");
        ++m_indent;
        line("properties = {");
        ++m_indent;
        for (const GraphProperty& p : m_graph.properties) line(std::format("{} = {},", p.name, luaValue(p.value)));
        --m_indent;
        line("},");
        --m_indent;
        line("}");
        line("");
        // Timers are only emitted when used; their code is written first and
        // filled in below (Luau needs the helper before the handlers).
        const std::size_t helperAt = m_lines.size();

        // Group the event nodes.
        std::map<std::string, std::vector<std::size_t>> lifecycle;
        std::vector<std::size_t> custom;
        std::vector<std::size_t> every;
        for (std::size_t i = 0; i < m_graph.nodes.size(); ++i) {
            const std::string& id = type(i).id;
            if (id == "event.custom") {
                custom.push_back(i);
            } else if (id == "event.every") {
                every.push_back(i);
            } else if (type(i).kind == NodeKind::Event) {
                lifecycle[id.substr(6)].push_back(i);
            }
        }
        const auto paramsOf = [&](const std::string& event) {
            std::vector<std::string> params;
            if (const NodeTypeInfo* t = findNodeType("event." + event)) {
                for (const PinInfo& p : t->outputs) {
                    if (p.type != PinType::Exec) params.push_back(p.name);
                }
            }
            return params;
        };
        // Start also registers custom event listeners.
        if (!lifecycle["Start"].empty() || !custom.empty()) {
            handler("Start", {}, lifecycle["Start"], [&] {
                for (const std::size_t c : custom) {
                    line(std::format("Events.on({}, function({})", in(c, "name"), local(c, "value")), c);
                    block([&] {
                        m_inScope[{c, "value"}] = local(c, "value");
                        chain(c, "then");
                    });
                    line("end)", c);
                }
            });
        }
        for (const auto& [event, nodes] : lifecycle) {
            if (event == "Start" || event == "Tick" || nodes.empty()) continue;
            handler(event, paramsOf(event), nodes);
        }
        // Tick: timers (Delay), "every" nodes, then the Tick chains. Emitted
        // last so it knows whether any Delay node needs the timer pump.
        const std::size_t tickStart = m_lines.size();
        const bool tick = !lifecycle["Tick"].empty() || !every.empty();
        if (tick) {
            handler("Tick", {"dt"}, lifecycle["Tick"], [&] {
                for (const std::size_t e : every) {
                    const std::string acc = std::format("self.__every{}", e);
                    line(std::format("{} = ({} or 0) + dt", acc, acc), e);
                    line(std::format("if {} >= {} then", acc, in(e, "seconds")), e);
                    block([&] {
                        line(std::format("{} -= {}", acc, in(e, "seconds")), e);
                        chain(e, "then");
                    });
                    line("end", e);
                }
            });
        }
        if (m_needsTimers) {
            // The pump goes at the top of Tick (creating Tick if there is none).
            std::vector<std::string> pump{
                "if self.__timers then",
                "    local now = Game.time()",
                "    local due = {}",
                "    for i = #self.__timers, 1, -1 do",
                "        if now >= self.__timers[i].at then",
                "            table.insert(due, 1, table.remove(self.__timers, i))",
                "        end",
                "    end",
                "    for _, t in ipairs(due) do",
                "        t.fn()",
                "    end",
                "end",
            };
            if (tick) {
                std::vector<std::string> indented;
                for (const auto& p : pump) indented.push_back("    " + p);
                m_lines.insert(m_lines.begin() + static_cast<std::ptrdiff_t>(tickStart + 1), indented.begin(), indented.end());
                m_lineNodes.insert(m_lineNodes.begin() + static_cast<std::ptrdiff_t>(tickStart + 1), indented.size(),
                                   std::string());
            } else {
                line("function Graph:Tick(dt)");
                for (const auto& p : pump) line("    " + p);
                line("end");
                line("");
            }
            const std::vector<std::string> helper{
                "-- Delay: runs fn after `seconds` of game time (checked every Tick).",
                "local function __after(self, seconds, fn)",
                "    self.__timers = self.__timers or {}",
                "    table.insert(self.__timers, { at = Game.time() + seconds, fn = fn })",
                "end",
                "",
            };
            m_lines.insert(m_lines.begin() + static_cast<std::ptrdiff_t>(helperAt), helper.begin(), helper.end());
            m_lineNodes.insert(m_lineNodes.begin() + static_cast<std::ptrdiff_t>(helperAt), helper.size(), std::string());
        }
        line("return Graph");
    }

    const Graph& m_graph;
    std::string m_name;
    std::vector<const NodeTypeInfo*> m_types;
    std::map<std::string, std::size_t> m_index;
    std::map<PinRef, PinRef> m_dataIn;
    std::map<PinRef, std::vector<std::size_t>> m_execOut;
    std::set<PinRef> m_outputUsed;
    std::map<PinRef, std::string> m_inScope; // values available here: (node, pin) -> Luau name
    std::set<std::size_t> m_visiting;
    std::vector<std::size_t> m_execStack;
    std::vector<GraphProblem> m_problems;
    std::vector<std::string> m_lines;
    std::vector<std::string> m_lineNodes;
    int m_indent = 0;
    bool m_needsTimers = false;
    bool m_tooLong = false;
};

} // namespace

CompiledGraph compileGraph(const Graph& graph, std::string_view name) { return Compiler(graph, name).run(); }

} // namespace px::scripting
