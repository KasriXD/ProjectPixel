#include <doctest.h>

#include "px/core/FileSystem.hpp"
#include "px/core/Log.hpp"
#include "px/project/Project.hpp"
#include "px/project/SceneFormat.hpp"
#include "px/runtime/GameLoader.hpp"
#include "px/runtime/GameSession.hpp"
#include "px/scripting/ApiReference.hpp"
#include "px/scripting/ScriptLibrary.hpp"
#include "px/scripting/ScriptSystem.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace px;
using namespace px::scene;
using namespace px::runtime;

namespace {

// Captures log output (script print/warn and engine errors) during a test.
class LogCapture {
public:
    LogCapture() {
        m_previous = log::minLevel();
        log::setMinLevel(log::Level::Info);
        log::setConsoleOutput(false);
        m_handle = log::addSink([this](log::Level level, std::string_view msg) {
            lines.emplace_back(msg);
            if (level == log::Level::Error) errors.emplace_back(msg);
            if (level == log::Level::Warn) warnings.emplace_back(msg);
        });
    }
    ~LogCapture() {
        log::removeSink(m_handle);
        log::setConsoleOutput(true);
        log::setMinLevel(m_previous);
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    // Lines printed by scripts, without the "[script] " prefix.
    [[nodiscard]] std::vector<std::string> printed() const {
        std::vector<std::string> out;
        for (const auto& l : lines) {
            if (l.starts_with("[")) {
                const auto close = l.find("] ");
                if (close != std::string::npos) out.push_back(l.substr(close + 2));
            }
        }
        return out;
    }
    [[nodiscard]] bool contains(std::string_view text) const {
        for (const auto& l : lines) {
            if (l.find(text) != std::string::npos) return true;
        }
        return false;
    }

    std::vector<std::string> lines;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

private:
    int m_handle = 0;
    log::Level m_previous = log::Level::Info;
};

bool contains(const std::string& haystack, std::string_view needle) { return haystack.find(needle) != std::string::npos; }

// Dereferences a component pointer, failing the test cleanly if it is missing.
template <class T>
T& need(T* component) {
    if (component == nullptr) throw std::runtime_error("expected component is missing");
    return *component;
}

project::ProjectManifest testProject() {
    project::ProjectManifest m;
    m.name = "Script Tests";
    m.settings.tickRate = 60;
    m.input.actions = {{"move_left", {Key::Left}}, {"move_right", {Key::Right}}, {"jump", {Key::Space}}};
    return m;
}

// Builds a scene and a script library in memory.
struct TestGame {
    project::ProjectManifest project = testProject();
    std::shared_ptr<scripting::ScriptLibrary> library = std::make_shared<scripting::ScriptLibrary>();
    Scene scene;
    int nextScript = 1;

    assets::AssetId addScript(const std::string& name, std::string_view source) {
        const assets::AssetId id{Guid(0x5c000000u, static_cast<std::uint64_t>(nextScript++))};
        auto status = library->add(id, name, source);
        if (!status) throw std::runtime_error(status.error().message);
        return id;
    }

    Entity addEntity(const std::string& name, std::optional<assets::AssetId> script = std::nullopt,
                     std::vector<ScriptProperty> properties = {}) {
        const Entity e = scene.createEntity(name);
        scene.set(e, Transform{});
        if (script) scene.set(e, Scripts{{AttachedScript{*script, std::move(properties)}}});
        return e;
    }

    Result<std::unique_ptr<GameSession>> start(const SessionOptions& options = {}) {
        return GameSession::create(project, scene, library, options);
    }

    std::unique_ptr<GameSession> mustStart(const SessionOptions& options = {}) {
        auto s = start(options);
        if (!s) throw std::runtime_error(s.error().message);
        return std::move(s.value());
    }
};

void run(GameSession& s, int ticks, const InputFrame& frame = {}) {
    for (int i = 0; i < ticks; ++i) s.tick(frame);
}

// A host for running a ScriptSystem directly (used for the client realm).
struct FakeHost final : scripting::IScriptHost {
    int findAction(std::string_view name) const override { return name == "jump" ? 0 : -1; }
    std::vector<std::string> actionNames() const override { return {"jump"}; }
    bool actionHeld(int) const override { return false; }
    bool actionPressed(int) const override { return false; }
    void addScore(int amount) override { score_ += amount; }
    int score() const override { return score_; }
    void setObjective(std::string) override {}
    void completeLevel(std::string) override {}
    double timeSeconds() const override { return 0.0; }
    std::uint64_t tickCount() const override { return 0; }
    void requestDestroy(Entity) override { ++destroyRequests; }
    void scriptLog(log::Level, std::string_view, std::string_view message) override { logs.emplace_back(message); }
    void scriptError(const scripting::ScriptError& error) override { errors.push_back(error.describe()); }

    int score_ = 0;
    int destroyRequests = 0;
    std::vector<std::string> logs;
    std::vector<std::string> errors;
};

} // namespace

// --- Luau integration ------------------------------------------------------------------------

TEST_CASE("Scripts compile at load; syntax errors name the file and line") {
    scripting::ScriptLibrary library;
    const assets::AssetId ok{Guid(1, 1)};
    CHECK(library.add(ok, "scripts/ok.luau", "local x = 1\nreturn {}").ok());
    const auto bad = library.add(assets::AssetId{Guid(1, 2)}, "scripts/bad.luau", "local x = 1\nlocal y = \nreturn {}");
    REQUIRE_FALSE(bad.ok());
    CHECK(contains(bad.error().message, "scripts/bad.luau:3:"));
    CHECK_FALSE(library.add(ok, "scripts/ok.luau", "return {}").ok()); // duplicate id

    scripting::ScriptLimits small;
    small.maxSourceBytes = 10;
    CHECK(library.add(assets::AssetId{Guid(1, 3)}, "big.luau", "return {} -- too long", small).error().code ==
          ErrorCode::LimitExceeded);
}

// --- Sandbox ------------------------------------------------------------------------------------

TEST_CASE("Sandbox: operating-system, loading and introspection functions do not exist") {
    LogCapture capture;
    TestGame game;
    const auto script = game.addScript("scripts/probe.luau", R"lua(
        local forbidden = { "io", "os", "debug", "require", "package", "loadstring", "load", "dofile",
                            "loadfile", "getfenv", "setfenv", "collectgarbage", "gcinfo", "newproxy",
                            "module", "jit", "ffi", "process", "file" }
        local present = {}
        for _, name in forbidden do
            if _G[name] ~= nil then table.insert(present, name) end
        end
        print("present:" .. table.concat(present, ","))
        -- What creators do get:
        print(typeof(math.floor), typeof(string.format), typeof(table.insert), typeof(vector.create),
              typeof(buffer.create), typeof(utf8.char), typeof(bit32.band), typeof(coroutine.wrap))
        return {}
    )lua");
    game.addEntity("Probe", script);
    auto session = game.mustStart();
    const auto printed = capture.printed();
    REQUIRE(printed.size() == 2);
    CHECK(printed[0] == "present:");
    CHECK(printed[1] == "function\tfunction\tfunction\tfunction\tfunction\tfunction\tfunction\tfunction");
}

TEST_CASE("Sandbox: built-in and engine tables are read-only and scripts cannot see each other's globals") {
    LogCapture capture;
    TestGame game;
    const auto writer = game.addScript("scripts/writer.luau", R"lua(
        sharedSecret = 42 -- a global of this script only
        local results = {}
        local function try(name, fn)
            local ok, err = pcall(fn)
            table.insert(results, name .. "=" .. (ok and "allowed" or "blocked"))
        end
        try("math.floor", function() math.floor = nil end)
        try("string.len", function() string.len = function() return 0 end end)
        try("Game.addScore", function() Game.addScore = nil end)
        try("rawset(Game)", function() rawset(Game, "x", 1) end)
        try("string metatable", function() getmetatable("").__index = {} end)
        try("vector metatable", function() getmetatable(Vec2(1, 2)).__add = nil end)
        try("_G.print", function() _G.print = nil end)
        print(table.concat(results, " "))
        return {}
    )lua");
    const auto reader = game.addScript("scripts/reader.luau", R"lua(
        print("reader sees sharedSecret:", sharedSecret)
        return {}
    )lua");
    game.addEntity("Writer", writer);
    game.addEntity("Reader", reader);
    auto session = game.mustStart();
    const auto printed = capture.printed();
    REQUIRE(printed.size() == 2);
    CHECK(printed[0] == "math.floor=blocked string.len=blocked Game.addScore=blocked rawset(Game)=blocked "
                        "string metatable=blocked vector metatable=blocked _G.print=blocked");
    // `_G` is the shared, read-only global table; a plain global assignment
    // (sharedSecret = 42) goes to the writer's own environment only.
    CHECK(printed[1] == "reader sees sharedSecret:\tnil");
}

TEST_CASE("Sandbox: Entity handles cannot be tampered with") {
    LogCapture capture;
    TestGame game;
    const auto script = game.addScript("scripts/tamper.luau", R"lua(
        local Probe = {}
        function Probe:Start()
            print(getmetatable(self.entity))
            print(pcall(setmetatable, self.entity, {}))
            local ok, err = pcall(function() self.entity.secret = 1 end)
            print(ok, string.find(err, "cannot hold custom fields") ~= nil)
            print(typeof(self.entity), tostring(self.entity))
        end
        return Probe
    )lua");
    game.addEntity("Thing", script);
    auto session = game.mustStart();
    session->tick({});
    const auto printed = capture.printed();
    REQUIRE(printed.size() == 4);
    CHECK(printed[0] == "Entity");
    CHECK(printed[1].starts_with("false"));
    CHECK(printed[2] == "false\ttrue");
    CHECK(printed[3] == "Entity\tEntity(Thing)");
}

// --- Resource limits -------------------------------------------------------------------------

TEST_CASE("CPU budget stops endless loops; other scripts and the game keep running") {
    LogCapture capture;
    TestGame game;
    const auto looping = game.addScript("scripts/loop.luau", R"lua(
        local Loop = {}
        function Loop:Tick(dt)
            while true do end
        end
        return Loop
    )lua");
    const auto sneaky = game.addScript("scripts/sneaky.luau", R"lua(
        local Sneaky = {}
        function Sneaky:Tick(dt)
            -- Catching the budget error does not help: every later safepoint fails too.
            while true do pcall(function() while true do end end) end
        end
        return Sneaky
    )lua");
    const auto counter = game.addScript("scripts/counter.luau", R"lua(
        local Counter = {}
        function Counter:Start() self.n = 0 end
        function Counter:Tick(dt) self.n += 1; print("tick", self.n) end
        return Counter
    )lua");
    game.addEntity("Looper", looping);
    game.addEntity("Sneaky", sneaky);
    game.addEntity("Counter", counter);
    SessionOptions options;
    options.scriptLimits.stepsPerCallback = 200'000;
    auto session = game.mustStart(options);

    const auto begin = std::chrono::steady_clock::now();
    run(*session, 3);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();

    CHECK(session->state().scriptErrors == 2); // each failing instance reports once, then is disabled
    CHECK(contains(session->state().lastScriptError, "CPU budget"));
    CHECK(capture.contains("scripts/loop.luau:4:")); // the error points at the loop
    const auto printed = capture.printed();
    REQUIRE(printed.size() == 3);
    CHECK(printed[2] == "tick\t3");
    const auto stats = session->scripts().stats();
    CHECK(stats.disabledInstances == 2);
    CHECK(seconds < 5.0);
}

TEST_CASE("Wall-clock watchdog stops slow callbacks") {
    LogCapture capture;
    TestGame game;
    const auto slow = game.addScript("scripts/slow.luau", R"lua(
        local Slow = {}
        function Slow:Tick(dt) while true do end end
        return Slow
    )lua");
    game.addEntity("Slow", slow);
    SessionOptions options;
    options.scriptLimits.stepsPerCallback = 1'000'000'000'000; // effectively unlimited steps
    options.scriptLimits.secondsPerCallback = 0.05;
    auto session = game.mustStart(options);
    const auto begin = std::chrono::steady_clock::now();
    session->tick({});
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    CHECK(session->state().scriptErrors == 1);
    CHECK(contains(session->state().lastScriptError, "ran longer than 50 ms"));
    CHECK(seconds < 2.0);
}

TEST_CASE("Memory limit stops runaway allocation and the VM recovers") {
    LogCapture capture;
    TestGame game;
    const auto hog = game.addScript("scripts/hog.luau", R"lua(
        local Hog = {}
        function Hog:Tick(dt)
            local t = {}
            for i = 1, 100000000 do t[i] = { i } end
        end
        return Hog
    )lua");
    const auto bigString = game.addScript("scripts/bigstring.luau", R"lua(
        local Big = {}
        function Big:Tick(dt) local s = string.rep("x", 1e9) end
        return Big
    )lua");
    const auto modest = game.addScript("scripts/modest.luau", R"lua(
        local Modest = {}
        function Modest:Tick(dt)
            local t = {}
            for i = 1, 1000 do t[i] = { i } end
            self.total = (self.total or 0) + #t
        end
        function Modest:Destroy() end
        return Modest
    )lua");
    game.addEntity("Hog", hog);
    game.addEntity("Big", bigString);
    const Entity modestEntity = game.addEntity("Modest", modest);
    SessionOptions options;
    options.scriptLimits.memoryBytes = 16u * 1024u * 1024u;
    options.scriptLimits.stepsPerCallback = 1'000'000'000; // let memory, not CPU, be the limit
    options.scriptLimits.secondsPerCallback = 10.0;
    auto session = game.mustStart(options);
    run(*session, 5);

    CHECK(session->state().scriptErrors == 2);
    CHECK(capture.contains("out of memory"));
    const auto stats = session->scripts().stats();
    CHECK(stats.disabledInstances == 2);
    CHECK(stats.memoryBytes < options.scriptLimits.memoryBytes);
    CHECK(session->scripts().vm().peakMemoryUsed() <= options.scriptLimits.memoryBytes);
    // The modest script kept working after the others failed.
    CHECK(session->scripts().hasScript(modestEntity));
}

TEST_CASE("Deep recursion is reported as an error, not a crash") {
    LogCapture capture;
    TestGame game;
    const auto recurse = game.addScript("scripts/recurse.luau", R"lua(
        local R = {}
        local function down(n) return down(n + 1) + 1 end
        function R:Start() down(1) end
        return R
    )lua");
    game.addEntity("Recurse", recurse);
    auto session = game.mustStart();
    session->tick({});
    CHECK(session->state().scriptErrors == 1);
    CHECK(contains(session->state().lastScriptError, "stack overflow"));
}

// --- Lifecycle ------------------------------------------------------------------------------

TEST_CASE("Lifecycle: Start once in creation order before any Tick; Tick every tick") {
    LogCapture capture;
    TestGame game;
    const auto script = game.addScript("scripts/order.luau", R"lua(
        local Order = { properties = { label = "?" } }
        function Order:Start()
            -- Every entity of the scene exists when Start runs.
            print("start " .. self.label .. " sees C:" .. tostring(World.find("C") ~= nil))
        end
        function Order:Tick(dt)
            print(string.format("tick %s %d dt=%.4f", self.label, Game.tickCount(), dt))
        end
        return Order
    )lua");
    for (const char* name : {"A", "B", "C"}) game.addEntity(name, script, {{"label", std::string(name)}});
    auto session = game.mustStart();
    run(*session, 2);
    const std::vector<std::string> expected = {
        "start A sees C:true", "start B sees C:true", "start C sees C:true", "tick A 0 dt=0.0167",
        "tick B 0 dt=0.0167",  "tick C 0 dt=0.0167",  "tick A 1 dt=0.0167", "tick B 1 dt=0.0167",
        "tick C 1 dt=0.0167"};
    CHECK(capture.printed() == expected);
}

TEST_CASE("Lifecycle: trigger enter/exit reach both entities; destroy is deferred and runs Destroy") {
    LogCapture capture;
    TestGame game;
    const auto zone = game.addScript("scripts/zone.luau", R"lua(
        local Zone = { properties = { destroyOnEnter = false } }
        function Zone:TriggerEnter(other)
            print("zone enter " .. other.name)
            if self.destroyOnEnter then
                self.entity:destroy()
                print("still valid after destroy(): " .. tostring(self.entity:isValid()))
            end
        end
        function Zone:TriggerExit(other) print("zone exit " .. other.name) end
        function Zone:Tick(dt) self.ticks = (self.ticks or 0) + 1 end
        function Zone:Destroy() print("zone destroyed at tick " .. Game.tickCount()) end
        return Zone
    )lua");
    const auto mover = game.addScript("scripts/mover.luau", R"lua(
        local Mover = {}
        function Mover:Start() self.entity.velocity = Vec2(120, 0) end
        function Mover:TriggerEnter(other) print("mover enter " .. other.name) end
        function Mover:TriggerExit(other) print("mover exit " .. other.name .. " valid=" .. tostring(other:isValid())) end
        return Mover
    )lua");
    const auto addZone = [&](const char* name, float x, bool destroy) {
        const Entity z = game.addEntity(name, zone, {{"destroyOnEnter", destroy}});
        need(game.scene.tryGet<Transform>(z)).position = {x, 0};
        game.scene.set(z, BoxCollider{{20, 20}, {}, true});
    };
    addZone("Passing Zone", 60, false);
    addZone("Exploding Zone", 200, true);
    const Entity body = game.addEntity("Body", mover);
    game.scene.set(body, BoxCollider{{10, 10}, {}, false});
    game.scene.set(body, KinematicBody{});

    auto session = game.mustStart();
    run(*session, 150);
    const std::vector<std::string> expected = {
        "zone enter Body",
        "mover enter Passing Zone",
        "zone exit Body",
        "mover exit Passing Zone valid=true",
        "zone enter Body",
        "still valid after destroy(): true",
        "mover enter Exploding Zone",
        "mover exit Exploding Zone valid=true",
        "zone destroyed at tick 92",
    };
    CHECK(capture.printed() == expected);
    CHECK(session->scene().findByName("Exploding Zone").isNull());
    CHECK(session->state().scriptErrors == 0);
}

TEST_CASE("Runtime errors disable only the failing instance and name file, line, entity and event") {
    LogCapture capture;
    TestGame game;
    const auto broken = game.addScript("scripts/broken.luau", R"lua(local Broken = {}
function Broken:Tick(dt)
    local nothing = nil
    nothing.field = 1
end
return Broken
)lua");
    const auto failsInStart = game.addScript("scripts/badstart.luau", R"lua(
        local S = {}
        function S:Start() error("setup failed") end
        function S:Tick(dt) print("never runs") end
        return S
    )lua");
    const auto healthy = game.addScript("scripts/healthy.luau", R"lua(
        local H = {}
        function H:Tick(dt) self.n = (self.n or 0) + 1 end
        function H:Destroy() end
        return H
    )lua");
    game.addEntity("Broken One", broken);
    game.addEntity("Broken Two", broken);
    game.addEntity("Bad Start", failsInStart);
    const Entity ok = game.addEntity("Healthy", healthy);
    auto session = game.mustStart();
    run(*session, 10);

    CHECK(session->state().scriptErrors == 3); // once per failing instance, not once per tick
    REQUIRE(capture.errors.size() == 3);
    CHECK(contains(capture.errors[0], "Script error in scripts/badstart.luau (entity 'Bad Start') during Start"));
    CHECK(contains(capture.errors[0], "setup failed"));
    CHECK(contains(capture.errors[1], "Script error in scripts/broken.luau (entity 'Broken One') during Tick"));
    CHECK(contains(capture.errors[1], "scripts/broken.luau:4: attempt to index nil"));
    CHECK(contains(capture.errors[2], "(entity 'Broken Two')"));
    CHECK_FALSE(capture.contains("never runs"));
    CHECK(session->scripts().hasScript(ok));
    CHECK(session->scripts().stats().disabledInstances == 3);
    bool sawEvent = false;
    // The session reports errors as game events too (shown on screen by the player).
    auto again = game.mustStart();
    again->tick({});
    for (const auto& ev : again->events()) sawEvent |= ev.type == GameEvent::Type::ScriptError;
    CHECK(sawEvent);
}

TEST_CASE("Invalid scripts and script components are rejected at load with clear messages") {
    LogCapture capture;
    const auto loadError = [](std::string_view source, std::vector<ScriptProperty> props = {}) {
        TestGame game;
        const auto script = game.addScript("scripts/thing.luau", source);
        game.addEntity("Thing", script, std::move(props));
        auto session = game.start();
        return session.ok() ? std::string() : session.error().message;
    };
    CHECK(contains(loadError("return 42"), "must end with `return <behaviour table>`, but it returned number"));
    CHECK(contains(loadError("local x = nil\nreturn x.y"), "scripts/thing.luau:2: attempt to index nil"));
    CHECK(contains(loadError("return { Tick = 5 }"), "'Tick' must be a function"));
    CHECK(contains(loadError("return { properties = { speed = {} } }"), "property 'speed' must default to"));
    CHECK(contains(loadError("return { properties = { entity = 1 } }"), "'entity' is reserved"));
    CHECK(contains(loadError("return { properties = { speed = 1 } }", {{"sped", 2.0}}),
                   "Entity 'Thing': scripts/thing.luau has no property 'sped' (did you mean 'speed'?)"));
    CHECK(contains(loadError("return { properties = { speed = 1 } }", {{"speed", std::string("fast")}}),
                   "property 'speed' of scripts/thing.luau must be a number, not a string"));
    CHECK(contains(loadError("while true do end\nreturn {}"), "CPU budget"));
    CHECK(loadError("return { properties = { speed = 1, label = \"x\", on = true, dir = Vec2(1, 0) } }",
                    {{"speed", 3.0}, {"label", std::string("y")}, {"on", false}, {"dir", Vec2{0, 1}}})
              .empty());

    // A Script that refers to a script asset that is not in the library.
    TestGame game;
    game.addEntity("Orphan", assets::AssetId{Guid(9, 9)});
    auto missing = game.start();
    REQUIRE_FALSE(missing.ok());
    CHECK(contains(missing.error().message, "Entity 'Orphan': Scripts refers to"));

    // A misspelled handler is a warning, not an error: it would otherwise silently never run.
    TestGame typo;
    const auto script = typo.addScript("scripts/typo.luau", "local T = {}\nfunction T:Tik(dt) end\nreturn T");
    typo.addEntity("Typo", script);
    CHECK(typo.start().ok());
    CHECK(capture.contains("function 'Tik' is not a lifecycle event (did you mean 'Tick'?)"));
}

// --- API ---------------------------------------------------------------------------------------

TEST_CASE("Entity API reads and writes components; destroyed entities are detected") {
    LogCapture capture;
    TestGame game;
    const auto script = game.addScript("scripts/entity.luau", R"lua(
        local E = {}
        function E:Start()
            local other = World.find("Other")
            print(self.entity.name, #self.entity.id, other.name)
            self.entity.position = Vec2(10, 20)
            self.entity.rotation = 45
            self.entity.scale = Vec2(2, 3)
            print(self.entity.position.x, self.entity.position.y, self.entity.rotation, self.entity.scale.y)
            print(self.entity:hasTag("hero"), self.entity:hasTag("villain"), self.entity:hasComponent("Script"),
                  other:hasComponent("Sprite"))
            print(pcall(function() return other.velocity end))
            print(pcall(function() return self.entity:hasComponent("Sprit") end))
            print(pcall(function() return self.entity.posiiton end))
            print(pcall(function() self.entity.id = "x" end))
            print(World.findById(other.id) == other, World.findById("not-an-id"))
            other:destroy()
            print("other valid until the phase ends:", other:isValid())
        end
        function E:Tick(dt)
            local other = World.find("Other")
            print("found after destroy:", other)
            if self.kept == nil then self.kept = self.entity end
            print(self.kept == self.entity)
        end
        return E
    )lua");
    const Entity e = game.addEntity("Hero", script);
    game.scene.set(e, Tags{{"hero"}});
    game.addEntity("Other");
    auto session = game.mustStart();
    session->tick({});

    const auto p = capture.printed();
    REQUIRE(p.size() == 11);
    CHECK(p[0] == "Hero\t36\tOther");
    CHECK(p[1] == "10\t20\t45\t3");
    CHECK(p[2] == "true\tfalse\ttrue\tfalse");
    CHECK(contains(p[3], "entity 'Other' has no KinematicBody component"));
    CHECK(contains(p[4], "unknown component 'Sprit' (did you mean 'Sprite'?)"));
    CHECK(contains(p[5], "Entity has no member 'posiiton' (did you mean 'position'?)"));
    CHECK(contains(p[6], "Entity.id is read-only"));
    CHECK(p[7] == "true\tnil");
    CHECK(p[8] == "other valid until the phase ends:\ttrue");
    CHECK(p[9] == "found after destroy:\tnil");
    CHECK(p[10] == "true");
    const auto* t = session->scene().tryGet<Transform>(e);
    REQUIRE(t != nullptr);
    CHECK(t->position == Vec2{10, 20});
    CHECK(t->rotation == doctest::Approx(45.0f));
}

TEST_CASE("Using a destroyed entity is an error except isValid()") {
    LogCapture capture;
    TestGame game;
    const auto script = game.addScript("scripts/holder.luau", R"lua(
        local H = {}
        function H:Start() self.target = World.find("Target"); self.target:destroy() end
        function H:Tick(dt)
            print(self.target:isValid(), tostring(self.target))
            print(pcall(function() return self.target.position end))
        end
        return H
    )lua");
    game.addEntity("Holder", script);
    game.addEntity("Target");
    auto session = game.mustStart();
    session->tick({});
    const auto p = capture.printed();
    REQUIRE(p.size() == 2);
    CHECK(p[0] == "false\tEntity(destroyed)");
    CHECK(contains(p[1], "this entity has been destroyed"));
}

TEST_CASE("Game, World and Input API") {
    LogCapture capture;
    TestGame game;
    const auto script = game.addScript("scripts/api.luau", R"lua(
        local A = {}
        function A:Start()
            Game.addScore(5)
            Game.addScore(-2)
            Game.setObjective("Find the key")
            assert(Game.getObjective() == "Find the key")
            local names = {}
            for _, e in World.findWithTag("pickup") do table.insert(names, e.name) end
            print(Game.getScore(), table.concat(names, ","))
            print(pcall(Game.addScore, 1.5))
            print(pcall(Input.isDown, "jmup"))
        end
        function A:Tick(dt)
            print(Game.tickCount(), Input.isDown("jump"), Input.wasPressed("jump"), Input.isDown("move_left"))
            if Game.tickCount() == 2 then Game.completeLevel("You win") end
        end
        return A
    )lua");
    game.addEntity("Rules", script);
    for (const char* name : {"Key", "Gem"}) {
        const Entity e = game.addEntity(name);
        game.scene.set(e, Tags{{"pickup"}});
    }
    auto session = game.mustStart();
    const ActionId jump = session->actions().find("jump");
    const InputFrame down = InputFrame::fromHeld(1u << jump, 0);
    const InputFrame held = InputFrame::fromHeld(1u << jump, 1u << jump);
    session->tick(down);
    session->tick(held);
    session->tick({});

    const auto p = capture.printed();
    REQUIRE(p.size() == 6);
    CHECK(p[0] == "3\tKey,Gem");
    CHECK(contains(p[1], "score amount must be a whole number"));
    CHECK(contains(p[2], "unknown input action 'jmup' (did you mean 'jump'?)"));
    CHECK(p[3] == "0\ttrue\ttrue\tfalse");
    CHECK(p[4] == "1\ttrue\tfalse\tfalse");
    CHECK(p[5] == "2\tfalse\tfalse\tfalse");
    CHECK(session->state().score == 3);
    CHECK(session->state().objective == "Find the key");
    CHECK(session->state().levelComplete);
    CHECK(session->state().completionMessage == "You win");
}

TEST_CASE("Events: emit, on and off; values must be serializable; storms are stopped") {
    LogCapture capture;
    TestGame game;
    const auto listener = game.addScript("scripts/listener.luau", R"lua(
        local L = {}
        function L:Start()
            self.id = Events.on("ping", function(value)
                print("got " .. typeof(value) .. " " .. tostring(value))
            end)
        end
        function L:Tick(dt)
            if Game.tickCount() == 1 then print("off:", Events.off(self.id), Events.off(self.id)) end
        end
        return L
    )lua");
    const auto sender = game.addScript("scripts/sender.luau", R"lua(
        local S = {}
        function S:Start()
            Events.emit("ping", 3)
            Events.emit("ping", "hello")
            Events.emit("ping", self.entity)
            Events.emit("ping")
            print(pcall(Events.emit, "ping", {}))
            print("emitted")
        end
        function S:Tick(dt)
            if Game.tickCount() == 2 then Events.emit("ping", 99) end
        end
        return S
    )lua");
    game.addEntity("Listener", listener);
    game.addEntity("Sender", sender);
    auto session = game.mustStart();
    run(*session, 3);
    const auto p = capture.printed();
    REQUIRE(p.size() == 7);
    CHECK(contains(p[0], "events must be serializable"));
    CHECK(p[1] == "emitted"); // delivery happens after the phase, not during emit
    CHECK(p[2] == "got number 3");
    CHECK(p[3] == "got string hello");
    CHECK(p[4] == "got Entity Entity(Sender)");
    CHECK(p[5] == "got nil nil");
    CHECK(p[6] == "off:\ttrue\tfalse"); // and the tick-2 event reaches nobody

    TestGame storm;
    const auto echo = storm.addScript("scripts/echo.luau", R"lua(
        Events.on("echo", function() Events.emit("echo") end)
        local E = {}
        function E:Start() Events.emit("echo") end
        return E
    )lua");
    storm.addEntity("Echo", echo);
    SessionOptions options;
    options.scriptLimits.maxEventDeliveriesPerTick = 100;
    auto stormSession = storm.mustStart(options);
    stormSession->tick({});
    CHECK(stormSession->state().scriptErrors >= 1);
    CHECK(contains(stormSession->state().lastScriptError, "more than 100 event deliveries"));
    stormSession->tick({}); // the game continues
}

TEST_CASE("print output is limited per tick") {
    LogCapture capture;
    TestGame game;
    const auto chatty = game.addScript("scripts/chatty.luau", R"lua(
        local C = {}
        function C:Tick(dt) for i = 1, 500 do print(i) end end
        return C
    )lua");
    game.addEntity("Chatty", chatty);
    auto session = game.mustStart();
    run(*session, 2);
    CHECK(capture.printed().size() == 2 * 100 + 2); // 100 lines + one "dropped" notice per tick
    CHECK(capture.contains("further output is dropped"));
}

TEST_CASE("Client realm: authority-only API is absent and entity state is read-only") {
    Scene scene;
    const Entity e = scene.createEntity("Thing");
    scene.set(e, Transform{{4, 5}});
    auto library = std::make_shared<scripting::ScriptLibrary>();
    const assets::AssetId id{Guid(7, 7)};
    REQUIRE(library
                ->add(id, "scripts/client.luau", R"lua(
        local C = {}
        function C:Start()
            print(Game.addScore == nil, Game.setObjective == nil, Game.completeLevel == nil, Game.getScore ~= nil)
            print(self.entity.position.x, self.entity.name)
            print(pcall(function() self.entity.position = Vec2(0, 0) end))
            print(pcall(function() self.entity:destroy() end))
        end
        return C
    )lua")
                .ok());
    scene.set(e, Scripts{{AttachedScript{id, {}}}});
    FakeHost host;
    scripting::ScriptSystemOptions options;
    options.realm = scripting::Realm::Client;
    auto system = scripting::ScriptSystem::create(scene, library, host, options);
    REQUIRE(system.ok());
    system.value()->start();
    REQUIRE(host.logs.size() == 4);
    CHECK(host.logs[0] == "true\ttrue\ttrue\ttrue");
    CHECK(host.logs[1] == "4\tThing");
    CHECK(contains(host.logs[2], "client scripts cannot change entity state"));
    CHECK(contains(host.logs[3], "only available to authority (server) scripts"));
    CHECK(host.destroyRequests == 0);
    CHECK(need(scene.tryGet<Transform>(e)).position == Vec2{4, 5});

    // The reference marks the same functions as authority-only.
    int authorityOnly = 0;
    for (const auto& m : scripting::apiReference()) {
        if (m.availability == scripting::ApiAvailability::AuthorityOnly) ++authorityOnly;
    }
    CHECK(authorityOnly == 6); // addScore, setObjective, completeLevel, destroy, setParent, respawn
}

TEST_CASE("math.random is deterministic per session seed") {
    const auto sequence = [](std::uint64_t seed) {
        LogCapture capture;
        TestGame game;
        const auto script = game.addScript("scripts/random.luau", R"lua(
            local R = {}
            function R:Start() print(math.random(1, 1000000), math.random(1, 1000000), math.random()) end
            return R
        )lua");
        game.addEntity("Random", script);
        SessionOptions options;
        options.seed = seed;
        auto session = game.mustStart(options);
        session->tick({});
        return capture.printed();
    };
    CHECK(sequence(7) == sequence(7));
    CHECK(sequence(7) != sequence(8));
}

TEST_CASE("A scripted moving platform carries a body standing on it") {
    LogCapture capture;
    TestGame game;
    const auto platformScript = game.addScript("scripts/lift.luau", R"lua(
        local Lift = {}
        function Lift:Start() self.origin = self.entity.position end
        function Lift:Tick(dt)
            -- 30 units right, then 20 units down, over 60 ticks
            local t = math.min(Game.tickCount(), 60)
            self.entity.position = self.origin + Vec2(t * 0.5, math.max(0, t - 20) * 0.5)
        end
        return Lift
    )lua");
    const Entity platform = game.addEntity("Lift", platformScript);
    need(game.scene.tryGet<Transform>(platform)).position = {100, 100};
    game.scene.set(platform, BoxCollider{{48, 16}, {}, false});
    const Entity rider = game.addEntity("Rider");
    need(game.scene.tryGet<Transform>(rider)).position = {100, 100 - 8 - 7}; // standing on top
    game.scene.set(rider, BoxCollider{{10, 14}, {}, false});
    game.scene.set(rider, KinematicBody{});
    game.scene.set(rider, PlatformerController{});
    auto session = game.mustStart();
    run(*session, 5); // settle: the rider lands on the platform
    const Vec2 before = need(session->scene().tryGet<Transform>(rider)).position;
    run(*session, 80);
    const Vec2 after = need(session->scene().tryGet<Transform>(rider)).position;
    const Vec2 platformNow = need(session->scene().tryGet<Transform>(platform)).position;
    CHECK(platformNow == Vec2{130, 120});
    CHECK(after.x - before.x == doctest::Approx(30.0f - (before.x - 100.0f)).epsilon(0.05));
    CHECK(after.y == doctest::Approx(platformNow.y - 8 - 7).epsilon(0.001));
    CHECK(need(session->scene().tryGet<KinematicBody>(rider)).onGround);
}

// --- Multiple behaviours and hierarchy ------------------------------------------------------

TEST_CASE("Several scripts on one entity run as separate instances, in order") {
    LogCapture capture;
    TestGame game;
    const auto a = game.addScript("scripts/a.luau", R"lua(
        local A = { properties = { n = 1 } }
        function A:Start() print("A start", self.n) end
        function A:TriggerEnter(other) print("A enter", other.name) end
        function A:Destroy() print("A destroy") end
        return A
    )lua");
    const auto b = game.addScript("scripts/b.luau", R"lua(
        local B = { properties = { n = 1 } }
        function B:Start() print("B start", self.n) end
        function B:Tick() if Game.tickCount() == 2 then self.entity:destroy() end end
        function B:TriggerEnter(other) print("B enter", other.name) end
        function B:Destroy() print("B destroy") end
        return B
    )lua");
    const Entity e = game.addEntity("Both");
    game.scene.set(e, Scripts{{AttachedScript{a, {{"n", 7.0}}}, AttachedScript{b, {}}}});
    game.scene.set(e, BoxCollider{{16, 16}, {}, true});
    const Entity body = game.addEntity("Body");
    game.scene.set(body, BoxCollider{{8, 8}});
    game.scene.set(body, KinematicBody{});

    auto session = game.mustStart();
    CHECK(session->scripts().stats().instances == 2);
    run(*session, 4);
    CHECK(capture.printed() == std::vector<std::string>{"A start\t7", "B start\t1", "A enter\tBody", "B enter\tBody",
                                                        "A destroy", "B destroy"});
    CHECK(session->scripts().stats().instances == 0);
    CHECK(session->state().scriptErrors == 0);
}

TEST_CASE("Hazards and checkpoints: respawn and spawnPoint") {
    LogCapture capture;
    TestGame game;
    // A checkpoint moves the spawn point; a hazard sends the player back to it.
    const auto checkpoint = game.addScript("scripts/checkpoint.luau", R"lua(
        local C = {}
        function C:TriggerEnter(other)
            if other:hasComponent("Player") then other.spawnPoint = self.entity.position end
        end
        return C
    )lua");
    const auto hazard = game.addScript("scripts/hazard.luau", R"lua(
        local H = {}
        function H:TriggerEnter(other)
            if other:hasComponent("Player") then other:respawn() end
        end
        function H:Start()
            print(pcall(function() self.entity:respawn() end))
        end
        return H
    )lua");
    const Entity cp = game.addEntity("Checkpoint", checkpoint);
    game.scene.get<Transform>(cp).position = {40, 0};
    game.scene.set(cp, BoxCollider{{8, 8}, {}, true});
    const Entity spikes = game.addEntity("Spikes", hazard);
    game.scene.get<Transform>(spikes).position = {80, 0};
    game.scene.set(spikes, BoxCollider{{8, 8}, {}, true});
    const Entity player = game.addEntity("Player");
    game.scene.set(player, BoxCollider{{8, 8}});
    game.scene.set(player, KinematicBody{});
    game.scene.set(player, Player{});

    auto session = game.mustStart();
    Scene& scene = session->scene();
    run(*session, 1);
    CHECK(contains(capture.printed().at(0), "only players respawn"));
    scene.get<KinematicBody>(player).velocity = {600, 0}; // 10 units per tick, no controller to slow it
    run(*session, 6);                                       // passes the checkpoint at x = 40
    CHECK(scene.get<Player>(player).spawnPoint == Vec2{40, 0});
    run(*session, 4); // reaches the spikes at x = 80
    CHECK(session->state().respawns == 1);
    CHECK(scene.worldPosition(player) == Vec2{40, 0});
    CHECK(scene.get<KinematicBody>(player).velocity == Vec2{});
    CHECK(session->state().scriptErrors == 0);
}

TEST_CASE("Scripts see the hierarchy: world and local positions, parents, children") {
    LogCapture capture;
    TestGame game;
    const auto probe = game.addScript("scripts/probe.luau", R"lua(
        local P = {}
        function P:Start()
            local e = self.entity
            local parent = World.find("Parent")
            print(e.parent == parent, e.position.x, e.localPosition.x, #parent:getChildren())
            e.position = Vec2(300, 0)
            print(e.localPosition.x)
            e:setParent(nil)
            print(e.parent == nil, e.position.x, e.localPosition.x)
            print(pcall(function() parent:setParent(e) e:setParent(parent) parent:setParent(e) end))
            print(e.rotation, e.localRotation)
        end
        return P
    )lua");
    const auto destroyer = game.addScript("scripts/destroyer.luau", R"lua(
        local D = {}
        function D:Tick() if Game.tickCount() == 1 then self.entity:destroy() end end
        function D:Destroy() print("destroy", self.entity.name) end
        return D
    )lua");
    const Entity parent = game.addEntity("Parent");
    game.scene.set(parent, Transform{{100, 0}});
    const Entity child = game.scene.createEntity("Child", parent);
    game.scene.set(child, Transform{{20, 0}});
    game.scene.set(child, Scripts{{AttachedScript{probe, {}}}});

    const Entity doomed = game.addEntity("Doomed", destroyer);
    const Entity kid = game.scene.createEntity("Kid", doomed);
    game.scene.set(kid, Scripts{{AttachedScript{destroyer, {}}}});

    auto session = game.mustStart();
    run(*session, 3);
    const auto printed = capture.printed();
    REQUIRE(printed.size() >= 7);
    CHECK(printed[0] == "true\t120\t20\t1");
    CHECK(printed[1] == "200");
    CHECK(printed[2] == "true\t300\t300");
    CHECK(contains(printed[3], "false"));
    CHECK(contains(printed[3], "cannot become a child of itself or of its own child"));
    CHECK(printed[4] == "0\t0");
    // Destroying a parent destroys its children; both get Destroy, parent first.
    CHECK(printed[5] == "destroy\tDoomed");
    CHECK(printed[6] == "destroy\tKid");
    CHECK(session->scene().findByName("Kid").isNull());
    CHECK(session->state().scriptErrors == 0);
}

TEST_CASE("Physics, triggers and rendering use world positions of children") {
    TestGame game;
    const auto counter = game.addScript("scripts/counter.luau", R"lua(
        local C = {}
        function C:TriggerEnter() Game.addScore(1) end
        return C
    )lua");
    // A trigger that is a child of a moved group: it must be found at its world position.
    const Entity group = game.addEntity("Group");
    game.scene.set(group, Transform{{200, 0}});
    const Entity trigger = game.scene.createEntity("Trigger", group);
    game.scene.set(trigger, Transform{{0, 0}});
    game.scene.set(trigger, BoxCollider{{16, 16}, {}, true});
    game.scene.set(trigger, Scripts{{AttachedScript{counter, {}}}});
    // A falling body that lands on a solid box that is a child of the group.
    const Entity floor = game.scene.createEntity("Floor", group);
    game.scene.set(floor, Transform{{0, 100}});
    game.scene.set(floor, BoxCollider{{64, 16}});
    const Entity body = game.addEntity("Body");
    game.scene.set(body, Transform{{200, 0}});
    game.scene.set(body, BoxCollider{{8, 8}});
    KinematicBody falling;
    falling.velocity = {0, 200};
    game.scene.set(body, falling);

    auto session = game.mustStart();
    run(*session, 120);
    CHECK(session->state().score == 1); // overlapped the trigger at world (200, 0)
    const Vec2 p = session->scene().worldPosition(body);
    CHECK(p.y == doctest::Approx(100 - 8 - 4)); // stopped by the floor at world y 100
}

// --- Formats and documentation ------------------------------------------------------------

TEST_CASE("Scene format 1 Collectible components migrate to tags with a warning") {
    const std::string v1 = R"lua({
        "format": "px.scene", "formatVersion": 1, "name": "Old",
        "entities": [ { "id": "cccccccc-0000-4000-8000-000000000001", "name": "Coin",
                        "components": { "Collectible": { "kind": "gem", "value": 5 } } } ] })lua";
    std::vector<std::string> warnings;
    auto scene = project::parseScene(v1, "old.scene.json", nullptr, &warnings);
    REQUIRE_MESSAGE(scene.ok(), (scene.ok() ? "" : scene.error().message));
    const Entity coin = scene->findByName("Coin");
    const auto* tags = scene->tryGet<Tags>(coin);
    REQUIRE(tags != nullptr);
    CHECK((tags != nullptr && tags->has("gem")));
    REQUIRE(warnings.size() == 1);
    CHECK(contains(warnings[0], "old.scene.json: scene format 1 -> 2: 1 Collectible component(s) were converted"));
    // Saving writes the current version.
    CHECK(contains(project::serializeScene(scene.value()), "\"formatVersion\": 3"));
}

TEST_CASE("Projects requiring a newer scripting API are refused") {
    const std::string manifest = R"lua({
        "format": "px.project", "formatVersion": 1, "id": "aaaaaaaa-0000-4000-8000-0000000000ff",
        "name": "Future", "startScene": "aaaaaaaa-0000-4000-8000-000000000002",
        "settings": { "scripting": { "apiVersion": 2 } },
        "assets": [ { "id": "aaaaaaaa-0000-4000-8000-000000000002", "type": "scene", "path": "s.scene.json" } ] })lua";
    auto parsed = project::parseProjectManifest(manifest, "project.json");
    REQUIRE_FALSE(parsed.ok());
    CHECK(contains(parsed.error().message, "needs scripting API version 2, but this engine provides version 1"));
}

TEST_CASE("Script files are loaded from the project and changes take effect without recompiling") {
    MemoryFileSystem files;
    const std::string sceneId = "aaaaaaaa-0000-4000-8000-000000000002";
    const std::string scriptId = "aaaaaaaa-0000-4000-8000-000000000003";
    const std::string manifest =
        "{ \"format\": \"px.project\", \"formatVersion\": 1, \"id\": \"aaaaaaaa-0000-4000-8000-0000000000ff\", "
        "\"name\": \"Hot\", \"startScene\": \"" + sceneId + "\", \"assets\": [ "
        "{ \"id\": \"" + sceneId + "\", \"type\": \"scene\", \"path\": \"s.scene.json\" }, "
        "{ \"id\": \"" + scriptId + "\", \"type\": \"script\", \"path\": \"scripts/score.luau\" } ] }";
    const std::string sceneJson =
        "{ \"format\": \"px.scene\", \"formatVersion\": 2, \"entities\": [ "
        "{ \"id\": \"cccccccc-0000-4000-8000-000000000001\", \"name\": \"Scorer\", \"components\": "
        "{ \"Script\": { \"script\": \"" + scriptId + "\", \"properties\": { \"points\": 2 } } } } ] }";
    files.addFile("project.json", manifest);
    files.addFile("s.scene.json", sceneJson);
    const auto scoreAfterOneTick = [&](std::string_view source) {
        files.addFile("scripts/score.luau", source);
        auto game = loadGame(files);
        REQUIRE_MESSAGE(game.ok(), (game.ok() ? "" : game.error().message));
        auto session = startSession(game.value());
        REQUIRE(session.ok());
        session.value()->tick({});
        return session.value()->state().score;
    };
    CHECK(scoreAfterOneTick("return { properties = { points = 1 }, Start = function(self) Game.addScore(self.points) end }") == 2);
    CHECK(scoreAfterOneTick("return { properties = { points = 1 }, Start = function(self) Game.addScore(self.points * 10) end }") == 20);

    files.addFile("scripts/score.luau", "return {");
    auto broken = loadGame(files);
    REQUIRE_FALSE(broken.ok());
    CHECK(contains(broken.error().message, "scripts/score.luau:1:"));
}

TEST_CASE("API reference document is up to date") {
    // Regenerate with: px-headless --print-api-reference > docs/scripting-api.md
    DirectoryFileSystem docs(PX_DOCS_DIR);
    auto text = docs.readText("scripting-api.md");
    REQUIRE_MESSAGE(text.ok(), "docs/scripting-api.md is missing");
    std::string onDisk = text.value();
    std::erase(onDisk, '\r'); // git may check text files out with CRLF line endings (Windows)
    const bool upToDate = onDisk == scripting::generateApiReferenceMarkdown();
    CHECK_MESSAGE(upToDate, "docs/scripting-api.md is out of date; regenerate it with px-headless --print-api-reference");

    // Every member is uniquely named within its group and documented.
    std::set<std::string> seen;
    for (const auto& m : scripting::apiReference()) {
        CHECK(seen.insert(std::string(m.group) + "." + std::string(m.name)).second);
        CHECK_FALSE(m.description.empty());
        CHECK_FALSE(m.signature.empty());
    }
}

TEST_CASE("Performance: scripted Tick cost stays small") {
    TestGame game;
    const auto spinner = game.addScript("scripts/spinner.luau", R"lua(
        local S = { properties = { speed = 90 } }
        function S:Tick(dt)
            self.entity.rotation = self.entity.rotation + self.speed * dt
            self.entity.position = self.entity.position + Vec2(0.1, 0)
        end
        return S
    )lua");
    const auto idle = game.addScript("scripts/idle.luau", "return { TriggerEnter = function() end }");
    constexpr int kTicking = 1000;
    constexpr int kIdle = 5000;
    for (int i = 0; i < kTicking; ++i) game.addEntity("Spinner", spinner);
    for (int i = 0; i < kIdle; ++i) game.addEntity("Idle", idle);
    auto session = game.mustStart();
    session->tick({}); // Start
    constexpr int kTicks = 60;
    const auto begin = std::chrono::steady_clock::now();
    run(*session, kTicks);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() / kTicks;
    const auto stats = session->scripts().stats();
    MESSAGE("1000 Tick handlers + 5000 idle scripts: " << ms << " ms per tick, script memory "
                                                       << stats.memoryBytes / 1024 << " KiB");
    CHECK(stats.tickHandlers == kTicking);
    CHECK(stats.instances == kTicking + kIdle);
    // Generous bound so slow CI and sanitizer builds pass; catches pathological regressions.
    CHECK(ms < 100.0);
}
