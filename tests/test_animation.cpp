// Animation clips: the file format, Animators choosing clips from movement
// and scripts, and the editor's clip tools.

#include <doctest.h>

#include "px/core/FileSystem.hpp"
#include "px/core/Log.hpp"
#include "px/editor/EditorProject.hpp"
#include "px/editor/Library.hpp"
#include "px/editor/SceneDocument.hpp"
#include "px/project/Animation.hpp"
#include "px/runtime/GameLoader.hpp"
#include "px/runtime/GameSession.hpp"
#include "px/scene/ComponentInfo.hpp"
#include "px/scripting/ScriptLibrary.hpp"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace px;
using namespace px::scene;
using namespace px::project;

namespace {

template <class T>
T must(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result.value());
}
template <class T>
T& need(T* component) {
    if (component == nullptr) throw std::runtime_error("expected component is missing");
    return *component;
}
bool contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

const assets::AssetId kSheet{Guid(0x7e000000u, 1)};

assets::AssetId clipId(std::uint64_t n) { return assets::AssetId{Guid(0xa0000000u, n)}; }

AnimationClip strip(int first, int count, float fps, bool loop) {
    AnimationClip clip;
    clip.texture = kSheet;
    for (int i = 0; i < count; ++i) clip.frames.push_back({static_cast<float>((first + i) * 16), 0, 16, 16});
    clip.framesPerSecond = fps;
    clip.loop = loop;
    return clip;
}

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
    std::vector<std::string> lines;

private:
    int m_handle = 0;
    log::Level m_previous = log::Level::Info;
};

// A character with idle (frames 0-1), run (2-5), jump (6), fall (7) and an
// "attack" clip (8-9, once) for scripts.
struct AnimatedGame {
    ProjectManifest project;
    std::shared_ptr<scripting::ScriptLibrary> scripts = std::make_shared<scripting::ScriptLibrary>();
    std::shared_ptr<AnimationLibrary> clips = std::make_shared<AnimationLibrary>();
    Scene scene;
    Entity hero;

    AnimatedGame() {
        project.name = "Anim";
        project.input.actions = {{"move_left", {Key::Left}}, {"move_right", {Key::Right}}, {"jump", {Key::Space}}};
        clips->add({clipId(1), "Idle", strip(0, 2, 2, true)});
        clips->add({clipId(2), "Run", strip(2, 4, 10, true)});
        clips->add({clipId(3), "Jump", strip(6, 1, 1, true)});
        clips->add({clipId(4), "Fall", strip(7, 1, 1, true)});
        clips->add({clipId(5), "Attack", strip(8, 2, 10, false)});
        const Entity floor = scene.createEntity("Floor");
        scene.set(floor, Transform{{0, 40}});
        scene.set(floor, BoxCollider{{400, 16}, {}, false});
        hero = scene.createEntity("Hero");
        scene.set(hero, Transform{{0, 20}});
        scene.set(hero, Sprite{});
        scene.set(hero, BoxCollider{{10, 14}, {}, false});
        scene.set(hero, KinematicBody{});
        scene.set(hero, PlatformerController{});
        Animator a;
        a.idle = clipId(1);
        a.run = clipId(2);
        a.jump = clipId(3);
        a.fall = clipId(4);
        scene.set(hero, a);
    }
    std::unique_ptr<runtime::GameSession> start() {
        runtime::SessionOptions options;
        options.animations = clips;
        return must(runtime::GameSession::create(project, scene, scripts, options));
    }
};

runtime::InputFrame keys(const runtime::GameSession& s, std::initializer_list<std::string_view> names, bool press) {
    std::uint32_t bits = 0;
    for (auto n : names) bits |= 1u << s.actions().find(n);
    return {bits, press ? bits : 0u};
}

float frameX(const runtime::GameSession& s, Entity e) { return need(s.scene().tryGet<Sprite>(e)).source.x; }

} // namespace

TEST_CASE("Animation: clip files round-trip and refuse bad data") {
    const AnimationClip clip = strip(2, 4, 10, false);
    const std::string text = serializeAnimation(clip);
    const AnimationClip back = must(parseAnimation(text, "run.anim.json"));
    CHECK(serializeAnimation(back) == text);
    CHECK(back.frames.size() == 4);
    CHECK_FALSE(back.loop);

    const auto error = [](std::string_view json, const assets::AssetRegistry* registry = nullptr) {
        auto r = parseAnimation(json, "bad.anim.json", registry);
        return r ? std::string() : r.error().message;
    };
    const std::string head = R"({"format": "px.animation", "formatVersion": 1, )";
    CHECK(contains(error(head + R"("frames": []})"), "texture"));
    CHECK(contains(error(head + R"("texture": "7e000000-0000-0000-0000-000000000001", "frames": [[0, 0, 0, 16]]})"),
                   "must be positive"));
    CHECK(contains(error(head + R"("texture": "7e000000-0000-0000-0000-000000000001", "frames": [],
                         "framesPerSecond": 0})"),
                   "greater than 0"));
    CHECK(contains(error(head + R"("texture": "7e000000-0000-0000-0000-000000000001", "frames": [], "speed": 2})"),
                   "unknown field 'speed'"));
    assets::AssetRegistry registry;
    REQUIRE(registry.add({kSheet, assets::AssetType::Script, "s", "s.luau", assets::TextureFilter::Nearest}).ok());
    CHECK(contains(error(head + "\"texture\": \"" + kSheet.toString() + "\", \"frames\": []}", &registry),
                   "not a texture"));

    // Timing.
    AnimationClip once = strip(0, 4, 4, false);
    CHECK(once.duration() == doctest::Approx(1.0));
    CHECK(once.frameAt(0.0f) == 0);
    CHECK(once.frameAt(0.6f) == 2);
    CHECK(once.frameAt(5.0f) == 3); // holds the last frame
    CHECK_FALSE(once.finishedAt(0.9f));
    CHECK(once.finishedAt(1.0f));
    once.loop = true;
    CHECK(once.frameAt(1.3f) == 1);
    CHECK_FALSE(once.finishedAt(10.0f));
    CHECK(gridCells({64, 32}, {16, 16}).size() == 8);
}

TEST_CASE("Animation: an Animator follows the character's movement") {
    AnimatedGame game;
    auto s = game.start();
    for (int i = 0; i < 30; ++i) s->tick({}); // lands, stands
    CHECK(s->scene().get<Animator>(game.hero).current == clipId(1));
    CHECK(frameX(*s, game.hero) < 32.0f); // an idle frame
    CHECK(need(s->scene().tryGet<Sprite>(game.hero)).texture == kSheet);

    for (int i = 0; i < 20; ++i) s->tick(keys(*s, {"move_right"}, i == 0));
    CHECK(s->scene().get<Animator>(game.hero).current == clipId(2));
    const float x = frameX(*s, game.hero);
    CHECK(x >= 32.0f);
    CHECK(x < 96.0f);

    s->tick(keys(*s, {"move_right", "jump"}, true));
    s->tick(keys(*s, {"move_right", "jump"}, false));
    CHECK(s->scene().get<Animator>(game.hero).current == clipId(3)); // going up
    for (int i = 0; i < 40 && s->scene().get<KinematicBody>(game.hero).velocity.y <= 0.0f; ++i) s->tick({});
    s->tick({});
    CHECK(s->scene().get<Animator>(game.hero).current == clipId(4)); // coming down
    CHECK(frameX(*s, game.hero) == 112.0f);
}

TEST_CASE("Animation: scripts play clips; one-shot clips hand back to the automatic choice") {
    Logs logs;
    AnimatedGame game;
    const assets::AssetId script{Guid(0x5c000000u, 1)};
    REQUIRE(game.scripts->add(script, "attacker", R"(
        local A = {}
        function A:Tick()
            local t = Game.tickCount()
            if t == 30 then
                self.entity:playAnimation("Attack")
                print("now " .. self.entity.animation)
            elseif t == 31 then
                print(select(2, pcall(function() self.entity:playAnimation("Atack") end)))
            elseif t == 60 then
                self.entity:playAnimation("Run")
            elseif t == 70 then
                self.entity:playAnimation(nil)
            end
        end
        return A)").ok());
    game.scene.set(game.hero, Scripts{{AttachedScript{script, {}}}});
    auto s = game.start();
    for (int i = 0; i < 31; ++i) s->tick({});
    CHECK(logs.has("now Attack"));
    CHECK(frameX(*s, game.hero) >= 128.0f); // an attack frame
    s->tick({});
    CHECK(logs.has("no animation named 'Atack' (did you mean 'Attack'?)"));
    for (int i = 0; i < 20; ++i) s->tick({}); // the attack (0.2 s) is over
    CHECK(s->scene().get<Animator>(game.hero).current == clipId(1));
    CHECK(s->scene().get<Animator>(game.hero).scripted.isNil());
    for (int i = 0; i < 10; ++i) s->tick({}); // tick 61: Run, looping, though standing
    CHECK(s->scene().get<Animator>(game.hero).current == clipId(2));
    for (int i = 0; i < 12; ++i) s->tick({}); // tick 71: back to automatic
    CHECK(s->scene().get<Animator>(game.hero).current == clipId(1));
    CHECK(s->state().scriptErrors == 0);
}

TEST_CASE("Animation: default clip, speed, pause, and no clips loaded") {
    AnimatedGame game;
    const Entity torch = game.scene.createEntity("Torch");
    game.scene.set(torch, Transform{});
    game.scene.set(torch, Sprite{});
    Animator a;
    a.clip = clipId(2); // 4 frames at 10 per second
    a.speed = 2.0f;
    game.scene.set(torch, a);
    auto s = game.start();
    for (int i = 0; i < 6; ++i) s->tick({}); // 0.1 s at double speed: frame 2
    CHECK(frameX(*s, torch) == 64.0f);
    s->scene().get<Animator>(torch).playing = false;
    for (int i = 0; i < 30; ++i) s->tick({});
    CHECK(frameX(*s, torch) == 64.0f);

    // Without clips (a session given none), Animators leave the Sprite alone.
    runtime::SessionOptions bare;
    auto plain = must(runtime::GameSession::create(game.project, game.scene, game.scripts, bare));
    plain->tick({});
    CHECK(need(plain->scene().tryGet<Sprite>(torch)).texture.isNil());
}

TEST_CASE("Animation: the editor creates clips from sprite strips; the library brings clips along") {
    const auto folder = std::filesystem::temp_directory_path() / ("px-anim-test-" + Guid::generate().toString());
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{folder};
    auto project = must(editor::EditorProject::create(folder / "game", "Anim Game"));
    const editor::Library library = must(editor::Library::load(std::filesystem::path(PX_LIBRARY_DIR)));
    const assets::AssetId sceneId = project->manifest().startScene;
    editor::SceneDocument doc(sceneId, must(project->loadScene(sceneId)));

    // The Toolbox's hero comes with its sprite sheet and clips.
    int heroIndex = -1;
    for (std::size_t i = 0; i < library.manifest().objects.size(); ++i) {
        if (library.manifest().objects[i].name == "Platformer Hero") heroIndex = static_cast<int>(i);
    }
    REQUIRE(heroIndex >= 0);
    const auto inserted = must(editor::insertLibraryObject(*project, doc, library, static_cast<std::size_t>(heroIndex),
                                                           {40, 40}));
    REQUIRE(inserted.size() == 1);
    const auto& animator = need(doc.scene().tryGet<Animator>(inserted[0]));
    const auto* idle = project->asset(animator.idle);
    REQUIRE(idle != nullptr);
    CHECK(idle->type == assets::AssetType::Animation);
    CHECK(idle->path == "animations/player_idle.anim.json");
    const AnimationClip idleClip = must(project->loadAnimation(animator.idle));
    CHECK(project->asset(idleClip.texture) != nullptr);
    REQUIRE(doc.save(*project).ok());

    // A clip from a sheet: the cells of the first row that are not empty.
    const AnimationClip run = must(project->clipFromSheet(idleClip.texture, {16, 16}));
    CHECK(run.frames.size() == 8);
    const auto runId = must(project->createAnimation("Run Fast", run));
    CHECK(project->asset(runId)->path == "animations/run_fast.anim.json");
    // The sheet is in use by the clips, so it cannot be deleted.
    const auto uses = project->findUses(idleClip.texture);
    CHECK(std::any_of(uses.begin(), uses.end(), [](const editor::AssetUse& u) {
        return u.where == assets::AssetType::Animation;
    }));
    CHECK_FALSE(project->deleteAsset(idleClip.texture).ok());

    // The game loads its clips and the hero animates.
    DirectoryFileSystem files(folder / "game");
    const auto game = must(runtime::loadGame(files));
    REQUIRE(game.animations != nullptr);
    CHECK(game.animations->size() == 5);
    auto session = must(runtime::startSession(game));
    for (int i = 0; i < 5; ++i) session->tick({});
    const Entity hero = session->player();
    REQUIRE_FALSE(hero.isNull());
    CHECK_FALSE(session->scene().get<Animator>(hero).current.isNil());
}

TEST_CASE("Animation: the examples' players animate while playing") {
    for (const char* example : {"platformer", "topdown"}) {
        CAPTURE(example);
        DirectoryFileSystem files(std::string(PX_EXAMPLES_DIR) + "/" + example);
        const auto game = must(runtime::loadGame(files));
        auto s = must(runtime::startSession(game));
        for (int i = 0; i < 30; ++i) s->tick({});
        const Entity hero = s->player();
        const std::string idle = game.animations->find(s->scene().get<Animator>(hero).current)->name;
        CHECK(contains(idle, "Idle"));
        for (int i = 0; i < 20; ++i) s->tick(keys(*s, {"move_right"}, i == 0));
        const std::string moving = game.animations->find(s->scene().get<Animator>(hero).current)->name;
        CHECK((contains(moving, "Run") || contains(moving, "Walk")));
    }
}
