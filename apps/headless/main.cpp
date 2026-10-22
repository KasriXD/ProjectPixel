// px-headless: load a project and run its start scene without graphics.
//
//   px-headless <project-dir> [--ticks N] [--input-script FILE]
//               [--expect-all-collected] [--log-events] [--trace-every N] [--verbose]
//
// Exit codes: 0 success, 1 load/runtime error, 2 bad arguments,
//             3 an --expect-* condition was not met.

#include "px/core/FileSystem.hpp"
#include "px/core/Log.hpp"
#include "px/core/Version.hpp"
#include "px/project/Project.hpp"
#include "px/project/SceneFormat.hpp"
#include "px/runtime/GameSession.hpp"
#include "px/runtime/InputScript.hpp"

#include <charconv>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

using namespace px;

namespace {

struct Options {
    std::string projectDir;
    std::optional<int> ticks;
    std::string inputScriptPath;
    bool expectAllCollected = false;
    bool logEvents = false;
    bool verbose = false;
    int traceEvery = 0; // print the player state every N ticks (0 = off)
};

void printUsage() {
    std::fprintf(stderr,
                 "%s headless runner %s\n"
                 "usage: px-headless <project-dir> [--ticks N] [--input-script FILE]\n"
                 "                   [--expect-all-collected] [--log-events] [--trace-every N] [--verbose]\n",
                 version::productName, version::string);
}

std::optional<Options> parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto needValue = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--ticks") {
            const char* v = needValue();
            int n = 0;
            if (v == nullptr || std::from_chars(v, v + std::char_traits<char>::length(v), n).ec != std::errc() || n < 0) {
                return std::nullopt;
            }
            o.ticks = n;
        } else if (arg == "--input-script") {
            const char* v = needValue();
            if (v == nullptr) return std::nullopt;
            o.inputScriptPath = v;
        } else if (arg == "--trace-every") {
            const char* v = needValue();
            if (v == nullptr || std::from_chars(v, v + std::char_traits<char>::length(v), o.traceEvery).ec != std::errc()) {
                return std::nullopt;
            }
        } else if (arg == "--expect-all-collected") {
            o.expectAllCollected = true;
        } else if (arg == "--log-events") {
            o.logEvents = true;
        } else if (arg == "--verbose") {
            o.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            return std::nullopt;
        } else if (!arg.starts_with("--") && o.projectDir.empty()) {
            o.projectDir = arg;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return std::nullopt;
        }
    }
    if (o.projectDir.empty()) return std::nullopt;
    return o;
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parseArgs(argc, argv);
    if (!options) {
        printUsage();
        return 2;
    }
    if (options->verbose) log::setMinLevel(log::Level::Debug);

    DirectoryFileSystem files(options->projectDir);
    auto manifest = project::loadProjectManifest(files);
    if (!manifest) {
        log::error("{}", manifest.error().message);
        return 1;
    }
    auto scene = project::loadScene(files, manifest.value(), manifest->startScene);
    if (!scene) {
        log::error("{}", scene.error().message);
        return 1;
    }
    log::info("Loaded '{}' (scene '{}', {} entities)", manifest->name, scene->name(), scene->entityCount());

    runtime::GameSession session(manifest.value(), std::move(scene.value()));

    std::optional<runtime::InputScriptPlayer> script;
    int ticks = options->ticks.value_or(600);
    if (!options->inputScriptPath.empty()) {
        // The script path is a local tool argument, not project content.
        const std::filesystem::path scriptPath(options->inputScriptPath);
        DirectoryFileSystem scriptDir(scriptPath.parent_path().empty() ? "." : scriptPath.parent_path());
        auto text = scriptDir.readText(scriptPath.filename().generic_string());
        if (!text) {
            log::error("{}", text.error().message);
            return 1;
        }
        auto parsed = runtime::InputScript::parse(text.value(), session.actions());
        if (!parsed) {
            log::error("{}: {}", options->inputScriptPath, parsed.error().message);
            return 1;
        }
        if (!options->ticks) ticks = parsed->totalTicks();
        script.emplace(std::move(parsed.value()));
    }

    const auto describePlayer = [&session]() -> std::string {
        const auto player = session.player();
        const auto* t = session.scene().tryGet<scene::Transform>(player);
        if (t == nullptr) return "none";
        const auto* body = session.scene().tryGet<scene::KinematicBody>(player);
        return std::format("position=({:.2f}, {:.2f}) velocity=({:.1f}, {:.1f}) onGround={}", t->position.x,
                           t->position.y, body ? body->velocity.x : 0.0f, body ? body->velocity.y : 0.0f,
                           body != nullptr && body->onGround);
    };

    for (int i = 0; i < ticks; ++i) {
        const runtime::InputFrame frame = script ? script->next() : runtime::InputFrame{};
        session.tick(frame);
        if (options->traceEvery > 0 && session.tickCount() % static_cast<std::uint64_t>(options->traceEvery) == 0) {
            std::printf("tick %llu held=0x%x %s\n", static_cast<unsigned long long>(session.tickCount()), frame.held,
                        describePlayer().c_str());
        }
        if (options->logEvents) {
            for (const auto& ev : session.events()) {
                switch (ev.type) {
                    case runtime::GameEvent::Type::Collected:
                        log::info("tick {}: collected {} (+{})", session.tickCount(), ev.kind, ev.value);
                        break;
                    case runtime::GameEvent::Type::PlayerRespawned:
                        log::info("tick {}: player respawned", session.tickCount());
                        break;
                    case runtime::GameEvent::Type::AllCollected:
                        log::info("tick {}: all collectibles collected", session.tickCount());
                        break;
                }
            }
        }
    }

    const auto& state = session.state();
    const std::string playerText = describePlayer();
    std::printf("ticks=%llu seconds=%.3f score=%d collected=%d/%d respawns=%d allCollected=%s player: %s\n",
                static_cast<unsigned long long>(session.tickCount()), session.elapsedSeconds(), state.score,
                state.collectiblesCollected, state.collectiblesTotal, state.respawns,
                state.allCollected ? "true" : "false", playerText.c_str());

    if (options->expectAllCollected && !state.allCollected) {
        log::error("Expected all collectibles to be collected");
        return 3;
    }
    return 0;
}
