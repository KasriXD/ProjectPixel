#pragma once

#include "px/assets/AssetRegistry.hpp"
#include "px/audio/Mixer.hpp"
#include "px/audio/Sound.hpp"
#include "px/core/FileSystem.hpp"
#include "px/runtime/GameSession.hpp"
#include "px/scene/Scene.hpp"

#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Plays a running game: the sounds its scripts start, its AudioSource
// components and its music (docs/audio.md). The same code serves a game
// played alone, a multiplayer client's view and Studio's play mode.
namespace px::audio {

// A game's sounds, read and decoded on first use. A sound that is missing or
// damaged is reported once and then stays silent.
class SoundLibrary {
public:
    SoundLibrary(const IFileSystem& files, const assets::AssetRegistry& assets);

    // nullptr if the sound cannot be played.
    [[nodiscard]] std::shared_ptr<const Sound> get(assets::AssetId id);
    // Decodes every short sound now (while a game loads, not while it plays).
    void preload();
    [[nodiscard]] std::size_t loaded() const { return m_sounds.size(); }

private:
    const IFileSystem& m_files;
    const assets::AssetRegistry& m_assets;
    std::unordered_map<assets::AssetId, std::shared_ptr<const Sound>> m_sounds;
    std::unordered_set<assets::AssetId> m_failed;
};

// Where the scene's camera looks from, for hearing: its centre and half the
// view in world units. `viewport` is the virtual resolution.
[[nodiscard]] Listener listenerForScene(const scene::Scene& scene, Vec2 viewport);

class GameAudio {
public:
    GameAudio(Mixer& mixer, SoundLibrary& sounds);
    ~GameAudio(); // fades out everything it started
    GameAudio(const GameAudio&) = delete;
    GameAudio& operator=(const GameAudio&) = delete;

    // Starts the sounds of a tick. `viewer` is the player this game is shown
    // to: sounds for someone else are skipped (kNoPlayer: play them all).
    // Sounds of an entity follow it while they play (see update).
    void play(std::span<const runtime::SoundEvent> sounds, const scene::Scene& scene,
              runtime::PlayerId viewer = runtime::kNoPlayer);

    // Once per drawn frame: moves the listener to `listener`, keeps the
    // AudioSource components and entity sounds in step with `scene`, and fades
    // to `music` when it changes.
    void update(const scene::Scene& scene, const runtime::MusicState& music, const Listener& listener);

    // Fades everything out (a level restarts, the game ends).
    void stopAll(float fadeSeconds = 0.1f);

    [[nodiscard]] std::size_t sourcesPlaying() const;
    [[nodiscard]] const runtime::MusicState& music() const { return m_music; }

private:
    struct Source {
        VoiceId voice = kNoVoice;
        assets::AssetId sound;
        bool loop = true;
        bool positional = true;
        float range = 0.0f;
        float pitch = 1.0f;
        bool done = false; // played once (no loop); again when `playing` turns true again
        bool seen = false;
    };
    struct Following {
        VoiceId voice;
        Guid entity;
    };

    void syncSources(const scene::Scene& scene);
    void syncMusic(const runtime::MusicState& music);

    Mixer& m_mixer;
    SoundLibrary& m_sounds;
    std::unordered_map<Guid, Source> m_sources; // AudioSource voices by entity
    std::vector<Following> m_following;         // entity sounds still playing
    std::vector<VoiceId> m_oneShots;            // all one-shot voices (for stopAll)
    runtime::MusicState m_music;
    VoiceId m_musicVoice = kNoVoice;
};

} // namespace px::audio
