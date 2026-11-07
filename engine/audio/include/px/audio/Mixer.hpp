#pragma once

#include "px/audio/Sound.hpp"
#include "px/core/Math.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// Mixes playing sounds into stereo samples (docs/audio.md#how-sounds-are-mixed).
// No device here: the platform layer asks for samples (SdlAudio), tests and
// px-headless --audio-out render them directly. Thread-safe: the device's
// thread renders while the game starts and stops voices.
namespace px::audio {

enum class Bus : std::uint8_t { Effects, Music };

using VoiceId = std::uint32_t;
inline constexpr VoiceId kNoVoice = 0;

struct PlayOptions {
    float volume = 1.0f; // 0..4
    float pitch = 1.0f;  // playback speed: 2 is an octave up and twice as fast (0.25..4)
    float pan = 0.0f;    // not positional: -1 left .. 1 right
    bool loop = false;
    Bus bus = Bus::Effects;
    // Heard from a point in the world: louder near the listener, panned to its side.
    bool positional = false;
    Vec2 position{};
    // Positional: silent this far from the listener (world units); 0 means
    // "what is on screen": full volume inside the view, fading out over half
    // a view beyond its edge.
    float range = 0.0f;
    float fadeIn = 0.0f; // seconds
};

// Where the player hears from: the camera's centre and half the view.
struct Listener {
    Vec2 position{};
    Vec2 halfView{160.0f, 90.0f};
};

// Gain and pan of a positional sound for a listener (what render applies).
struct Placement {
    float gain = 1.0f;
    float pan = 0.0f;
};
[[nodiscard]] Placement placeSound(const Listener& listener, Vec2 position, float range);

class Mixer {
public:
    static constexpr std::size_t kMaxVoices = 32;

    explicit Mixer(int sampleRate = 48000);
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    [[nodiscard]] int sampleRate() const { return m_rate; }

    // Starts a sound. With every voice busy, the oldest sound effect that
    // does not loop makes room (music is never cut off). kNoVoice: not played.
    VoiceId play(std::shared_ptr<const Sound> sound, const PlayOptions& options);
    // Stops after fading out (a few milliseconds at least, so it does not click).
    void stop(VoiceId voice, float fadeSeconds = 0.0f);
    void setVolume(VoiceId voice, float volume);
    void setPosition(VoiceId voice, Vec2 position);
    [[nodiscard]] bool isPlaying(VoiceId voice) const;
    void stopAll(float fadeSeconds = 0.0f);
    [[nodiscard]] std::size_t activeVoices() const;

    void setListener(const Listener& listener);
    // 0..1 each.
    void setMasterVolume(float volume);
    void setBusVolume(Bus bus, float volume);
    [[nodiscard]] float busVolume(Bus bus) const;
    [[nodiscard]] float masterVolume() const;

    // Writes `frames` stereo frames (interleaved left, right) in [-1, 1].
    void render(float* out, std::size_t frames);

private:
    struct Voice;
    void renderBlock(float* out, std::size_t frames);
    [[nodiscard]] Voice* find(VoiceId id);
    [[nodiscard]] const Voice* find(VoiceId id) const;

    int m_rate;
    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<Voice>> m_voices;
    VoiceId m_nextId = 1;
    std::uint64_t m_order = 0;
    Listener m_listener;
    float m_master = 1.0f;
    std::array<float, 2> m_bus{1.0f, 1.0f};
    std::vector<float> m_scratch;
};

} // namespace px::audio
