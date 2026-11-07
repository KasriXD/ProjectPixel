#include "px/audio/Sound.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

#include "qoa.h"

namespace px::audio {

std::string_view soundFormatName(SoundFormat format) noexcept {
    switch (format) {
        case SoundFormat::Wav: return "WAV";
        case SoundFormat::Vorbis: return "Ogg Vorbis";
        case SoundFormat::Qoa: return "QOA";
    }
    return "?";
}

namespace {

using Bytes = std::span<const std::uint8_t>;

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint64_t be64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

bool startsWith(Bytes b, const char* magic, std::size_t at = 0) {
    const std::size_t n = std::strlen(magic);
    return b.size() >= at + n && std::memcmp(b.data() + at, magic, n) == 0;
}

Status checkLimits(const SoundInfo& info) {
    if (info.channels < 1 || info.channels > kMaxChannels) {
        return makeError(ErrorCode::Unsupported, "{} channels: sounds are mono or stereo", info.channels);
    }
    if (info.sampleRate < kMinSampleRate || info.sampleRate > kMaxSampleRate) {
        return makeError(ErrorCode::Unsupported, "a sample rate of {} Hz (from {} to {} Hz work)", info.sampleRate,
                         kMinSampleRate, kMaxSampleRate);
    }
    if (info.frames == 0) return makeError(ErrorCode::InvalidData, "the sound is empty");
    if (info.seconds() > kMaxSoundSeconds) {
        return makeError(ErrorCode::LimitExceeded, "{:.0f} seconds long (at most {:.0f})", info.seconds(),
                         kMaxSoundSeconds);
    }
    return {};
}

// --- WAV ----------------------------------------------------------------------------------------

struct WavLayout {
    SoundInfo info;
    int bits = 16;
    bool isFloat = false;
    std::size_t dataOffset = 0;
    std::size_t dataBytes = 0;
};

Result<WavLayout> parseWav(Bytes b) {
    if (!startsWith(b, "RIFF") || !startsWith(b, "WAVE", 8)) return makeError(ErrorCode::InvalidData, "not a WAV file");
    WavLayout w;
    w.info.format = SoundFormat::Wav;
    bool haveFormat = false;
    bool haveData = false;
    std::size_t at = 12;
    while (at + 8 <= b.size() && !(haveFormat && haveData)) {
        const std::uint32_t size = le32(b.data() + at + 4);
        const std::size_t body = at + 8;
        const std::size_t available = b.size() - body;
        if (startsWith(b, "fmt ", at)) {
            if (size < 16 || size > available) return makeError(ErrorCode::InvalidData, "a damaged WAV format chunk");
            const std::uint8_t* f = b.data() + body;
            std::uint16_t tag = le16(f);
            w.info.channels = le16(f + 2);
            w.info.sampleRate = static_cast<int>(std::min<std::uint32_t>(le32(f + 4), 1u << 30));
            w.bits = le16(f + 14);
            if (tag == 0xFFFE) { // WAVE_FORMAT_EXTENSIBLE: the real format is in the sub-format GUID
                if (size < 40) return makeError(ErrorCode::InvalidData, "a damaged WAV format chunk");
                tag = le16(f + 24);
            }
            if (tag == 1 && (w.bits == 8 || w.bits == 16 || w.bits == 24 || w.bits == 32)) {
                w.isFloat = false;
            } else if (tag == 3 && w.bits == 32) {
                w.isFloat = true;
            } else {
                return makeError(ErrorCode::Unsupported,
                                 "a WAV encoding the engine does not read (format {}, {} bits): use PCM or 32-bit float",
                                 tag, w.bits);
            }
            haveFormat = true;
        } else if (startsWith(b, "data", at)) {
            w.dataOffset = body;
            w.dataBytes = std::min<std::size_t>(size, available); // tolerate a short last chunk
            haveData = true;
        }
        const std::size_t next = body + static_cast<std::size_t>(size) + (size & 1u);
        if (next <= at) break;
        at = next;
    }
    if (!haveFormat || !haveData) return makeError(ErrorCode::InvalidData, "a WAV file without format or data");
    if (w.info.channels < 1) return makeError(ErrorCode::InvalidData, "a WAV file without channels");
    const std::size_t frameBytes = static_cast<std::size_t>(w.info.channels) * static_cast<std::size_t>(w.bits / 8);
    w.info.frames = w.dataBytes / frameBytes;
    if (auto ok = checkLimits(w.info); !ok) return ok.error();
    return w;
}

float wavSample(const std::uint8_t* p, int bits, bool isFloat) {
    switch (bits) {
        case 8: return (static_cast<float>(p[0]) - 128.0f) / 128.0f;
        case 16: return static_cast<float>(static_cast<std::int16_t>(le16(p))) / 32768.0f;
        case 24: {
            const std::int32_t v = static_cast<std::int32_t>((static_cast<std::uint32_t>(p[0]) << 8) |
                                                             (static_cast<std::uint32_t>(p[1]) << 16) |
                                                             (static_cast<std::uint32_t>(p[2]) << 24));
            return static_cast<float>(v >> 8) / 8388608.0f;
        }
        default: {
            const std::uint32_t raw = le32(p);
            if (isFloat) {
                float f = 0.0f;
                std::memcpy(&f, &raw, 4);
                return std::isfinite(f) ? std::clamp(f, -1.0f, 1.0f) : 0.0f;
            }
            return static_cast<float>(static_cast<double>(static_cast<std::int32_t>(raw)) / 2147483648.0);
        }
    }
}

class WavStream final : public SoundStream {
public:
    WavStream(std::shared_ptr<const std::vector<std::uint8_t>> bytes, WavLayout layout)
        : m_bytes(std::move(bytes)), m_w(layout) {}
    const SoundInfo& info() const override { return m_w.info; }
    std::size_t read(float* out, std::size_t frames) override {
        const std::size_t n = std::min<std::uint64_t>(frames, m_w.info.frames - m_frame);
        const std::size_t step = static_cast<std::size_t>(m_w.bits / 8);
        const std::size_t channels = static_cast<std::size_t>(m_w.info.channels);
        const std::uint8_t* p = m_bytes->data() + m_w.dataOffset + m_frame * step * channels;
        for (std::size_t i = 0; i < n * channels; ++i, p += step) out[i] = wavSample(p, m_w.bits, m_w.isFloat);
        m_frame += n;
        return n;
    }
    void rewind() override { m_frame = 0; }

private:
    std::shared_ptr<const std::vector<std::uint8_t>> m_bytes;
    WavLayout m_w;
    std::uint64_t m_frame = 0;
};

// --- Ogg Vorbis ---------------------------------------------------------------------------------

struct VorbisCloser {
    void operator()(stb_vorbis* v) const { stb_vorbis_close(v); }
};
using VorbisHandle = std::unique_ptr<stb_vorbis, VorbisCloser>;

Result<VorbisHandle> openVorbis(Bytes b, SoundInfo& info) {
    if (b.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return makeError(ErrorCode::LimitExceeded, "the file is too large");
    }
    int error = 0;
    VorbisHandle v(stb_vorbis_open_memory(b.data(), static_cast<int>(b.size()), &error, nullptr));
    if (!v) return makeError(ErrorCode::InvalidData, "a damaged Ogg Vorbis file (error {})", error);
    const stb_vorbis_info vi = stb_vorbis_get_info(v.get());
    info.format = SoundFormat::Vorbis;
    info.sampleRate = static_cast<int>(vi.sample_rate);
    info.channels = vi.channels;
    info.frames = stb_vorbis_stream_length_in_samples(v.get());
    if (auto ok = checkLimits(info); !ok) return ok.error();
    return v;
}

class VorbisStream final : public SoundStream {
public:
    VorbisStream(std::shared_ptr<const std::vector<std::uint8_t>> bytes, VorbisHandle v, SoundInfo info)
        : m_bytes(std::move(bytes)), m_v(std::move(v)), m_info(info) {}
    const SoundInfo& info() const override { return m_info; }
    std::size_t read(float* out, std::size_t frames) override {
        const int channels = m_info.channels;
        const std::size_t limit = std::min<std::size_t>(frames, 1u << 20);
        const int got = stb_vorbis_get_samples_float_interleaved(m_v.get(), channels, out,
                                                                 static_cast<int>(limit) * channels);
        if (got <= 0) return 0;
        for (int i = 0; i < got * channels; ++i) out[i] = std::isfinite(out[i]) ? std::clamp(out[i], -1.0f, 1.0f) : 0.0f;
        return static_cast<std::size_t>(got);
    }
    void rewind() override { (void)stb_vorbis_seek_start(m_v.get()); }

private:
    std::shared_ptr<const std::vector<std::uint8_t>> m_bytes; // the decoder reads them
    VorbisHandle m_v;
    SoundInfo m_info;
};

// --- QOA (decoded here, checking every length; the encoder is qoa.h's) ------------------------------
//
// The tables and the predictor follow qoa.h (MIT, Dominic Szablewski). The
// arithmetic is 64-bit and wraps like the reference decoder's on valid files,
// so damaged files cannot overflow anything.

constexpr int kQoaSliceLen = 20;
constexpr int kQoaSlicesPerFrame = 256;
constexpr int kQoaFrameLen = kQoaSliceLen * kQoaSlicesPerFrame;

constexpr int kQoaDequant[16][8] = {
    {1, -1, 3, -3, 5, -5, 7, -7},
    {5, -5, 18, -18, 32, -32, 49, -49},
    {16, -16, 53, -53, 95, -95, 147, -147},
    {34, -34, 113, -113, 203, -203, 315, -315},
    {63, -63, 210, -210, 378, -378, 588, -588},
    {104, -104, 345, -345, 621, -621, 966, -966},
    {158, -158, 528, -528, 950, -950, 1477, -1477},
    {228, -228, 760, -760, 1368, -1368, 2128, -2128},
    {316, -316, 1053, -1053, 1895, -1895, 2947, -2947},
    {422, -422, 1405, -1405, 2529, -2529, 3934, -3934},
    {548, -548, 1828, -1828, 3290, -3290, 5117, -5117},
    {696, -696, 2320, -2320, 4176, -4176, 6496, -6496},
    {868, -868, 2893, -2893, 5207, -5207, 8099, -8099},
    {1064, -1064, 3548, -3548, 6386, -6386, 9933, -9933},
    {1286, -1286, 4288, -4288, 7718, -7718, 12005, -12005},
    {1536, -1536, 5120, -5120, 9216, -9216, 14336, -14336},
};

struct QoaLms {
    std::int32_t history[4]{};
    std::int32_t weights[4]{};
};

std::int32_t wrapAdd(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

Result<SoundInfo> qoaHeader(Bytes b) {
    if (b.size() < 16 || !startsWith(b, "qoaf")) return makeError(ErrorCode::InvalidData, "not a QOA file");
    SoundInfo info;
    info.format = SoundFormat::Qoa;
    info.frames = be64(b.data()) & 0xffffffffu;
    const std::uint64_t frame = be64(b.data() + 8);
    info.channels = static_cast<int>((frame >> 56) & 0xff);
    info.sampleRate = static_cast<int>((frame >> 32) & 0xffffff);
    if (auto ok = checkLimits(info); !ok) return ok.error();
    return info;
}

class QoaStream final : public SoundStream {
public:
    QoaStream(std::shared_ptr<const std::vector<std::uint8_t>> bytes, SoundInfo info)
        : m_bytes(std::move(bytes)), m_info(info) {
        m_frame.resize(static_cast<std::size_t>(kQoaFrameLen) * static_cast<std::size_t>(m_info.channels));
    }
    const SoundInfo& info() const override { return m_info; }
    std::size_t read(float* out, std::size_t frames) override {
        std::size_t done = 0;
        const std::size_t channels = static_cast<std::size_t>(m_info.channels);
        while (done < frames) {
            if (m_used == m_have) {
                if (m_delivered >= m_info.frames || !decodeFrame()) break;
            }
            const std::size_t n = std::min(frames - done, m_have - m_used);
            for (std::size_t i = 0; i < n * channels; ++i) {
                out[done * channels + i] = static_cast<float>(m_frame[m_used * channels + i]) / 32768.0f;
            }
            done += n;
            m_used += n;
            m_delivered += n;
        }
        return done;
    }
    void rewind() override {
        m_offset = 8;
        m_have = m_used = 0;
        m_delivered = 0;
    }

    // Damaged data ends the sound (playing) or fails it (decodeSound).
    [[nodiscard]] bool damaged() const { return m_damaged; }

private:
    bool decodeFrame() {
        const std::vector<std::uint8_t>& b = *m_bytes;
        const int channels = m_info.channels;
        const std::size_t headerSize = 8 + 16 * static_cast<std::size_t>(channels);
        if (m_offset + headerSize > b.size()) return fail();
        const std::uint64_t header = be64(b.data() + m_offset);
        const int frameChannels = static_cast<int>((header >> 56) & 0xff);
        const int rate = static_cast<int>((header >> 32) & 0xffffff);
        const std::size_t samples = (header >> 16) & 0xffff;
        const std::size_t frameSize = header & 0xffff;
        const std::size_t slices = (samples + kQoaSliceLen - 1) / kQoaSliceLen;
        if (frameChannels != channels || rate != m_info.sampleRate || samples == 0 || samples > kQoaFrameLen ||
            frameSize < headerSize + slices * static_cast<std::size_t>(channels) * 8 || m_offset + frameSize > b.size()) {
            return fail();
        }
        std::size_t p = m_offset + 8;
        QoaLms lms[kMaxChannels];
        for (int c = 0; c < channels; ++c) {
            std::uint64_t history = be64(b.data() + p);
            std::uint64_t weights = be64(b.data() + p + 8);
            p += 16;
            for (int i = 0; i < 4; ++i) {
                lms[c].history[i] = static_cast<std::int16_t>(history >> 48);
                lms[c].weights[i] = static_cast<std::int16_t>(weights >> 48);
                history <<= 16;
                weights <<= 16;
            }
        }
        for (std::size_t start = 0; start < samples; start += kQoaSliceLen) {
            for (int c = 0; c < channels; ++c) {
                std::uint64_t slice = be64(b.data() + p);
                p += 8;
                const int scale = static_cast<int>((slice >> 60) & 0xf);
                slice <<= 4;
                const std::size_t end = std::min(start + kQoaSliceLen, samples);
                for (std::size_t s = start; s < end; ++s) {
                    QoaLms& l = lms[c];
                    std::int64_t predicted = 0;
                    for (int i = 0; i < 4; ++i) predicted += static_cast<std::int64_t>(l.weights[i]) * l.history[i];
                    predicted >>= 13;
                    const int quantized = static_cast<int>((slice >> 61) & 0x7);
                    const int dequantized = kQoaDequant[scale][quantized];
                    const std::int64_t value = std::clamp<std::int64_t>(predicted + dequantized, -32768, 32767);
                    m_frame[s * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] =
                        static_cast<std::int16_t>(value);
                    slice <<= 3;
                    const std::int32_t delta = dequantized >> 4;
                    for (int i = 0; i < 4; ++i) l.weights[i] = wrapAdd(l.weights[i], l.history[i] < 0 ? -delta : delta);
                    l.history[0] = l.history[1];
                    l.history[1] = l.history[2];
                    l.history[2] = l.history[3];
                    l.history[3] = static_cast<std::int32_t>(value);
                }
            }
        }
        m_offset += frameSize;
        m_have = std::min<std::uint64_t>(samples, m_info.frames - m_delivered);
        m_used = 0;
        return m_have > 0;
    }
    bool fail() {
        m_damaged = true;
        return false;
    }

    std::shared_ptr<const std::vector<std::uint8_t>> m_bytes;
    SoundInfo m_info;
    std::vector<std::int16_t> m_frame;
    std::size_t m_offset = 8;
    std::size_t m_have = 0;
    std::size_t m_used = 0;
    std::uint64_t m_delivered = 0;
    bool m_damaged = false;
};

std::shared_ptr<const std::vector<std::uint8_t>> share(Bytes b) {
    return std::make_shared<const std::vector<std::uint8_t>>(b.begin(), b.end());
}

} // namespace

Result<SoundInfo> probeSound(Bytes bytes) {
    if (startsWith(bytes, "RIFF")) {
        auto w = parseWav(bytes);
        if (!w) return w.error();
        return w->info;
    }
    if (startsWith(bytes, "OggS")) {
        SoundInfo info;
        auto v = openVorbis(bytes, info);
        if (!v) return v.error();
        return info;
    }
    if (startsWith(bytes, "qoaf")) return qoaHeader(bytes);
    return makeError(ErrorCode::Unsupported, "not a sound file the engine reads (WAV, Ogg Vorbis or QOA)");
}

Result<std::unique_ptr<SoundStream>> openSound(std::shared_ptr<const std::vector<std::uint8_t>> bytes) {
    if (!bytes) return makeError(ErrorCode::InvalidArgument, "no sound data");
    const Bytes b(*bytes);
    if (startsWith(b, "RIFF")) {
        auto w = parseWav(b);
        if (!w) return w.error();
        return std::unique_ptr<SoundStream>(std::make_unique<WavStream>(std::move(bytes), *w));
    }
    if (startsWith(b, "OggS")) {
        SoundInfo info;
        auto v = openVorbis(b, info);
        if (!v) return v.error();
        return std::unique_ptr<SoundStream>(std::make_unique<VorbisStream>(std::move(bytes), std::move(v.value()), info));
    }
    if (startsWith(b, "qoaf")) {
        auto info = qoaHeader(b);
        if (!info) return info.error();
        return std::unique_ptr<SoundStream>(std::make_unique<QoaStream>(std::move(bytes), *info));
    }
    return makeError(ErrorCode::Unsupported, "not a sound file the engine reads (WAV, Ogg Vorbis or QOA)");
}

Result<PcmSound> decodeSound(Bytes bytes) {
    auto stream = openSound(share(bytes));
    if (!stream) return stream.error();
    const SoundInfo info = (*stream)->info();
    const std::size_t channels = static_cast<std::size_t>(info.channels);
    PcmSound pcm;
    pcm.sampleRate = info.sampleRate;
    pcm.channels = info.channels;
    pcm.samples.reserve(static_cast<std::size_t>(info.frames) * channels);
    std::vector<float> chunk(4096 * channels);
    while (pcm.frames() < info.frames) {
        const std::size_t got = (*stream)->read(chunk.data(), 4096);
        if (got == 0) break;
        for (std::size_t i = 0; i < got * channels; ++i) {
            pcm.samples.push_back(static_cast<std::int16_t>(std::lround(std::clamp(chunk[i], -1.0f, 1.0f) * 32767.0f)));
        }
    }
    if (const auto* qoa = dynamic_cast<const QoaStream*>(stream->get()); qoa != nullptr && qoa->damaged()) {
        return makeError(ErrorCode::InvalidData, "a damaged QOA file");
    }
    // Ogg Vorbis files may be a little shorter or longer than their header
    // says; WAV and QOA must be exactly as long.
    if (pcm.frames() == 0) return makeError(ErrorCode::InvalidData, "the sound has no samples");
    if (info.format != SoundFormat::Vorbis && pcm.frames() != info.frames) {
        return makeError(ErrorCode::InvalidData, "the {} file ends early", soundFormatName(info.format));
    }
    return pcm;
}

namespace {

// Linear conversion to `rate` (only ever down to 48 kHz here).
PcmSound resampled(const PcmSound& in, int rate) {
    PcmSound out;
    out.sampleRate = rate;
    out.channels = in.channels;
    const std::size_t channels = static_cast<std::size_t>(in.channels);
    const double step = static_cast<double>(in.sampleRate) / rate;
    const std::uint64_t frames = static_cast<std::uint64_t>(static_cast<double>(in.frames()) / step);
    out.samples.resize(static_cast<std::size_t>(frames) * channels);
    for (std::uint64_t f = 0; f < frames; ++f) {
        const double at = static_cast<double>(f) * step;
        const std::size_t i0 = static_cast<std::size_t>(at);
        const std::size_t i1 = std::min<std::size_t>(i0 + 1, in.frames() - 1);
        const double t = at - static_cast<double>(i0);
        for (std::size_t c = 0; c < channels; ++c) {
            const double v = in.samples[i0 * channels + c] * (1.0 - t) + in.samples[i1 * channels + c] * t;
            out.samples[f * channels + c] = static_cast<std::int16_t>(std::lround(v));
        }
    }
    return out;
}

} // namespace

Result<std::vector<std::uint8_t>> encodeQoa(const PcmSound& sound) {
    if (sound.channels < 1 || sound.channels > kMaxChannels || sound.frames() == 0) {
        return makeError(ErrorCode::InvalidArgument, "nothing to encode");
    }
    const PcmSound* source = &sound;
    PcmSound converted;
    if (sound.sampleRate > 48000) {
        converted = resampled(sound, 48000);
        source = &converted;
    }
    if (source->frames() > 0xffffffffu) return makeError(ErrorCode::LimitExceeded, "the sound is too long");
    qoa_desc desc{};
    desc.channels = static_cast<unsigned int>(source->channels);
    desc.samplerate = static_cast<unsigned int>(source->sampleRate);
    desc.samples = static_cast<unsigned int>(source->frames());
    unsigned int length = 0;
    void* encoded = qoa_encode(source->samples.data(), &desc, &length);
    if (encoded == nullptr) return makeError(ErrorCode::InvalidData, "QOA encoding failed");
    std::vector<std::uint8_t> out(static_cast<const std::uint8_t*>(encoded), static_cast<const std::uint8_t*>(encoded) + length);
    std::free(encoded);
    return out;
}

std::vector<std::uint8_t> encodeWav(const PcmSound& sound) {
    std::vector<std::uint8_t> out;
    const auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    const auto put16 = [&](std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    const auto tag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
    const std::uint32_t data = static_cast<std::uint32_t>(sound.samples.size() * 2);
    const std::uint16_t channels = static_cast<std::uint16_t>(sound.channels);
    tag("RIFF");
    put32(36 + data);
    tag("WAVE");
    tag("fmt ");
    put32(16);
    put16(1);
    put16(channels);
    put32(static_cast<std::uint32_t>(sound.sampleRate));
    put32(static_cast<std::uint32_t>(sound.sampleRate) * channels * 2);
    put16(static_cast<std::uint16_t>(channels * 2));
    put16(16);
    tag("data");
    put32(data);
    for (const std::int16_t s : sound.samples) put16(static_cast<std::uint16_t>(s));
    return out;
}

Result<std::shared_ptr<const Sound>> Sound::load(std::vector<std::uint8_t> bytes, std::string name) {
    auto shared = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
    auto stream = openSound(shared);
    if (!stream) return makeError(stream.error().code, "Sound '{}': {}", name, stream.error().message);
    auto sound = std::shared_ptr<Sound>(new Sound());
    sound->m_info = (*stream)->info();
    sound->m_name = std::move(name);
    sound->m_bytes = std::move(shared);
    if (sound->m_info.seconds() <= kDecodeWholeSeconds) {
        const std::size_t channels = static_cast<std::size_t>(sound->m_info.channels);
        sound->m_samples.resize(static_cast<std::size_t>(sound->m_info.frames) * channels);
        std::size_t frames = 0;
        while (frames < sound->m_info.frames) {
            const std::size_t got = (*stream)->read(sound->m_samples.data() + frames * channels,
                                                    static_cast<std::size_t>(sound->m_info.frames) - frames);
            if (got == 0) break;
            frames += got;
        }
        if (frames == 0) return makeError(ErrorCode::InvalidData, "Sound '{}' has no samples", sound->m_name);
        sound->m_samples.resize(frames * channels);
        sound->m_info.frames = frames;
    }
    return std::shared_ptr<const Sound>(std::move(sound));
}

} // namespace px::audio
