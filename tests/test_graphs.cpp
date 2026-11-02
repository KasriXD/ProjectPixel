// Visual scripts: the node catalog from the API reference, the graph format,
// compiling to Luau, problems named by node, and graphs behaving like the
// equivalent scripts in a running game.

#include <doctest.h>

#include "px/core/Log.hpp"
#include "px/runtime/GameSession.hpp"
#include "px/scripting/ApiReference.hpp"
#include "px/scripting/Graph.hpp"
#include "px/scripting/ScriptLibrary.hpp"

#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace px;
using namespace px::scene;
using namespace px::scripting;

namespace {

template <class T>
T must(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result.value());
}
bool contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

class Logs {
public:
    Logs() {
        m_previous = log::minLevel();
        log::setMinLevel(log::Level::Info);
        log::setConsoleOutput(false);
        m_handle = log::addSink([this](log::Level, std::string_view m) { lines.emplace_back(m); });
    }
    ~Logs() {
        log::removeSink(m_handle);
        log::setConsoleOutput(true);
        log::setMinLevel(m_previous);
    }
    Logs(const Logs&) = delete;
    Logs& operator=(const Logs&) = delete;
    [[nodiscard]] bool has(std::string_view text) const {
        for (const auto& l : lines) {
            if (l.find(text) != std::string::npos) return true;
        }
        return false;
    }
    [[nodiscard]] int count(std::string_view text) const {
        int n = 0;
        for (const auto& l : lines) n += l.find(text) != std::string::npos ? 1 : 0;
        return n;
    }
    std::vector<std::string> lines;

private:
    int m_handle = 0;
    log::Level m_previous = log::Level::Info;
};

GraphNode node(std::string id, std::string type, std::map<std::string, GraphValue> inputs = {}, std::string property = {}) {
    GraphNode n;
    n.id = std::move(id);
    n.type = std::move(type);
    n.inputs = std::move(inputs);
    n.property = std::move(property);
    return n;
}

GraphLink wire(const std::string& from, const std::string& to) {
    const auto f = from.rfind('.');
    const auto t = to.rfind('.');
    return {from.substr(0, f), from.substr(f + 1), to.substr(0, t), to.substr(t + 1)};
}

// The coin as a graph: on TriggerEnter, if the other has a Player, add
// `value` to the score, emit "coin_collected" and destroy this entity.
Graph coinGraph() {
    Graph g;
    g.properties = {{"value", 1.0}};
    g.nodes = {node("enter", "event.TriggerEnter"),
               node("isPlayer", "method.hasComponent", {{"name", std::string("Player")}}),
               node("if", "flow.branch"),
               node("value", "property.get", {}, "value"),
               node("score", "call.Game.addScore"),
               node("emit", "call.Events.emit", {{"name", std::string("coin_collected")}}),
               node("destroy", "method.destroy")};
    g.links = {wire("enter.other", "isPlayer.self"), wire("enter.then", "if.in"),
               wire("isPlayer.result", "if.condition"), wire("if.true", "score.in"),
               wire("value.value", "score.amount"), wire("score.then", "emit.in"),
               wire("value.value", "emit.value"), wire("emit.then", "destroy.in")};
    return g;
}

const assets::AssetId kGraphId{Guid(0x6a000000u, 1)};
const assets::AssetId kScriptId{Guid(0x5c000000u, 1)};

// A small game: a player walking right into whatever has the behaviour.
struct Game {
    project::ProjectManifest project;
    std::shared_ptr<ScriptLibrary> library = std::make_shared<ScriptLibrary>();
    Scene scene;
    Entity player;
    Entity thing;

    Game() {
        project.name = "Graphs";
        project.input.actions = {{"move_left", {Key::Left}}, {"move_right", {Key::Right}}, {"jump", {Key::Space}}};
        player = scene.createEntity("Player");
        scene.set(player, Transform{{0, 0}});
        scene.set(player, BoxCollider{{10, 10}, {}, false});
        KinematicBody body;
        body.velocity = {120, 0};
        scene.set(player, body);
        scene.set(player, Player{});
        thing = scene.createEntity("Thing");
        scene.set(thing, Transform{{40, 0}});
        scene.set(thing, BoxCollider{{10, 10}, {}, true});
    }
    void attachGraph(const Graph& g, std::vector<ScriptProperty> props = {}) {
        const Status added = library->addGraph(kGraphId, "graphs/test.graph.json", serializeGraph(g));
        REQUIRE_MESSAGE(added.ok(), (added.ok() ? std::string() : added.error().message));
        scene.set(thing, Scripts{{AttachedScript{kGraphId, std::move(props)}}});
    }
    std::unique_ptr<runtime::GameSession> start() {
        return must(runtime::GameSession::create(project, scene, library));
    }
};

void run(runtime::GameSession& s, int ticks) {
    for (int i = 0; i < ticks; ++i) s.tick({});
}

} // namespace

TEST_CASE("Graphs: every API member has a parsed signature, and the catalog covers the API") {
    for (const ApiMember& m : apiReference()) {
        CAPTURE(m.signature);
        const ApiSignature sig = parseApiSignature(m);
        for (const ApiParam& p : sig.params) {
            CHECK_FALSE(p.name.empty());
            CHECK_FALSE(p.type.empty());
        }
        if (m.kind == ApiKind::Property) CHECK(sig.result.has_value());
    }
    const ApiSignature spawn = parseApiSignature(*std::find_if(apiReference().begin(), apiReference().end(),
                                                               [](const ApiMember& m) { return m.name == "spawn"; }));
    REQUIRE(spawn.params.size() == 3);
    CHECK(spawn.params[0].type == "string");
    CHECK(spawn.params[1].optional);
    CHECK(spawn.result->type == "Entity");

    std::set<std::string> ids;
    for (const NodeTypeInfo& t : nodeCatalog()) {
        CHECK_MESSAGE(ids.insert(t.id).second, t.id);
        CHECK_FALSE(t.title.empty());
    }
    for (const char* id : {"event.Start", "event.TriggerEnter", "call.Game.addScore", "method.destroy", "get.position",
                           "set.position", "call.World.spawn", "flow.branch", "flow.forEach", "flow.delay", "math.add",
                           "event.custom", "event.every"}) {
        CHECK_MESSAGE(findNodeType(id) != nullptr, id);
    }
    CHECK(findNodeType("call.Events.on") == nullptr); // takes a function: "On Custom Event" instead
    CHECK(findNodeType("call.Game.addScore")->authorityOnly);
    CHECK(findNodeType("get.position")->kind == NodeKind::Data);
}

TEST_CASE("Graphs: every node type compiles to valid Luau") {
    for (const NodeTypeInfo& t : nodeCatalog()) {
        CAPTURE(t.id);
        Graph g;
        g.properties = {{"p", 1.0}};
        GraphNode n = node("n", t.id, {}, t.needsProperty || t.id.starts_with("property.") ? "p" : "");
        for (const PinInfo& in : t.inputs) {
            if (in.literalOnly) n.inputs[in.name] = in.type == PinType::Number ? GraphValue(1.0) : GraphValue(std::string("x"));
        }
        g.nodes.push_back(n);
        g.nodes.push_back(node("self", "data.self"));
        g.nodes.push_back(node("all", "call.World.findWithTag", {{"tag", std::string("x")}}));
        for (const PinInfo& in : t.inputs) {
            if (in.type == PinType::Entity) g.links.push_back(wire("self.entity", "n." + in.name));
            if (in.type == PinType::EntityList) g.links.push_back(wire("all.result", "n." + in.name));
        }
        if (t.kind == NodeKind::Event) {
            g.nodes.push_back(node("print", "call.print"));
            g.links.push_back(wire("n.then", "print.in"));
            for (const PinInfo& out : t.outputs) {
                if (out.type != PinType::Exec) {
                    g.links.push_back(wire("n." + out.name, "print.value"));
                    break;
                }
            }
        } else {
            g.nodes.push_back(node("start", "event.Start"));
            g.nodes.push_back(node("print", "call.print"));
            if (t.kind == NodeKind::Action) {
                g.links.push_back(wire("start.then", "n.in"));
                const PinInfo* next = t.output("then") ? t.output("then") : t.output("done") ? t.output("done") : t.output("true");
                if (next != nullptr) g.links.push_back(wire("n." + next->name, "print.in"));
            } else {
                g.links.push_back(wire("start.then", "print.in"));
                g.links.push_back(wire("n." + t.outputs.front().name, "print.value"));
            }
        }
        const CompiledGraph compiled = compileGraph(g, "all");
        for (const auto& p : compiled.problems) CAPTURE(p.message);
        REQUIRE(compiled.ok());
        ScriptLibrary lib;
        const auto added = lib.add(kGraphId, "all", compiled.source);
        CHECK_MESSAGE(added.ok(), (added.ok() ? std::string() : added.error().message + "\n" + compiled.source));
    }
}

TEST_CASE("Graphs: the coin graph plays like coin.luau") {
    const auto play = [](bool asGraph) {
        Game game;
        if (asGraph) {
            game.attachGraph(coinGraph(), {{"value", 5.0}});
        } else {
            REQUIRE(game.library->add(kScriptId, "coin", R"(
                local Coin = { properties = { value = 1 } }
                function Coin:TriggerEnter(other)
                    if other:hasComponent("Player") then
                        Game.addScore(self.value)
                        Events.emit("coin_collected", self.value)
                        self.entity:destroy()
                    end
                end
                return Coin)").ok());
            game.scene.set(game.thing, Scripts{{AttachedScript{kScriptId, {{"value", 5.0}}}}});
        }
        auto s = game.start();
        run(*s, 30);
        return std::pair{s->state().score, s->scene().isAlive(game.thing) ? 1 : 0};
    };
    const auto graph = play(true);
    const auto script = play(false);
    CHECK(graph.first == 5);
    CHECK(graph.second == 0);
    CHECK(graph == script);
}

TEST_CASE("Graphs: flow, math, properties, custom events and timers") {
    Logs logs;
    Game game;
    game.scene.get<KinematicBody>(game.player).velocity = {};
    Graph g;
    g.properties = {{"count", 0.0}};
    g.nodes = {
        node("start", "event.Start"),
        node("rep", "flow.repeat", {{"count", 3.0}}),
        node("cnt", "property.get", {}, "count"),
        node("inc", "math.add", {{"b", 1.0}}),
        node("set", "property.set", {}, "count"),
        node("say", "call.print"),
        node("join", "text.join", {{"a", std::string("count=")}}),
        node("emit", "call.Events.emit", {{"name", std::string("ping")}, {"value", 7.0}}),
        node("on", "event.custom", {{"name", std::string("ping")}}),
        node("got", "call.print"),
        node("wait", "flow.delay", {{"seconds", 0.5}}),
        node("late", "call.print", {{"value", std::string("half a second later")}}),
        node("every", "event.every", {{"seconds", 0.25}}),
        node("tick", "call.print", {{"value", std::string("every quarter")}}),
        node("cmp", "compare.greater", {{"b", 2.0}}),
        node("if", "flow.branch"),
        node("big", "call.print", {{"value", std::string("more than two")}}),
    };
    g.links = {
        wire("start.then", "rep.in"), wire("rep.body", "set.in"), wire("cnt.value", "inc.a"),
        wire("inc.result", "set.value"), wire("rep.done", "say.in"), wire("cnt.value", "join.b"),
        wire("join.result", "say.value"), wire("say.then", "if.in"), wire("cnt.value", "cmp.a"),
        wire("cmp.result", "if.condition"), wire("if.true", "big.in"), wire("big.then", "emit.in"),
        wire("emit.then", "wait.in"), wire("wait.then", "late.in"), wire("on.then", "got.in"),
        wire("on.value", "got.value"), wire("every.then", "tick.in"),
    };
    game.attachGraph(g);
    auto s = game.start();
    run(*s, 1);
    CHECK(logs.has("count=3"));
    CHECK(logs.has("more than two"));
    CHECK_FALSE(logs.has("half a second later"));
    run(*s, 2);
    CHECK(logs.has("] 7")); // the custom event's value
    run(*s, 40);
    CHECK(logs.has("half a second later"));
    CHECK(logs.count("every quarter") == 2); // at 0.25 s and 0.5 s of the first 43 ticks
    CHECK(s->state().scriptErrors == 0);
}

TEST_CASE("Graphs: problems name the node") {
    const auto problems = [](const Graph& g) {
        std::string all;
        for (const auto& p : compileGraph(g, "t").problems) all += p.message + "\n";
        return all;
    };
    Graph unknown;
    unknown.nodes = {node("a", "call.Game.fly")};
    CHECK(contains(problems(unknown), "unknown node type 'call.Game.fly'"));

    Graph missing;
    missing.nodes = {node("start", "event.Start"), node("each", "flow.forEach")};
    missing.links = {wire("start.then", "each.in")};
    CHECK(contains(problems(missing), "node 'For Each Entity' (each): input 'list' needs a connection"));

    Graph mismatch;
    mismatch.nodes = {node("start", "event.Start"), node("pos", "get.position"), node("if", "flow.branch")};
    mismatch.links = {wire("start.then", "if.in"), wire("pos.value", "if.condition")};
    CHECK(contains(problems(mismatch), "input 'condition' (boolean) cannot take 'value' (vector)"));

    Graph loop;
    loop.nodes = {node("start", "event.Start"), node("a", "call.print"), node("b", "call.print")};
    loop.links = {wire("start.then", "a.in"), wire("a.then", "b.in"), wire("b.then", "a.in")};
    CHECK(contains(problems(loop), "the chain comes back to this node"));

    Graph scope;
    scope.nodes = {node("enter", "event.TriggerEnter"), node("start", "event.Start"), node("say", "call.print")};
    scope.links = {wire("start.then", "say.in"), wire("enter.other", "say.value")};
    CHECK(contains(problems(scope), "only available in the chain after that node"));

    Graph literal;
    literal.nodes = {node("on", "event.custom"), node("name", "text.join")};
    literal.links = {wire("name.result", "on.name")};
    CHECK(contains(problems(literal), "must be typed into the node"));

    Graph property;
    property.nodes = {node("get", "property.get", {}, "speed")};
    CHECK(contains(problems(property), "there is no property 'speed'"));
}

TEST_CASE("Graphs: runtime errors point at the node that failed") {
    Logs logs;
    Game game;
    Graph g;
    g.nodes = {node("start", "event.Start"), node("spawn", "call.World.spawn", {{"prefab", std::string("Nothing")}})};
    g.links = {wire("start.then", "spawn.in")};
    game.attachGraph(g);
    auto s = game.start();
    run(*s, 1);
    CHECK(s->state().scriptErrors == 1);
    CHECK(contains(s->state().lastScriptError, "node 'World Spawn' (spawn)"));
    CHECK(contains(s->state().lastScriptError, "no prefab named 'Nothing'"));
}

TEST_CASE("Graphs: files round-trip and refuse bad data") {
    Graph g = coinGraph();
    g.nodes[0].position = {12.5f, -40};
    const std::string text = serializeGraph(g);
    const Graph back = must(parseGraph(text, "coin.graph.json"));
    CHECK(serializeGraph(back) == text);
    CHECK(back.nodes.size() == g.nodes.size());
    CHECK(back.links == g.links);
    CHECK(back.nodes[0].position == Vec2{12.5f, -40});
    CHECK(compileGraph(back, "coin").ok());

    const auto error = [](std::string_view json) {
        auto r = parseGraph(json, "bad.graph.json");
        return r ? std::string() : r.error().message;
    };
    CHECK(contains(error(R"({"format": "px.graph", "formatVersion": 2})"), "update the engine"));
    CHECK(contains(error(R"({"format": "px.scene", "formatVersion": 1})"), "is this a visual script?"));
    CHECK(contains(error(R"({"format": "px.graph", "formatVersion": 1, "nodes": [{"id": "a"}]})"), "needs a \"type\""));
    CHECK(contains(error(R"({"format": "px.graph", "formatVersion": 1, "links": [{"from": "a", "to": "b.c"}]})"),
                   "expected \"node.pin\""));
    CHECK(contains(error(R"({"format": "px.graph", "formatVersion": 1, "nodez": []})"), "unknown field 'nodez'"));
    CHECK(contains(error("{nope"), "not valid JSON"));
}
