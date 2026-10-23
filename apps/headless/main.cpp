// px-headless: load a project and run its start scene without graphics.
// It is the seed of the dedicated server and is used for automated tests.
//
//   px-headless <project-dir> [--ticks N] [--input-script FILE]
//               [--expect-level-complete] [--expect-no-script-errors]
//               [--log-events] [--trace-every N] [--verbose]
//   px-headless --print-api-reference
//   px-headless --print-component-reference
//
// Exit codes: 0 success, 1 load/runtime error, 2 bad arguments,
//             3 an --expect-* condition was not met.

#include "px/core/FileSystem.hpp"
#include "px/scene/ComponentInfo.hpp"
#include "px/core/Log.hpp"
#include "px/core/Version.hpp"
#include "px/runtime/GameLoader.hpp"
#include "px/runtime/InputScript.hpp"
#include "px/scripting/ApiReference.hpp"

#include <charconv>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <string_view>

using namespace px;

namespace {

struct Options {
    std::string projectDir;
    std::optional<int> ticks;
    std::string inputScriptPath;
    bool expectLevelComplete = false;
    bool expectNoScriptErrors = false;
    bool logEvents = false;
    bool verbose = false;
    bool printApiReference = false;
    bool printComponentReference = false;
    int traceEvery = 0; // print the player state every N ticks (0 = off)
};

void printUsage() {
    std::fprintf(stderr,
                 "%s headless runner %s\n"
                 "usage: px-headless <project-dir> [--ticks N] [--input-script FILE]\n"
                 "                   [--expect-level-complete] [--expect-no-script-errors]\n"
                 "                   [--log-events] [--trace-every N] [--verbose]\n"
                 "       px-headless --print-api-reference\n"
                 "       px-headless --print-component-reference\n",
                 version::productName, version::string);
}

bool parseInt(const char* v, int& out) {
    if (v == nullptr) return false;
    const std::string_view s(v);
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size() && out >= 0;
}

std::optional<Options> parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto needValue = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--ticks") {
            int n = 0;
            if (!parseInt(needValue(), n)) return std::nullopt;
            o.ticks = n;
        } else if (arg == "--input-script") {
            const char* v = needValue();
            if (v == nullptr) return std::nullopt;
            o.inputScriptPath = v;
        } else if (arg == "--trace-every") {
            if (!parseInt(needValue(), o.traceEvery)) return std::nullopt;
        } else if (arg == "--expect-level-complete") {
            o.expectLevelComplete = true;
        } else if (arg == "--expect-no-script-errors") {
            o.expectNoScriptErrors = true;
        } else if (arg == "--log-events") {
            o.logEvents = true;
        } else if (arg == "--verbose") {
            o.verbose = true;
        } else if (arg == "--print-api-reference") {
            o.printApiReference = true;
        } else if (arg == "--print-component-reference") {
            o.printComponentReference = true;
        } else if (arg == "--help" || arg == "-h") {
            return std::nullopt;
        } else if (!arg.starts_with("--") && o.projectDir.empty()) {
            o.projectDir = arg;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return std::nullopt;
        }
    }
    if (o.projectDir.empty() && !o.printApiReference && !o.printComponentReference) return std::nullopt;
    return o;
}

const char* eventName(runtime::GameEvent::Type type) {
    switch (type) {
        case runtime::GameEvent::Type::ScoreChanged: return "score";
        case runtime::GameEvent::Type::ObjectiveChanged: return "objective";
        case runtime::GameEvent::Type::LevelCompleted: return "level complete";
        case runtime::GameEvent::Type::PlayerRespawned: return "player respawned";
        case runtime::GameEvent::Type::ScriptError: return "script error";
        case runtime::GameEvent::Type::ScriptEvent: return "script event";
    }
    return "?";
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parseArgs(argc, argv);
    if (!options) {
        printUsage();
        return 2;
    }
    if (options->printApiReference) {
        const std::string md = scripting::generateApiReferenceMarkdown();
        std::fwrite(md.data(), 1, md.size(), stdout);
        return 0;
    }
    if (options->printComponentReference) {
        const std::string md = scene::generateComponentReferenceMarkdown();
        std::fwrite(md.data(), 1, md.size(), stdout);
        return 0;
    }
    if (options->verbose) log::setMinLevel(log::Level::Debug);

    DirectoryFileSystem files(options->projectDir);
    auto game = runtime::loadGame(files);
    if (!game) {
        log::error("{}", game.error().message);
        return 1;
    }
    for (const auto& w : game->warnings) log::warn("{}", w);
    auto created = runtime::startSession(game.value());
    if (!created) {
        log::error("{}", created.error().message);
        return 1;
    }
    runtime::GameSession& session = *created.value();
    log::info("Loaded '{}' (scene '{}', {} entities, {} scripts)", game->manifest.name, session.scene().name(),
              session.scene().entityCount(), game->scripts->size());

    std::optional<runtime::InputScriptPlayer> script;
    int ticks = options->ticks.value_or(600);
    if (!options->inputScriptPath.empty()) {
        // The input script path is a local tool argument, not project content.
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
        const auto* t = session.scene().tryGet<scene::Transform>(session.player());
        if (t == nullptr) return "none";
        const auto* body = session.scene().tryGet<scene::KinematicBody>(session.player());
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
                std::string detail = ev.text;
                if (ev.type == runtime::GameEvent::Type::ScoreChanged) detail = std::to_string(ev.value);
                log::info("tick {}: {}{}{}", session.tickCount(), eventName(ev.type), detail.empty() ? "" : ": ",
                          detail);
            }
        }
    }

    const auto& state = session.state();
    const auto stats = session.scripts().stats();
    std::printf("ticks=%llu seconds=%.3f score=%d levelComplete=%s respawns=%d scriptErrors=%d objective=\"%s\" "
                "scripts=%zu/%zu memory=%zuKiB player: %s\n",
                static_cast<unsigned long long>(session.tickCount()), session.elapsedSeconds(), state.score,
                state.levelComplete ? "true" : "false", state.respawns, state.scriptErrors, state.objective.c_str(),
                stats.instances - stats.disabledInstances, stats.instances, stats.memoryBytes / 1024,
                describePlayer().c_str());

    if (options->expectLevelComplete && !state.levelComplete) {
        log::error("Expected the level to be completed");
        return 3;
    }
    if (options->expectNoScriptErrors && state.scriptErrors > 0) {
        log::error("Expected no script errors, got {}: {}", state.scriptErrors, state.lastScriptError);
        return 3;
    }
    return 0;
}
