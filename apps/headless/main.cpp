// px-headless: load a project and run its start scene without graphics.
// It is the seed of the dedicated server and is used for automated tests.
//
//   px-headless <project-dir> [--ticks N] [--input-script FILE]
//               [--expect-level-complete] [--expect-no-script-errors]
//               [--log-events] [--trace-every N] [--verbose]
//   px-headless <project-dir> --resave
//   px-headless <project-dir> --connect ADDRESS [--name NAME] [--input-script FILE] [--ticks N]
//   px-headless <project-dir> --net-test [--clients N] [--latency MS] [--jitter MS] [--loss PCT]
//               [--udp] [--port N] [--input-script FILE] [--ticks N] [--expect-...]
//   px-headless --print-api-reference
//   px-headless --compile-graph FILE.graph.json
//   px-headless --print-component-reference
//
// Exit codes: 0 success, 1 load/runtime error, 2 bad arguments,
//             3 an --expect-* condition was not met.

#include "NetTools.hpp"

#include "px/core/FileSystem.hpp"
#include "px/core/Log.hpp"
#include "px/core/Version.hpp"
#include "px/project/Animation.hpp"
#include "px/project/Package.hpp"
#include "px/project/Prefab.hpp"
#include "px/project/SceneFormat.hpp"
#include "px/project/Tileset.hpp"
#include "px/runtime/GameLoader.hpp"
#include "px/scene/ComponentInfo.hpp"
#include "px/runtime/InputScript.hpp"
#include "px/scripting/ApiReference.hpp"
#include "px/scripting/Graph.hpp"
#if defined(PX_HAVE_PLATFORM)
#include "px/online/PackageCheck.hpp"
#include "px/online/Thumbnail.hpp"
#include "px/online/PlatformClient.hpp"
#endif

#include <charconv>
#include <filesystem>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    std::string compileGraph; // print the Luau a visual script compiles to
    bool printComponentReference = false;
    bool resave = false;
    std::string packTo;       // --pack FILE: write the project as a game package
    std::string checkPackage; // --check-package FILE: validate an uploaded package (platform)
    std::string checkOutput;  // --output FILE: the checked package, images re-encoded
    std::string checkThumbnail; // --check-thumbnail FILE: make a 320x180 PNG of a picture (platform)
    std::string thumbnail;      // --thumbnail FILE: with --publish, the game's picture
    int traceEvery = 0; // print the player state every N ticks (0 = off)
    // Multiplayer tools (NetTools.hpp).
    std::string connectAddress;
    std::string playerName;
    bool netTest = false;
    int clients = 2;
    double latencyMs = 0.0;
    double jitterMs = 0.0;
    double lossPercent = 0.0;
    bool udp = false;
    int port = 0;
    bool expectJoined = false;
    int expectPlayers = 0;
    // Platform tools (docs/platform.md#command-line-tools).
    std::string platformUrl;
    std::string user;
    std::string password;
    bool registerAccount = false;
    bool publish = false;       // --publish: publish the project
    long long game = 0;         // --game ID: an existing game (publish) / --play ID: the game to join
    std::string title;
    std::string notes;
    bool play = false;
};

void printUsage() {
    std::fprintf(stderr,
                 "%s headless runner %s\n"
                 "usage: px-headless <project-dir> [--ticks N] [--input-script FILE]\n"
                 "                   [--expect-level-complete] [--expect-no-script-errors]\n"
                 "                   [--log-events] [--trace-every N] [--verbose]\n"
                 "       px-headless <project-dir> --resave   (rewrite project.json, scenes, prefabs,\n"
                 "                                             animations, tilesets and visual scripts\n"
                 "                                             as Studio saves them)\n"
                 "       px-headless <project-dir> --connect ADDRESS[:PORT] [--name NAME]\n"
                 "                   [--input-script FILE] [--ticks N] [--expect-joined] [--expect-players N]\n"
                 "                                            (join a px-server as a scripted bot)\n"
                 "       px-headless <project-dir> --net-test [--clients N] [--latency MS] [--jitter MS]\n"
                 "                   [--loss PCT] [--udp] [--port N] [--input-script FILE] [--ticks N]\n"
                 "                   [--expect-joined] [--expect-level-complete] [--expect-no-script-errors]\n"
                 "                                            (server and clients in one process; client 1\n"
                 "                                             plays the input script)\n"
                 "       px-headless <project-dir> --pack FILE.pxpkg   (write the game as a package)\n"
                 "       px-headless --check-package FILE.pxpkg [--output FILE] [--ticks N]\n"
                 "       px-headless --check-thumbnail PICTURE --output FILE.png   (the platform's thumbnail check)\n"
                 "                                            (the platform's upload check; prints a report)\n"
                 "       px-headless <project-dir or FILE.pxpkg> ...   (packages run like project folders)\n"
                 "       px-headless <project-dir> --platform URL --user NAME --password TEXT [--register]\n"
                 "                   --publish [--game ID | --title TEXT] [--notes TEXT] [--thumbnail PICTURE]\n"
                 "                                            (publish the game on a platform)\n"
                 "       px-headless --platform URL --user NAME --password TEXT [--register] --play ID\n"
                 "                   [--input-script FILE] [--ticks N] [--expect-...]\n"
                 "                                            (join a published game as a scripted bot)\n"
                 "       px-headless --print-api-reference\n"
                 "       px-headless --compile-graph FILE.graph.json   (print the Luau a visual script runs as)\n"
                 "       px-headless --print-component-reference\n",
                 version::productName, version::string);
}

bool parseInt(const char* v, int& out) {
    if (v == nullptr) return false;
    const std::string_view s(v);
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size() && out >= 0;
}

bool parseDouble(const char* v, double& out) {
    if (v == nullptr) return false;
    try {
        std::size_t used = 0;
        out = std::stod(v, &used);
        return used == std::string_view(v).size() && out >= 0.0;
    } catch (...) {
        return false;
    }
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
        } else if (arg == "--resave") {
            o.resave = true;
        } else if (arg == "--connect") {
            const char* v = needValue();
            if (v == nullptr) return std::nullopt;
            o.connectAddress = v;
        } else if (arg == "--name") {
            const char* v = needValue();
            if (v == nullptr) return std::nullopt;
            o.playerName = v;
        } else if (arg == "--net-test") {
            o.netTest = true;
        } else if (arg == "--clients") {
            if (!parseInt(needValue(), o.clients) || o.clients < 1 || o.clients > 64) return std::nullopt;
        } else if (arg == "--latency") {
            if (!parseDouble(needValue(), o.latencyMs)) return std::nullopt;
        } else if (arg == "--jitter") {
            if (!parseDouble(needValue(), o.jitterMs)) return std::nullopt;
        } else if (arg == "--loss") {
            if (!parseDouble(needValue(), o.lossPercent) || o.lossPercent > 100.0) return std::nullopt;
        } else if (arg == "--udp") {
            o.udp = true;
        } else if (arg == "--port") {
            if (!parseInt(needValue(), o.port) || o.port < 1 || o.port > 65535) return std::nullopt;
        } else if (arg == "--expect-joined") {
            o.expectJoined = true;
        } else if (arg == "--expect-players") {
            if (!parseInt(needValue(), o.expectPlayers) || o.expectPlayers < 1) return std::nullopt;
        } else if (arg == "--verbose") {
            o.verbose = true;
        } else if (arg == "--print-api-reference") {
            o.printApiReference = true;
        } else if (arg == "--print-component-reference") {
            o.printComponentReference = true;
        } else if (arg == "--compile-graph") {
            o.compileGraph = needValue();
        } else if (arg == "--platform") {
            o.platformUrl = needValue();
        } else if (arg == "--user") {
            o.user = needValue();
        } else if (arg == "--password") {
            o.password = needValue();
        } else if (arg == "--register") {
            o.registerAccount = true;
        } else if (arg == "--publish") {
            o.publish = true;
        } else if (arg == "--title") {
            o.title = needValue();
        } else if (arg == "--notes") {
            o.notes = needValue();
        } else if (arg == "--game" || arg == "--play") {
            o.play = o.play || arg == "--play";
            try {
                o.game = std::stoll(std::string(needValue()));
            } catch (...) {
                return std::nullopt;
            }
        } else if (arg == "--pack") {
            o.packTo = needValue();
        } else if (arg == "--check-package") {
            o.checkPackage = needValue();
        } else if (arg == "--output") {
            o.checkOutput = needValue();
        } else if (arg == "--check-thumbnail") {
            o.checkThumbnail = needValue();
        } else if (arg == "--thumbnail") {
            o.thumbnail = needValue();
        } else if (arg == "--help" || arg == "-h") {
            return std::nullopt;
        } else if (!arg.starts_with("--") && o.projectDir.empty()) {
            o.projectDir = arg;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return std::nullopt;
        }
    }
    if (o.projectDir.empty() && !o.printApiReference && !o.printComponentReference && o.compileGraph.empty() &&
        o.checkPackage.empty() && o.checkThumbnail.empty() && !(o.play && !o.platformUrl.empty())) {
        return std::nullopt;
    }
    return o;
}

// The platform's thumbnail check: the picture becomes a 320x180 PNG in --output.
int checkThumbnailCommand(const Options& o) {
#if defined(PX_HAVE_PLATFORM)
    auto bytes = readLocalFile(o.checkThumbnail);
    if (!bytes) {
        std::fprintf(stderr, "%s\n", bytes.error().message.c_str());
        return 1;
    }
    auto png = online::makeThumbnail(bytes.value());
    if (!png) {
        std::fprintf(stderr, "%s\n", png.error().message.c_str());
        return 1;
    }
    if (o.checkOutput.empty()) {
        std::fprintf(stderr, "--check-thumbnail needs --output FILE\n");
        return 2;
    }
    std::FILE* out = std::fopen(o.checkOutput.c_str(), "wb");
    if (out == nullptr || std::fwrite(png->data(), 1, png->size(), out) != png->size()) {
        if (out != nullptr) std::fclose(out);
        std::fprintf(stderr, "cannot write %s\n", o.checkOutput.c_str());
        return 1;
    }
    std::fclose(out);
    return 0;
#else
    (void)o;
    std::fprintf(stderr, "this build has no platform support\n");
    return 1;
#endif
}

// The platform's upload check (docs/platform.md#publishing): run in its own
// process with resource limits. Prints a JSON report on success; the error
// on stderr and exit code 1 if the game cannot be accepted.
int checkPackageCommand(const Options& o) {
#if defined(PX_HAVE_PLATFORM)
    log::setMinLevel(log::Level::Error); // stdout is the report; keep game logs out of it
    auto package = project::readPackageFile(o.checkPackage);
    if (!package) {
        std::fprintf(stderr, "%s\n", package.error().message.c_str());
        return 1;
    }
    online::PackageCheckOptions check;
    if (o.ticks) check.ticks = *o.ticks;
    auto report = online::checkPackage(package.value(), check);
    if (!report) {
        std::fprintf(stderr, "%s\n", report.error().message.c_str());
        return 1;
    }
    if (!o.checkOutput.empty()) {
        if (auto written = project::writePackageFile(o.checkOutput, package.value()); !written) {
            std::fprintf(stderr, "%s\n", written.error().message.c_str());
            return 1;
        }
    }
    const std::string json = online::reportToJson(report.value());
    std::printf("%s\n", json.c_str());
    return 0;
#else
    (void)o;
    std::fprintf(stderr, "this build has no platform support\n");
    return 1;
#endif
}

#if defined(PX_HAVE_PLATFORM)
// Logs in (or creates the account) for the platform tools.
Result<online::PlatformClient> platformLogin(const Options& o) {
    online::PlatformClient client(o.platformUrl);
    if (o.user.empty() || o.password.empty()) {
        return makeError(ErrorCode::InvalidArgument, "--platform needs --user NAME --password TEXT");
    }
    auto auth = o.registerAccount ? client.registerAccount(o.user, o.password) : client.login(o.user, o.password);
    if (!auth) return auth.error();
    return client;
}

// --publish: packages the project, creates the game if needed, uploads a version.
int publishCommand(const Options& o) {
    auto client = platformLogin(o);
    if (!client) {
        log::error("{}", client.error().message);
        return 1;
    }
    auto package = project::collectPackage(std::filesystem::path(o.projectDir));
    if (!package) {
        log::error("{}", package.error().message);
        return 1;
    }
    long long gameId = o.game;
    if (gameId == 0) {
        auto manifest = project::loadProjectManifest(*package->toFileSystem());
        const std::string title = o.title.empty() ? (manifest ? manifest->name : std::string("My Game")) : o.title;
        auto game = client->createGame(title, "", online::Visibility::Public);
        if (!game) {
            log::error("{}", game.error().message);
            return 1;
        }
        gameId = game->id;
        log::info("Created game {} '{}'", gameId, game->title);
    }
    auto version = client->publish(gameId, project::encodePackage(package.value()), o.notes);
    if (!version) {
        log::error("{}", version.error().message);
        return 1;
    }
    std::printf("published game=%lld version=%d bytes=%llu package=%s\n", gameId, version->number,
                static_cast<unsigned long long>(version->bytes), version->packageHash.c_str());
    if (!o.thumbnail.empty()) {
        auto picture = readLocalFile(o.thumbnail);
        if (!picture) {
            log::error("{}", picture.error().message);
            return 1;
        }
        if (auto sent = client->uploadThumbnail(gameId, picture.value()); !sent) {
            log::error("The thumbnail was not accepted: {}", sent.error().message);
            return 1;
        }
        std::printf("thumbnail uploaded\n");
    }
    return 0;
}

// --play: joins a published game through the platform as a scripted bot.
int playCommand(const Options& o, headless::NetToolOptions net) {
    auto client = platformLogin(o);
    if (!client) {
        log::error("{}", client.error().message);
        return 1;
    }
    auto ticket = client->join(o.game);
    if (!ticket) {
        log::error("{}", ticket.error().message);
        return 1;
    }
    online::PackageCache cache(online::userPlatformDir() / "packages");
    auto bytes = cache.get(client.value(), ticket->packageHash);
    if (!bytes) {
        log::error("{}", bytes.error().message);
        return 1;
    }
    auto package = project::decodePackage(bytes.value());
    if (!package) {
        log::error("{}", package.error().message);
        return 1;
    }
    auto game = runtime::loadGame(*package->toFileSystem());
    if (!game) {
        log::error("{}", game.error().message);
        return 1;
    }
    if (!o.inputScriptPath.empty()) {
        const std::filesystem::path scriptPath(o.inputScriptPath);
        DirectoryFileSystem scriptDir(scriptPath.parent_path().empty() ? "." : scriptPath.parent_path());
        auto text = scriptDir.readText(scriptPath.filename().generic_string());
        if (!text) {
            log::error("{}", text.error().message);
            return 1;
        }
        auto parsed = runtime::InputScript::parse(text.value(), runtime::ActionSet(game->manifest.input));
        if (!parsed) {
            log::error("{}: {}", o.inputScriptPath, parsed.error().message);
            return 1;
        }
        net.script = std::move(parsed.value());
    }
    net.connectAddress = std::format("{}:{}", ticket->host, ticket->port);
    net.token = ticket->token;
    log::info("Joining game {} version {} at {}", o.game, ticket->version, net.connectAddress);
    return headless::runBot(game.value(), net);
}
#endif

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

// Rewrites the manifest and every scene in the current format (migrations
// applied, every field written), exactly as Studio would save them.
int resaveProject(DirectoryFileSystem& files) {
    auto manifest = project::loadProjectManifest(files);
    if (!manifest) {
        log::error("{}", manifest.error().message);
        return 1;
    }
    for (const auto& entry : manifest->assets.entries()) {
        if (entry.type == assets::AssetType::Animation || entry.type == assets::AssetType::Tileset ||
            entry.type == assets::AssetType::Graph) {
            std::string saved;
            if (entry.type == assets::AssetType::Graph) {
                auto text = files.readText(entry.path);
                auto graph = text ? scripting::parseGraph(text.value(), files.describe(entry.path))
                                  : Result<scripting::Graph>(text.error());
                if (!graph) {
                    log::error("{}", graph.error().message);
                    return 1;
                }
                saved = scripting::serializeGraph(graph.value());
            } else if (entry.type == assets::AssetType::Animation) {
                auto clip = project::loadAnimation(files, manifest.value(), entry.id);
                if (!clip) {
                    log::error("{}", clip.error().message);
                    return 1;
                }
                saved = project::serializeAnimation(clip.value());
            } else {
                auto tileset = project::loadTileset(files, manifest.value(), entry.id);
                if (!tileset) {
                    log::error("{}", tileset.error().message);
                    return 1;
                }
                saved = project::serializeTileset(tileset.value());
            }
            if (auto current = files.readText(entry.path); current && current.value() != saved) {
                if (auto written = files.writeText(entry.path, saved); !written) {
                    log::error("{}", written.error().message);
                    return 1;
                }
                log::info("Rewrote {}", entry.path);
            }
            continue;
        }
        if (entry.type != assets::AssetType::Scene && entry.type != assets::AssetType::Prefab) continue;
        auto text = files.readText(entry.path);
        if (!text) {
            log::error("{}", text.error().message);
            return 1;
        }
        std::vector<std::string> warnings;
        const bool isPrefab = entry.type == assets::AssetType::Prefab;
        auto scene = isPrefab
                         ? project::parsePrefab(text.value(), files.describe(entry.path), &manifest->assets, &warnings)
                         : project::parseScene(text.value(), files.describe(entry.path), &manifest->assets, &warnings);
        if (!scene) {
            log::error("{}", scene.error().message);
            return 1;
        }
        for (const auto& w : warnings) log::warn("{}", w);
        const std::string saved = isPrefab ? project::serializePrefab(scene.value()) : project::serializeScene(scene.value());
        if (saved != text.value()) {
            if (auto written = files.writeText(entry.path, saved); !written) {
                log::error("{}", written.error().message);
                return 1;
            }
            log::info("Rewrote {}", entry.path);
        }
    }
    const std::string manifestText = project::serializeProjectManifest(manifest.value());
    auto current = files.readText(project::kProjectFileName);
    if (!current || current.value() != manifestText) {
        if (auto written = files.writeText(project::kProjectFileName, manifestText); !written) {
            log::error("{}", written.error().message);
            return 1;
        }
        log::info("Rewrote {}", project::kProjectFileName);
    }
    return 0;
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
    if (!options->compileGraph.empty()) {
        // Shows what a visual script runs as (and its problems).
        const std::filesystem::path file(options->compileGraph);
        DirectoryFileSystem here(file.has_parent_path() ? file.parent_path() : std::filesystem::path("."));
        auto text = here.readText(file.filename().string());
        if (!text) {
            log::error("{}", text.error().message);
            return 1;
        }
        auto graph = scripting::parseGraph(text.value(), options->compileGraph);
        if (!graph) {
            log::error("{}", graph.error().message);
            return 1;
        }
        const auto compiled = scripting::compileGraph(graph.value(), options->compileGraph);
        for (const auto& p : compiled.problems) log::error("{}: {}", options->compileGraph, p.message);
        std::fwrite(compiled.source.data(), 1, compiled.source.size(), stdout);
        return compiled.ok() ? 0 : 1;
    }
    if (options->printComponentReference) {
        const std::string md = scene::generateComponentReferenceMarkdown();
        std::fwrite(md.data(), 1, md.size(), stdout);
        return 0;
    }
    if (!options->checkPackage.empty()) return checkPackageCommand(*options);
    if (!options->checkThumbnail.empty()) return checkThumbnailCommand(*options);
    if (!options->platformUrl.empty()) {
#if defined(PX_HAVE_PLATFORM)
        if (options->publish) return publishCommand(*options);
        if (options->play) {
            headless::NetToolOptions net;
            net.ticks = options->ticks;
            net.expectLevelComplete = options->expectLevelComplete;
            net.expectNoScriptErrors = options->expectNoScriptErrors;
            net.expectJoined = options->expectJoined;
            net.expectPlayers = options->expectPlayers;
            return playCommand(*options, std::move(net));
        }
        log::error("--platform goes with --publish or --play ID");
        return 2;
#else
        log::error("This build has no platform support");
        return 1;
#endif
    }
    if (options->verbose) log::setMinLevel(log::Level::Debug);

    // A project folder, or a game package (*.pxpkg) as the platform sends it.
    std::unique_ptr<IFileSystem> files;
    if (options->projectDir.ends_with(".pxpkg")) {
        auto package = project::readPackageFile(options->projectDir);
        if (!package) {
            log::error("{}", package.error().message);
            return 1;
        }
        files = package->toFileSystem();
    } else {
        auto dir = std::make_unique<DirectoryFileSystem>(options->projectDir);
        if (options->resave) return resaveProject(*dir);
        files = std::move(dir);
    }
    if (!options->packTo.empty()) {
        auto package = project::collectPackage(*files);
        if (!package) {
            log::error("{}", package.error().message);
            return 1;
        }
        if (auto written = project::writePackageFile(options->packTo, package.value()); !written) {
            log::error("{}", written.error().message);
            return 1;
        }
        log::info("Wrote {} ({} files, {} bytes)", options->packTo, package->files.size(), package->totalBytes());
        return 0;
    }
    auto game = runtime::loadGame(*files);
    if (!game) {
        log::error("{}", game.error().message);
        return 1;
    }
    for (const auto& w : game->warnings) log::warn("{}", w);

    if (!options->connectAddress.empty() || options->netTest) {
        headless::NetToolOptions net;
        net.connectAddress = options->connectAddress;
        net.playerName = options->playerName;
        net.clients = options->clients;
        net.latencyMs = options->latencyMs;
        net.jitterMs = options->jitterMs;
        net.lossPercent = options->lossPercent;
        net.udp = options->udp;
        net.port = options->port;
        net.ticks = options->ticks;
        net.expectLevelComplete = options->expectLevelComplete;
        net.expectNoScriptErrors = options->expectNoScriptErrors;
        net.expectJoined = options->expectJoined;
        net.expectPlayers = options->expectPlayers;
        if (!options->inputScriptPath.empty()) {
            const std::filesystem::path scriptPath(options->inputScriptPath);
            DirectoryFileSystem scriptDir(scriptPath.parent_path().empty() ? "." : scriptPath.parent_path());
            auto text = scriptDir.readText(scriptPath.filename().generic_string());
            if (!text) {
                log::error("{}", text.error().message);
                return 1;
            }
            auto parsed = runtime::InputScript::parse(text.value(), runtime::ActionSet(game->manifest.input));
            if (!parsed) {
                log::error("{}: {}", options->inputScriptPath, parsed.error().message);
                return 1;
            }
            net.script = std::move(parsed.value());
        }
        return options->netTest ? headless::runNetTest(game.value(), net) : headless::runBot(game.value(), net);
    }

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
