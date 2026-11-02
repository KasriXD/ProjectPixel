#pragma once

#include "px/assets/AssetRegistry.hpp"
#include "px/core/FileSystem.hpp"
#include "px/core/Math.hpp"
#include "px/core/Result.hpp"
#include "px/project/Project.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Animation clips ("*.anim.json"): frames of a sprite sheet played at a
// speed, looping or once. An Animator component plays them on a Sprite
// (docs/animation.md).
namespace px::project {

inline constexpr int kAnimationFormatVersion = 1;
inline constexpr std::size_t kMaxAnimationClipFrames = 1024;

struct AnimationClip {
    assets::AssetId texture;     // the sprite sheet
    std::vector<Rect> frames;    // texel rectangles, in playing order
    float framesPerSecond = 8.0f;
    bool loop = true;            // otherwise it stops on the last frame
    Vec2 cellSize{16.0f, 16.0f}; // Studio's frame grid (not used when playing)

    // Seconds for one pass over the frames (0 without frames).
    [[nodiscard]] float duration() const;
    // The frame shown `seconds` after the clip started.
    [[nodiscard]] std::size_t frameAt(float seconds) const;
    // True once a clip that does not loop has shown its last frame.
    [[nodiscard]] bool finishedAt(float seconds) const;
};

// Parses a clip. With `assets`, the texture must be a texture asset.
[[nodiscard]] Result<AnimationClip> parseAnimation(std::string_view json, std::string_view sourceName,
                                                   const assets::AssetRegistry* assets = nullptr);
[[nodiscard]] std::string serializeAnimation(const AnimationClip& clip);
[[nodiscard]] Result<AnimationClip> loadAnimation(const IFileSystem& fs, const ProjectManifest& project,
                                                  assets::AssetId id);

// Every clip of a project, loaded once (immutable while a game runs).
class AnimationLibrary {
public:
    struct Entry {
        assets::AssetId id;
        std::string name;
        AnimationClip clip;
    };
    void add(Entry entry);
    [[nodiscard]] const Entry* find(assets::AssetId id) const;
    [[nodiscard]] const Entry* findByName(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> names() const;
    [[nodiscard]] std::size_t size() const { return m_entries.size(); }

private:
    std::vector<Entry> m_entries;
};

[[nodiscard]] Result<std::shared_ptr<const AnimationLibrary>> loadAnimationLibrary(const IFileSystem& fs,
                                                                                    const ProjectManifest& project);

// The cells of a sprite sheet of `textureSize` cut into `cellSize` pieces,
// left to right, top to bottom (Studio's frame picker).
[[nodiscard]] std::vector<Rect> gridCells(Vec2i textureSize, Vec2 cellSize);

} // namespace px::project
