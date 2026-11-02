#include "px/project/Animation.hpp"

#include "JsonUtil.hpp"

#include <cmath>
#include <format>

namespace px::project {

using detail::fail;
using detail::Json;
using detail::ObjectReader;

float AnimationClip::duration() const {
    if (frames.empty() || framesPerSecond <= 0.0f) return 0.0f;
    return static_cast<float>(frames.size()) / framesPerSecond;
}

std::size_t AnimationClip::frameAt(float seconds) const {
    if (frames.empty()) return 0;
    const auto count = static_cast<long long>(frames.size());
    long long frame = static_cast<long long>(std::floor(std::max(0.0f, seconds) * framesPerSecond));
    if (loop) {
        frame %= count;
    } else if (frame >= count) {
        frame = count - 1;
    }
    return static_cast<std::size_t>(frame);
}

bool AnimationClip::finishedAt(float seconds) const {
    return !loop && !frames.empty() && seconds * framesPerSecond >= static_cast<float>(frames.size());
}

Result<AnimationClip> parseAnimation(std::string_view json, std::string_view sourceName,
                                     const assets::AssetRegistry* assets) {
    try {
        Json root = detail::parseJsonText(json);
        std::vector<std::string> warnings;
        detail::checkAndMigrate(root, "px.animation", kAnimationFormatVersion, {}, "animation", warnings);
        ObjectReader r(root, "");
        (void)r.find("format");
        (void)r.find("formatVersion");
        AnimationClip clip;
        r.required("texture", clip.texture);
        if (clip.texture.isNil()) fail("texture", "a texture asset id is required");
        if (assets != nullptr) {
            const auto* entry = assets->find(clip.texture);
            if (entry == nullptr) {
                fail("texture", std::format("refers to asset {}, which is not in the project", clip.texture.toString()));
            }
            if (entry->type != assets::AssetType::Texture) {
                fail("texture", std::format("asset '{}' is a {}, not a texture", entry->name,
                                            assets::assetTypeName(entry->type)));
            }
        }
        const Json& frames = r.require("frames");
        if (!frames.is_array()) fail("frames", "expected an array of [x, y, w, h] rectangles");
        if (frames.size() > kMaxAnimationClipFrames) {
            fail("frames", std::format("too many frames; the limit is {}", kMaxAnimationClipFrames));
        }
        for (std::size_t i = 0; i < frames.size(); ++i) {
            Rect frame;
            detail::readValue(frames[i], detail::indexPath("frames", i), frame);
            if (!(frame.w > 0.0f) || !(frame.h > 0.0f)) {
                fail(detail::indexPath("frames", i), "width and height must be positive");
            }
            clip.frames.push_back(frame);
        }
        r.optional("framesPerSecond", clip.framesPerSecond);
        if (!(clip.framesPerSecond > 0.0f) || clip.framesPerSecond > 240.0f) {
            fail("framesPerSecond", "must be greater than 0 and at most 240");
        }
        r.optional("loop", clip.loop);
        r.optional("cellSize", clip.cellSize);
        if (!(clip.cellSize.x >= 1.0f) || !(clip.cellSize.y >= 1.0f) || clip.cellSize.x > 4096.0f ||
            clip.cellSize.y > 4096.0f) {
            fail("cellSize", "must be between [1, 1] and [4096, 4096]");
        }
        r.finish();
        return clip;
    } catch (const detail::FormatError& e) {
        return makeError(ErrorCode::InvalidData, "{}: {}", sourceName, e.what());
    }
}

std::string serializeAnimation(const AnimationClip& clip) {
    Json root;
    root["format"] = "px.animation";
    root["formatVersion"] = kAnimationFormatVersion;
    root["texture"] = detail::toJson(clip.texture);
    Json frames = Json::array();
    for (const Rect& f : clip.frames) frames.push_back(detail::toJson(f));
    root["frames"] = std::move(frames);
    root["framesPerSecond"] = detail::toJson(clip.framesPerSecond);
    root["loop"] = clip.loop;
    root["cellSize"] = detail::toJson(clip.cellSize);
    return detail::dumpPretty(root);
}

Result<AnimationClip> loadAnimation(const IFileSystem& fs, const ProjectManifest& project, assets::AssetId id) {
    const auto* entry = project.assets.find(id);
    if (entry == nullptr) return makeError(ErrorCode::NotFound, "Animation {} is not in the project", id.toString());
    if (entry->type != assets::AssetType::Animation) {
        return makeError(ErrorCode::InvalidArgument, "Asset '{}' is not an animation", entry->name);
    }
    auto text = fs.readText(entry->path);
    if (!text) return text.error();
    return parseAnimation(text.value(), fs.describe(entry->path), &project.assets);
}

void AnimationLibrary::add(Entry entry) { m_entries.push_back(std::move(entry)); }

const AnimationLibrary::Entry* AnimationLibrary::find(assets::AssetId id) const {
    for (const Entry& e : m_entries) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

const AnimationLibrary::Entry* AnimationLibrary::findByName(std::string_view name) const {
    for (const Entry& e : m_entries) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

std::vector<std::string> AnimationLibrary::names() const {
    std::vector<std::string> out;
    for (const Entry& e : m_entries) out.push_back(e.name);
    return out;
}

Result<std::shared_ptr<const AnimationLibrary>> loadAnimationLibrary(const IFileSystem& fs,
                                                                      const ProjectManifest& project) {
    auto library = std::make_shared<AnimationLibrary>();
    for (const auto& entry : project.assets.entries()) {
        if (entry.type != assets::AssetType::Animation) continue;
        auto clip = loadAnimation(fs, project, entry.id);
        if (!clip) return clip.error();
        library->add({entry.id, entry.name, std::move(clip.value())});
    }
    return std::shared_ptr<const AnimationLibrary>(std::move(library));
}

std::vector<Rect> gridCells(Vec2i textureSize, Vec2 cellSize) {
    std::vector<Rect> cells;
    if (cellSize.x < 1.0f || cellSize.y < 1.0f) return cells;
    const int columns = static_cast<int>(static_cast<float>(textureSize.x) / cellSize.x);
    const int rows = static_cast<int>(static_cast<float>(textureSize.y) / cellSize.y);
    if (static_cast<long long>(columns) * rows > 16384) return cells; // not a sprite sheet worth picking from
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < columns; ++x) {
            cells.push_back({static_cast<float>(x) * cellSize.x, static_cast<float>(y) * cellSize.y, cellSize.x,
                             cellSize.y});
        }
    }
    return cells;
}

} // namespace px::project
