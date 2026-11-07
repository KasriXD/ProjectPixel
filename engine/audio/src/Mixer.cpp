#include "px/audio/Mixer.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace px::audio {

namespace {

constexpr std::size_t kBlock = 256;      // frames mixed with one set of gains (ramped across it)
constexpr std::size_t kStreamChunk = 2048; // frames decoded at a time for long sounds
constexpr float kMinFadeSeconds = 0.005f; // stopping never clicks

float clampVolume(float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 4.0f) : 0.0f; }

// Soft limit: unchanged below 0.9, approaching 1 above.
float limit(float x) {
    const float a = std::fabs(x);
    if (a <= 0.9f) return x;
    const float y = 0.9f + 0.1f * std::tanh((a - 0.9f) / 0.1f);
    return std::copysign(std::min(y, 1.0f), x);
}

} // namespace

Placement placeSound(const Listener& listener, Vec2 position, float range) {
    const Vec2 d = position - listener.position;
    const float hw = std::max(listener.halfView.x, 1.0f);
    Placement p;
    if (range > 0.0f) {
        p.gain = std::clamp(1.0f - length(d) / range, 0.0f, 1.0f);
    } else {
        const float hh = std::max(listener.halfView.y, 1.0f);
        const Vec2 outside{std::max(std::fabs(d.x) - hw, 0.0f), std::max(std::fabs(d.y) - hh, 0.0f)};
        p.gain = std::clamp(1.0f - length(outside) / hw, 0.0f, 1.0f);
    }
    p.pan = std::clamp(d.x / (hw * 1.5f), -1.0f, 1.0f) * 0.75f;
    return p;
}

struct Mixer::Voice {
    VoiceId id = kNoVoice;
    std::shared_ptr<const Sound> sound;
    std::unique_ptr<SoundStream> stream; // long sounds
    int channels = 1;
    std::uint64_t frames = 0;            // decoded sounds
    // Long sounds: a window of decoded frames [windowStart, windowStart + windowFrames).
    std::vector<float> window;
    std::uint64_t windowStart = 0;
    std::size_t windowFrames = 0;
    bool streamEnded = false;

    double position = 0.0; // in source frames (keeps counting across loops for streams)
    double step = 1.0;
    bool loop = false;
    Bus bus = Bus::Effects;
    float volume = 1.0f;
    float pan = 0.0f;
    bool positional = false;
    Vec2 at{};
    float range = 0.0f;
    float envelope = 1.0f; // fades
    float envelopeStep = 0.0f; // per frame
    bool stopping = false;
    bool finished = false;
    float gainL = 0.0f; // gains applied at the end of the last block (ramped from)
    float gainR = 0.0f;
    bool fresh = true;
    std::uint64_t order = 0;

    // Frame `index` of the source into l, r. False past the end.
    bool frameAt(std::uint64_t index, float& l, float& r) {
        if (sound->decoded()) {
            if (index >= frames) {
                if (!loop) return false;
                index %= frames;
            }
            const float* s = sound->samples().data() + index * static_cast<std::size_t>(channels);
            l = s[0];
            r = channels == 2 ? s[1] : s[0];
            return true;
        }
        while (index >= windowStart + windowFrames) {
            if (streamEnded) return false;
            refill(index);
        }
        if (index < windowStart) index = windowStart; // (never: the window only moves forward)
        const float* s = window.data() + (index - windowStart) * static_cast<std::size_t>(channels);
        l = s[0];
        r = channels == 2 ? s[1] : s[0];
        return true;
    }

    void refill(std::uint64_t needed) {
        const std::size_t ch = static_cast<std::size_t>(channels);
        // Keep the last frame (it may still be interpolated from).
        std::size_t keep = 0;
        if (windowFrames > 0 && needed == windowStart + windowFrames) {
            std::copy_n(window.begin() + static_cast<std::ptrdiff_t>((windowFrames - 1) * ch), ch, window.begin());
            keep = 1;
            windowStart += windowFrames - 1;
        } else {
            windowStart += windowFrames;
        }
        std::size_t got = stream->read(window.data() + keep * ch, kStreamChunk);
        if (got == 0 && loop) {
            stream->rewind();
            got = stream->read(window.data() + keep * ch, kStreamChunk);
        }
        if (got == 0) streamEnded = true;
        windowFrames = keep + got;
    }
};

Mixer::Mixer(int sampleRate) : m_rate(std::clamp(sampleRate, kMinSampleRate, 192000)) {
    m_scratch.resize(kBlock * 2);
}

Mixer::~Mixer() = default;

Mixer::Voice* Mixer::find(VoiceId id) {
    for (auto& v : m_voices) {
        if (v->id == id) return v.get();
    }
    return nullptr;
}

const Mixer::Voice* Mixer::find(VoiceId id) const {
    for (const auto& v : m_voices) {
        if (v->id == id) return v.get();
    }
    return nullptr;
}

VoiceId Mixer::play(std::shared_ptr<const Sound> sound, const PlayOptions& options) {
    if (!sound || sound->info().frames == 0) return kNoVoice;
    auto voice = std::make_unique<Voice>();
    voice->channels = sound->info().channels;
    if (sound->decoded()) {
        voice->frames = sound->samples().size() / static_cast<std::size_t>(voice->channels);
        if (voice->frames == 0) return kNoVoice;
    } else {
        auto stream = sound->stream();
        if (!stream) return kNoVoice;
        voice->stream = std::move(stream.value());
        voice->window.resize((kStreamChunk + 1) * static_cast<std::size_t>(voice->channels));
    }
    voice->sound = std::move(sound);
    const float pitch = std::isfinite(options.pitch) ? std::clamp(options.pitch, 0.25f, 4.0f) : 1.0f;
    voice->step = static_cast<double>(voice->sound->info().sampleRate) / m_rate * pitch;
    voice->loop = options.loop;
    voice->bus = options.bus;
    voice->volume = clampVolume(options.volume);
    voice->pan = std::isfinite(options.pan) ? std::clamp(options.pan, -1.0f, 1.0f) : 0.0f;
    voice->positional = options.positional;
    voice->at = options.position;
    voice->range = std::isfinite(options.range) ? std::max(options.range, 0.0f) : 0.0f;
    if (options.fadeIn > 0.0f && std::isfinite(options.fadeIn)) {
        voice->envelope = 0.0f;
        voice->envelopeStep = 1.0f / (options.fadeIn * static_cast<float>(m_rate));
    }

    std::lock_guard lock(m_mutex);
    std::erase_if(m_voices, [](const auto& v) { return v->finished; });
    if (m_voices.size() >= kMaxVoices) {
        // Room for it: the oldest effect that does not loop, else the oldest effect.
        auto victim = m_voices.end();
        for (bool looping : {false, true}) {
            for (auto it = m_voices.begin(); it != m_voices.end(); ++it) {
                if ((*it)->bus != Bus::Effects || (*it)->loop != looping) continue;
                if (victim == m_voices.end() || (*it)->order < (*victim)->order) victim = it;
            }
            if (victim != m_voices.end()) break;
        }
        if (victim == m_voices.end()) return kNoVoice;
        m_voices.erase(victim);
    }
    voice->id = m_nextId++;
    if (m_nextId == kNoVoice) m_nextId = 1;
    voice->order = m_order++;
    const VoiceId id = voice->id;
    m_voices.push_back(std::move(voice));
    return id;
}

void Mixer::stop(VoiceId id, float fadeSeconds) {
    std::lock_guard lock(m_mutex);
    if (Voice* v = find(id)) {
        const float seconds = std::isfinite(fadeSeconds) ? std::max(fadeSeconds, kMinFadeSeconds) : kMinFadeSeconds;
        v->stopping = true;
        v->envelopeStep = -std::max(v->envelope, 0.001f) / (seconds * static_cast<float>(m_rate));
    }
}

void Mixer::setVolume(VoiceId id, float volume) {
    std::lock_guard lock(m_mutex);
    if (Voice* v = find(id)) v->volume = clampVolume(volume);
}

void Mixer::setPosition(VoiceId id, Vec2 position) {
    std::lock_guard lock(m_mutex);
    if (Voice* v = find(id)) v->at = position;
}

bool Mixer::isPlaying(VoiceId id) const {
    std::lock_guard lock(m_mutex);
    const Voice* v = find(id);
    return v != nullptr && !v->finished && !v->stopping;
}

void Mixer::stopAll(float fadeSeconds) {
    std::lock_guard lock(m_mutex);
    const float seconds = std::isfinite(fadeSeconds) ? std::max(fadeSeconds, kMinFadeSeconds) : kMinFadeSeconds;
    for (auto& v : m_voices) {
        v->stopping = true;
        v->envelopeStep = -std::max(v->envelope, 0.001f) / (seconds * static_cast<float>(m_rate));
    }
}

std::size_t Mixer::activeVoices() const {
    std::lock_guard lock(m_mutex);
    return static_cast<std::size_t>(
        std::count_if(m_voices.begin(), m_voices.end(), [](const auto& v) { return !v->finished; }));
}

void Mixer::setListener(const Listener& listener) {
    std::lock_guard lock(m_mutex);
    m_listener = listener;
}

void Mixer::setMasterVolume(float volume) {
    std::lock_guard lock(m_mutex);
    m_master = std::isfinite(volume) ? std::clamp(volume, 0.0f, 1.0f) : 0.0f;
}

void Mixer::setBusVolume(Bus bus, float volume) {
    std::lock_guard lock(m_mutex);
    m_bus[static_cast<std::size_t>(bus)] = std::isfinite(volume) ? std::clamp(volume, 0.0f, 1.0f) : 0.0f;
}

float Mixer::busVolume(Bus bus) const {
    std::lock_guard lock(m_mutex);
    return m_bus[static_cast<std::size_t>(bus)];
}

float Mixer::masterVolume() const {
    std::lock_guard lock(m_mutex);
    return m_master;
}

void Mixer::render(float* out, std::size_t frames) {
    std::lock_guard lock(m_mutex);
    std::size_t done = 0;
    while (done < frames) {
        const std::size_t n = std::min(kBlock, frames - done);
        renderBlock(out + done * 2, n);
        done += n;
    }
    std::erase_if(m_voices, [](const auto& v) { return v->finished; });
}

void Mixer::renderBlock(float* out, std::size_t frames) {
    std::fill_n(out, frames * 2, 0.0f);
    for (auto& vp : m_voices) {
        Voice& v = *vp;
        if (v.finished) continue;
        // Gains for the end of this block; the block ramps to them.
        float gain = v.volume * m_bus[static_cast<std::size_t>(v.bus)] * m_master;
        float pan = v.pan;
        if (v.positional) {
            const Placement p = placeSound(m_listener, v.at, v.range);
            gain *= p.gain;
            pan = p.pan;
        }
        float targetL = gain;
        float targetR = gain;
        if (v.channels == 1) {
            const float theta = (pan + 1.0f) * std::numbers::pi_v<float> / 4.0f;
            targetL = gain * std::cos(theta) * std::numbers::sqrt2_v<float>;
            targetR = gain * std::sin(theta) * std::numbers::sqrt2_v<float>;
        } else {
            targetL *= std::min(1.0f, 1.0f - pan);
            targetR *= std::min(1.0f, 1.0f + pan);
        }
        if (v.fresh) {
            v.gainL = targetL;
            v.gainR = targetR;
            v.fresh = false;
        }
        const float stepL = (targetL - v.gainL) / static_cast<float>(frames);
        const float stepR = (targetR - v.gainR) / static_cast<float>(frames);
        float gl = v.gainL;
        float gr = v.gainR;
        for (std::size_t f = 0; f < frames; ++f) {
            const auto index = static_cast<std::uint64_t>(v.position);
            const float t = static_cast<float>(v.position - static_cast<double>(index));
            float l0 = 0.0f;
            float r0 = 0.0f;
            if (!v.frameAt(index, l0, r0)) {
                v.finished = true;
                break;
            }
            float l1 = l0;
            float r1 = r0;
            (void)v.frameAt(index + 1, l1, r1); // past the end: hold the last frame
            const float env = v.envelope;
            gl += stepL;
            gr += stepR;
            out[f * 2] += (l0 + (l1 - l0) * t) * gl * env;
            out[f * 2 + 1] += (r0 + (r1 - r0) * t) * gr * env;
            v.position += v.step;
            if (v.envelopeStep != 0.0f) {
                v.envelope += v.envelopeStep;
                if (v.envelope >= 1.0f) {
                    v.envelope = 1.0f;
                    v.envelopeStep = 0.0f;
                } else if (v.envelope <= 0.0f) {
                    v.envelope = 0.0f;
                    if (v.stopping) {
                        v.finished = true;
                        break;
                    }
                }
            }
        }
        v.gainL = targetL;
        v.gainR = targetR;
        // Decoded loops keep their position small.
        if (v.loop && v.sound->decoded() && v.frames > 0 && v.position >= static_cast<double>(v.frames)) {
            v.position = std::fmod(v.position, static_cast<double>(v.frames));
        }
    }
    for (std::size_t i = 0; i < frames * 2; ++i) out[i] = limit(out[i]);
}

} // namespace px::audio
