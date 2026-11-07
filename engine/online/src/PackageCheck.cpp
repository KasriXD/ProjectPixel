#include "px/online/PackageCheck.hpp"

#include "px/assets/Image.hpp"
#include "px/audio/Sound.hpp"
#include "px/project/Font.hpp"
#include "px/project/Project.hpp"
#include "px/project/SceneFormat.hpp"
#include "px/runtime/GameLoader.hpp"
#include "px/scene/Components.hpp"

#include <nlohmann/json.hpp>
#include <stb_image_write.h>

#include <format>

namespace px::online {

namespace {
void appendBytes(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* p = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}

Result<std::vector<std::uint8_t>> reencodePng(const project::PackageFile& file) {
    auto image = assets::decodeImage(file.data, file.path);
    if (!image) return image.error();
    std::vector<std::uint8_t> png;
    const int ok = stbi_write_png_to_func(appendBytes, &png, image->width, image->height, 4, image->pixels.data(),
                                          image->width * 4);
    if (ok == 0 || png.empty()) return makeError(ErrorCode::InvalidData, "{}: the image could not be re-encoded", file.path);
    return png;
}
} // namespace

Result<PackageReport> checkPackage(project::Package& package, const PackageCheckOptions& options) {
    PackageReport report;
    auto files = package.toFileSystem();
    auto manifest = project::loadProjectManifest(*files);
    if (!manifest) return manifest.error();
    report.name = manifest->name;
    report.gameType = std::string(project::gameTypeName(manifest->settings.gameType));
    report.leaderboards = manifest->leaderboards;

    // Images: decoded with the engine's limits and written again as PNG, so
    // players only ever decode files the platform produced.
    for (const auto& entry : manifest->assets.entries()) {
        if (entry.type == assets::AssetType::Scene) ++report.scenes;
        if (assets::isBehaviour(entry.type)) ++report.scripts;
        if (entry.type != assets::AssetType::Texture) continue;
        auto path = normalizeRelativePath(entry.path);
        if (!path) return path.error();
        auto* file = const_cast<project::PackageFile*>(package.find(path.value()));
        if (file == nullptr) return makeError(ErrorCode::NotFound, "The package has no file '{}'", path.value());
        if (options.reencodeImages) {
            auto png = reencodePng(*file);
            if (!png) return png.error();
            file->data = std::move(png.value());
        } else {
            auto image = assets::decodeImage(file->data, file->path);
            if (!image) return image.error();
        }
        ++report.images;
    }

    // Sounds: decoded with the engine's limits and written again as QOA, so
    // players only ever decode sound files the platform produced (and only
    // the simplest format: no Vorbis or WAV variants from the internet).
    for (const auto& entry : manifest->assets.entries()) {
        if (entry.type != assets::AssetType::Sound) continue;
        auto path = normalizeRelativePath(entry.path);
        if (!path) return path.error();
        auto* file = const_cast<project::PackageFile*>(package.find(path.value()));
        if (file == nullptr) return makeError(ErrorCode::NotFound, "The package has no file '{}'", path.value());
        auto info = audio::probeSound(file->data);
        if (!info) return makeError(info.error().code, "{}: {}", file->path, info.error().message);
        report.soundSeconds += info->seconds();
        if (report.soundSeconds > kMaxGameSoundSeconds) {
            return makeError(ErrorCode::LimitExceeded, "The game's sounds are longer than {} minutes together",
                             static_cast<int>(kMaxGameSoundSeconds / 60));
        }
        auto pcm = audio::decodeSound(file->data);
        if (!pcm) return makeError(pcm.error().code, "{}: {}", file->path, pcm.error().message);
        if (options.reencodeSounds) {
            auto qoa = audio::encodeQoa(pcm.value());
            if (!qoa) return makeError(qoa.error().code, "{}: {}", file->path, qoa.error().message);
            file->data = std::move(qoa.value());
        }
        ++report.sounds;
    }
    files = package.toFileSystem();

    // Fonts: their letters must be inside their (re-encoded) images.
    for (const auto& entry : manifest->assets.entries()) {
        if (entry.type != assets::AssetType::Font) continue;
        auto font = project::loadFont(*files, manifest->assets, entry.id);
        if (!font) return font.error();
        const auto* texture = manifest->assets.find(font->texture);
        auto bytes = files->readBinary(texture->path);
        if (!bytes) return bytes.error();
        auto image = assets::decodeImage(bytes.value(), texture->path);
        if (!image) return image.error();
        if (auto inside = project::checkFontTexture(font.value(), {image->width, image->height}, entry.path); !inside) {
            return inside.error();
        }
        ++report.fonts;
    }

    // Every scene, not only the start scene.
    for (const auto& entry : manifest->assets.entries()) {
        if (entry.type != assets::AssetType::Scene) continue;
        auto scene = project::loadScene(*files, manifest.value(), entry.id);
        if (!scene) return scene.error();
    }

    // The game as players get it: manifest, start scene, scripts (compiled),
    // prefabs, animations, tilesets.
    auto game = runtime::loadGame(*files);
    if (!game) return game.error();
    report.warnings = game->warnings;
    report.entities = game->startScene.registry().count<scene::EntityInfo>();
    report.spawnPoints = game->startScene.registry().count<scene::Player>();

    if (options.ticks > 0) {
        auto session = runtime::startSession(game.value());
        if (!session) return session.error();
        for (int i = 0; i < options.ticks; ++i) (*session)->tick();
        report.scriptErrors = (*session)->state().scriptErrors;
        report.firstScriptError = (*session)->state().lastScriptError;
    }
    report.files = package.files.size();
    report.bytes = package.totalBytes();
    return report;
}

std::string leaderboardsToJson(const std::vector<project::LeaderboardDef>& boards) {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& b : boards) {
        list.push_back({{"id", b.id},
                        {"title", b.title},
                        {"order", std::string(project::leaderboardOrderName(b.order))},
                        {"format", std::string(project::scoreFormatName(b.format))}});
    }
    return list.dump();
}

Result<std::vector<project::LeaderboardDef>> leaderboardsFromJson(std::string_view json) {
    try {
        const auto list = nlohmann::json::parse(json);
        std::vector<project::LeaderboardDef> out;
        for (const auto& item : list) {
            project::LeaderboardDef b;
            b.id = item.at("id").get<std::string>();
            b.title = item.value("title", b.id);
            b.order = project::parseLeaderboardOrder(item.value("order", std::string("highest")))
                          .value_or(project::LeaderboardOrder::Highest);
            b.format = project::parseScoreFormat(item.value("format", std::string("number")))
                           .value_or(project::ScoreFormat::Number);
            if (!project::isValidLeaderboardId(b.id)) return makeError(ErrorCode::InvalidData, "bad leaderboard id");
            out.push_back(std::move(b));
        }
        return out;
    } catch (const std::exception& e) {
        return makeError(ErrorCode::InvalidData, "Invalid leaderboards: {}", e.what());
    }
}

std::string reportToJson(const PackageReport& r) {
    nlohmann::json j;
    j["name"] = r.name;
    j["gameType"] = r.gameType;
    j["files"] = r.files;
    j["bytes"] = r.bytes;
    j["images"] = r.images;
    j["sounds"] = r.sounds;
    j["soundSeconds"] = r.soundSeconds;
    j["fonts"] = r.fonts;
    j["scenes"] = r.scenes;
    j["scripts"] = r.scripts;
    j["entities"] = r.entities;
    j["spawnPoints"] = r.spawnPoints;
    j["scriptErrors"] = r.scriptErrors;
    j["firstScriptError"] = r.firstScriptError;
    j["warnings"] = r.warnings;
    j["leaderboards"] = nlohmann::json::parse(leaderboardsToJson(r.leaderboards));
    return j.dump();
}

Result<PackageReport> reportFromJson(std::string_view json) {
    try {
        const auto j = nlohmann::json::parse(json);
        PackageReport r;
        r.name = j.at("name").get<std::string>();
        r.gameType = j.at("gameType").get<std::string>();
        r.files = j.at("files").get<std::size_t>();
        r.bytes = j.at("bytes").get<std::size_t>();
        r.images = j.at("images").get<std::size_t>();
        r.sounds = j.value("sounds", std::size_t{0}); // reports from before sounds
        r.soundSeconds = j.value("soundSeconds", 0.0);
        r.fonts = j.value("fonts", std::size_t{0});
        r.scenes = j.at("scenes").get<std::size_t>();
        r.scripts = j.at("scripts").get<std::size_t>();
        r.entities = j.at("entities").get<std::size_t>();
        r.spawnPoints = j.at("spawnPoints").get<std::size_t>();
        r.scriptErrors = j.at("scriptErrors").get<int>();
        r.firstScriptError = j.at("firstScriptError").get<std::string>();
        r.warnings = j.at("warnings").get<std::vector<std::string>>();
        if (j.contains("leaderboards")) {
            auto boards = leaderboardsFromJson(j["leaderboards"].dump());
            if (!boards) return boards.error();
            r.leaderboards = std::move(boards.value());
        }
        return r;
    } catch (const std::exception& e) {
        return makeError(ErrorCode::InvalidData, "Invalid package report: {}", e.what());
    }
}

} // namespace px::online
