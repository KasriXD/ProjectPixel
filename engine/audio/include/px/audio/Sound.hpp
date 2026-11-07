#pragma once

#include "px/core/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Sound files (docs/audio.md#sound-files): WAV (PCM or float), Ogg Vorbis
// and QOA. Creators import WAV and Ogg Vorbis; the platform re-encodes every
// sound of a published game as QOA in its sandboxed check, so players'
// devices only ever decode QOA from the platform (a small decoder written
// here with every length checked).
namespace px::audio {

enum class SoundFormat { Wav, Vorbis, Qoa };

[[nodiscard]] std::string_view soundFormatName(SoundFormat format) noexcept;

inline constexpr int kMinSampleRate = 8000;
inline constexpr int kMaxSampleRate = 96000;
inline constexpr int kMaxChannels = 2;
inline constexpr double kMaxSoundSeconds = 600.0; // 10 minutes
// Sounds up to this long are decoded once and kept in memory; longer ones
// (music) are decoded while they play.
inline constexpr double kDecodeWholeSeconds = 15.0;

struct SoundInfo {
    SoundFormat format = SoundFormat::Wav;
    int sampleRate = 0;
    int channels = 0;        // 1 or 2
    std::uint64_t frames = 0; // samples per channel

    [[nodiscard]] double seconds() const {
        return sampleRate > 0 ? static_cast<double>(frames) / static_cast<double>(sampleRate) : 0.0;
    }
};

// Reads the header: format, rate, channels, length. Checks the limits above.
[[nodiscard]] Result<SoundInfo> probeSound(std::span<const std::uint8_t> bytes);

// Decodes a sound while it plays.
class SoundStream {
public:
    virtual ~SoundStream() = default;
    [[nodiscard]] virtual const SoundInfo& info() const = 0;
    // Up to `frames` frames as interleaved floats in [-1, 1] (info().channels
    // per frame). Returns how many were read: 0 at the end (or on damaged data,
    // which ends the sound early).
    virtual std::size_t read(float* out, std::size_t frames) = 0;
    // Back to the start (for looping).
    virtual void rewind() = 0;
};

// The bytes stay shared with the stream (and with other streams of them).
[[nodiscard]] Result<std::unique_ptr<SoundStream>> openSound(std::shared_ptr<const std::vector<std::uint8_t>> bytes);

// A whole sound as 16-bit samples, interleaved.
struct PcmSound {
    int sampleRate = 0;
    int channels = 0;
    std::vector<std::int16_t> samples;

    [[nodiscard]] std::uint64_t frames() const {
        return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
    }
};

// Decodes every sample (checks, re-encoding). Damaged data is an error.
[[nodiscard]] Result<PcmSound> decodeSound(std::span<const std::uint8_t> bytes);

// QOA, as the platform stores sounds of published games. Rates above 48 kHz
// are converted to 48 kHz first.
[[nodiscard]] Result<std::vector<std::uint8_t>> encodeQoa(const PcmSound& sound);

// A 16-bit PCM WAV file (px-headless --audio-out, tests).
[[nodiscard]] std::vector<std::uint8_t> encodeWav(const PcmSound& sound);

// A sound ready to play: its bytes and, when it is short, every sample
// decoded (as floats). Shared between the voices playing it.
class Sound {
public:
    [[nodiscard]] static Result<std::shared_ptr<const Sound>> load(std::vector<std::uint8_t> bytes, std::string name);

    [[nodiscard]] const SoundInfo& info() const { return m_info; }
    [[nodiscard]] const std::string& name() const { return m_name; }
    // Whole sound in memory: samples() has info().frames * channels values.
    [[nodiscard]] bool decoded() const { return !m_samples.empty(); }
    [[nodiscard]] const std::vector<float>& samples() const { return m_samples; }
    // Long sounds: a new stream for each voice.
    [[nodiscard]] Result<std::unique_ptr<SoundStream>> stream() const { return openSound(m_bytes); }

private:
    SoundInfo m_info;
    std::string m_name;
    std::shared_ptr<const std::vector<std::uint8_t>> m_bytes;
    std::vector<float> m_samples;
};

} // namespace px::audio
