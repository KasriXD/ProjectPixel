#include "px/audio/GameAudio.hpp"

#include "px/core/Log.hpp"
#include "px/scene/Components.hpp"

#include <algorithm>

namespace px::audio {

using scene::Entity;

namespace {

constexpr float kSourceFadeSeconds = 0.05f; // an AudioSource that stops or goes away

std::unordered_map<Guid, Entity> entitiesByGuid(const scene::Scene& scene) {
    std::unordered_map<Guid, Entity> out;
    out.reserve(scene.entityCount());
    for (const Entity e : scene.entities()) {
        if (const auto* info = scene.tryGet<scene::EntityInfo>(e)) out.emplace(info->id, e);
    }
    return out;
}

} // namespace

// --- SoundLibrary -------------------------------------------------------------------------

SoundLibrary::SoundLibrary(const IFileSystem& files, const assets::AssetRegistry& assets)
    : m_files(files), m_assets(assets) {}

std::shared_ptr<const Sound> SoundLibrary::get(assets::AssetId id) {
    if (const auto it = m_sounds.find(id); it != m_sounds.end()) return it->second;
    if (id.isNil() || m_failed.contains(id)) return nullptr;
    const auto fail = [&](const std::string& message) -> std::shared_ptr<const Sound> {
        log::error("Sound {}: {}", id.toString(), message);
        m_failed.insert(id);
        return nullptr;
    };
    const auto* entry = m_assets.find(id);
    if (entry == nullptr) return fail("not found in the project's assets");
    if (entry->type != assets::AssetType::Sound) return fail("asset '" + entry->name + "' is not a sound");
    auto bytes = m_files.readBinary(entry->path);
    if (!bytes) return fail(bytes.error().message);
    auto sound = Sound::load(std::move(bytes.value()), entry->name);
    if (!sound) return fail(sound.error().message);
    m_sounds.emplace(id, sound.value());
    return sound.value();
}

void SoundLibrary::preload() {
    for (const auto& entry : m_assets.entries()) {
        if (entry.type == assets::AssetType::Sound) (void)get(entry.id);
    }
}

Listener listenerForScene(const scene::Scene& scene, Vec2 viewport) {
    Listener listener;
    float zoom = 1.0f;
    for (const Entity e : scene.entities()) {
        if (const auto* cam = scene.tryGet<scene::Camera>(e)) {
            listener.position = cam->position;
            zoom = cam->zoom > 0.0f ? cam->zoom : 1.0f;
            break;
        }
    }
    listener.halfView = viewport * (0.5f / zoom);
    return listener;
}

// --- GameAudio ----------------------------------------------------------------------------

GameAudio::GameAudio(Mixer& mixer, SoundLibrary& sounds) : m_mixer(mixer), m_sounds(sounds) {}

GameAudio::~GameAudio() { stopAll(); }

void GameAudio::play(std::span<const runtime::SoundEvent> sounds, const scene::Scene& scene, runtime::PlayerId viewer) {
    std::unordered_map<Guid, Entity> byGuid;
    bool mapped = false;
    for (const runtime::SoundEvent& s : sounds) {
        if (viewer != runtime::kNoPlayer && s.player != runtime::kNoPlayer && s.player != viewer) continue;
        auto sound = m_sounds.get(s.sound);
        if (!sound) continue;
        PlayOptions options;
        options.volume = s.volume;
        options.pitch = s.pitch;
        if (s.position) {
            options.positional = true;
            options.position = *s.position;
        }
        if (!s.entity.isNil()) {
            if (!mapped) {
                byGuid = entitiesByGuid(scene);
                mapped = true;
            }
            // Where the entity is in this scene (a client's view lags the server).
            if (const auto it = byGuid.find(s.entity); it != byGuid.end() && scene.tryGet<scene::Transform>(it->second)) {
                options.positional = true;
                options.position = scene.worldPosition(it->second);
            }
        }
        const VoiceId voice = m_mixer.play(std::move(sound), options);
        if (voice == kNoVoice) continue;
        m_oneShots.push_back(voice);
        if (!s.entity.isNil() && options.positional) m_following.push_back({voice, s.entity});
    }
}

void GameAudio::update(const scene::Scene& scene, const runtime::MusicState& music, const Listener& listener) {
    m_mixer.setListener(listener);
    std::erase_if(m_oneShots, [&](VoiceId v) { return !m_mixer.isPlaying(v); });
    std::erase_if(m_following, [&](const Following& f) { return !m_mixer.isPlaying(f.voice); });
    if (!m_following.empty()) {
        const auto byGuid = entitiesByGuid(scene);
        for (const Following& f : m_following) {
            const auto it = byGuid.find(f.entity);
            // A destroyed entity's sound plays on where it was.
            if (it != byGuid.end() && scene.tryGet<scene::Transform>(it->second) != nullptr) {
                m_mixer.setPosition(f.voice, scene.worldPosition(it->second));
            }
        }
    }
    syncSources(scene);
    syncMusic(music);
}

void GameAudio::syncSources(const scene::Scene& scene) {
    for (auto& [guid, source] : m_sources) source.seen = false;
    for (const Entity e : scene.entities()) {
        const auto* a = scene.tryGet<scene::AudioSource>(e);
        const auto* info = scene.tryGet<scene::EntityInfo>(e);
        if (a == nullptr || info == nullptr || a->sound.isNil()) continue;
        const auto it = m_sources.find(info->id);
        if (!a->playing) {
            if (it != m_sources.end()) {
                if (it->second.voice != kNoVoice) m_mixer.stop(it->second.voice, kSourceFadeSeconds);
                m_sources.erase(it);
            }
            continue;
        }
        Source& s = m_sources[info->id];
        s.seen = true;
        const bool changed = s.sound != a->sound || s.loop != a->loop || s.positional != a->positional ||
                             s.range != a->range || s.pitch != a->pitch;
        const Vec2 position = scene.tryGet<scene::Transform>(e) != nullptr ? scene.worldPosition(e) : Vec2{};
        if (changed && s.voice != kNoVoice) {
            m_mixer.stop(s.voice, kSourceFadeSeconds);
            s.voice = kNoVoice;
        }
        if (changed) s.done = false;
        s.sound = a->sound;
        s.loop = a->loop;
        s.positional = a->positional;
        s.range = a->range;
        s.pitch = a->pitch;
        if (s.voice != kNoVoice && !m_mixer.isPlaying(s.voice)) {
            // A sound that does not loop has played once. (A loop that ended had
            // damaged data: it is not retried every frame either.)
            s.voice = kNoVoice;
            s.done = true;
        }
        if (s.voice == kNoVoice && !s.done) {
            auto sound = m_sounds.get(a->sound);
            if (!sound) {
                s.done = true;
                continue;
            }
            PlayOptions options;
            options.volume = a->volume;
            options.pitch = a->pitch;
            options.loop = a->loop;
            options.positional = a->positional;
            options.position = position;
            options.range = a->range;
            // kNoVoice: every voice is busy with music and loops; tried again next frame.
            s.voice = m_mixer.play(std::move(sound), options);
            continue;
        }
        if (s.voice != kNoVoice) {
            m_mixer.setVolume(s.voice, a->volume);
            if (a->positional) m_mixer.setPosition(s.voice, position);
        }
    }
    // Entities that went away (or lost their AudioSource).
    for (auto it = m_sources.begin(); it != m_sources.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        if (it->second.voice != kNoVoice) m_mixer.stop(it->second.voice, kSourceFadeSeconds);
        it = m_sources.erase(it);
    }
}

void GameAudio::syncMusic(const runtime::MusicState& music) {
    if (music.sound == m_music.sound) {
        if (music.volume != m_music.volume && m_musicVoice != kNoVoice) m_mixer.setVolume(m_musicVoice, music.volume);
        m_music = music;
        return;
    }
    // A different song: the old one fades out while the new one fades in.
    if (m_musicVoice != kNoVoice) m_mixer.stop(m_musicVoice, music.fadeSeconds);
    m_musicVoice = kNoVoice;
    m_music = music;
    if (music.sound.isNil()) return;
    auto sound = m_sounds.get(music.sound);
    if (!sound) return;
    PlayOptions options;
    options.volume = music.volume;
    options.loop = true;
    options.bus = Bus::Music;
    options.fadeIn = music.fadeSeconds;
    m_musicVoice = m_mixer.play(std::move(sound), options);
}

std::size_t GameAudio::sourcesPlaying() const {
    std::size_t n = 0;
    for (const auto& [guid, s] : m_sources) n += s.voice != kNoVoice && m_mixer.isPlaying(s.voice) ? 1 : 0;
    return n;
}

void GameAudio::stopAll(float fadeSeconds) {
    for (const VoiceId v : m_oneShots) m_mixer.stop(v, fadeSeconds);
    for (const auto& [guid, s] : m_sources) {
        if (s.voice != kNoVoice) m_mixer.stop(s.voice, fadeSeconds);
    }
    if (m_musicVoice != kNoVoice) m_mixer.stop(m_musicVoice, fadeSeconds);
    m_oneShots.clear();
    m_following.clear();
    m_sources.clear();
    m_musicVoice = kNoVoice;
    m_music = {};
}

} // namespace px::audio
