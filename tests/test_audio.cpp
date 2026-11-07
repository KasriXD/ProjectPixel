// Sound files and mixing (docs/audio.md).
#include "px/audio/GameAudio.hpp"
#include "px/audio/Mixer.hpp"
#include "px/audio/Sound.hpp"
#include "px/core/FileSystem.hpp"

#include <doctest.h>

#include <cmath>
#include <numbers>
#include <random>

using namespace px;
using namespace px::audio;

namespace {

PcmSound sine(int rate, int channels, double seconds, double frequency, double amplitude = 0.5) {
    PcmSound p;
    p.sampleRate = rate;
    p.channels = channels;
    const auto frames = static_cast<std::size_t>(seconds * rate);
    for (std::size_t f = 0; f < frames; ++f) {
        const double v = amplitude * std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(f) / rate);
        for (int c = 0; c < channels; ++c) {
            p.samples.push_back(static_cast<std::int16_t>(std::lround(v * (c == 1 ? -32767.0 : 32767.0))));
        }
    }
    return p;
}

// A sound at a constant level (easy to check after mixing).
PcmSound constant(int rate, double seconds, std::int16_t value) {
    PcmSound p;
    p.sampleRate = rate;
    p.channels = 1;
    p.samples.assign(static_cast<std::size_t>(seconds * rate), value);
    return p;
}

std::shared_ptr<const Sound> loadPcm(const PcmSound& pcm, std::string name = "test") {
    auto sound = Sound::load(encodeWav(pcm), std::move(name));
    REQUIRE(sound.ok());
    return sound.value();
}

std::vector<float> renderSeconds(Mixer& m, double seconds) {
    std::vector<float> out(static_cast<std::size_t>(seconds * m.sampleRate()) * 2);
    m.render(out.data(), out.size() / 2);
    return out;
}

double rms(std::span<const float> s, std::size_t channel, std::size_t from = 0, std::size_t to = SIZE_MAX) {
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t f = from; f < std::min(to, s.size() / 2); ++f, ++n) sum += s[f * 2 + channel] * s[f * 2 + channel];
    return n > 0 ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
}

std::vector<std::uint8_t> wavHeader(std::uint16_t format, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits,
                                    std::span<const std::uint8_t> data) {
    PcmSound empty;
    empty.sampleRate = static_cast<int>(rate);
    empty.channels = channels;
    auto w = encodeWav(empty);
    w[20] = static_cast<std::uint8_t>(format);
    w[21] = static_cast<std::uint8_t>(format >> 8);
    w[34] = static_cast<std::uint8_t>(bits);
    w[40] = static_cast<std::uint8_t>(data.size());
    w[41] = static_cast<std::uint8_t>(data.size() >> 8);
    w.insert(w.end(), data.begin(), data.end());
    return w;
}

} // namespace

TEST_CASE("Sounds: WAV files") {
    const PcmSound pcm = sine(22050, 2, 0.25, 440.0);
    const auto wav = encodeWav(pcm);
    auto info = probeSound(wav);
    REQUIRE(info.ok());
    CHECK(info->format == SoundFormat::Wav);
    CHECK(info->sampleRate == 22050);
    CHECK(info->channels == 2);
    CHECK(info->frames == pcm.frames());
    auto back = decodeSound(wav);
    REQUIRE(back.ok());
    CHECK(back->samples == pcm.samples);

    // 8-bit, 24-bit and float samples.
    const std::uint8_t eight[] = {128, 255, 0, 192};
    auto e = decodeSound(wavHeader(1, 1, 8000, 8, eight));
    REQUIRE(e.ok());
    CHECK(e->samples[0] == 0);
    CHECK(e->samples[1] > 32000);
    CHECK(e->samples[2] < -32000);
    const std::uint8_t twentyFour[] = {0, 0, 0x40, 0, 0, 0xC0}; // +0.5, -0.5
    auto t = decodeSound(wavHeader(1, 1, 8000, 24, twentyFour));
    REQUIRE(t.ok());
    CHECK(t->samples[0] == 16384);
    CHECK(t->samples[1] == -16384);
    float floats[] = {0.25f, -1.5f}; // out of range: clamped
    auto f = decodeSound(wavHeader(3, 1, 8000, 32, std::span(reinterpret_cast<const std::uint8_t*>(floats), 8)));
    REQUIRE(f.ok());
    CHECK(f->samples[0] == 8192);
    CHECK(f->samples[1] == -32767);

    // What is refused.
    CHECK_FALSE(probeSound(std::vector<std::uint8_t>(wav.begin(), wav.begin() + 20)).ok());
    CHECK(probeSound(wavHeader(2, 1, 8000, 4, eight)).error().code == ErrorCode::Unsupported); // ADPCM
    CHECK(probeSound(wavHeader(1, 3, 8000, 8, eight)).error().code == ErrorCode::Unsupported); // 3 channels
    CHECK(probeSound(wavHeader(1, 1, 4000, 8, eight)).error().code == ErrorCode::Unsupported); // 4 kHz
    CHECK(probeSound(wavHeader(1, 1, 8000, 8, {})).error().code == ErrorCode::InvalidData);    // empty
    auto huge = wavHeader(1, 1, 8000, 8, eight);
    huge[40] = huge[41] = huge[42] = 0xff; // a data chunk of 16 MB (more than the file has): what is there counts
    CHECK(probeSound(huge).ok());
    CHECK_FALSE(probeSound(std::vector<std::uint8_t>{'O', 'g', 'g', 'S', 0, 0}).ok());
    CHECK(probeSound(std::vector<std::uint8_t>{1, 2, 3, 4, 5}).error().code == ErrorCode::Unsupported);
}

TEST_CASE("Sounds: Ogg Vorbis") {
    auto bytes = readLocalFile(std::filesystem::path(PX_TEST_DATA_DIR) / "beep.ogg");
    REQUIRE(bytes.ok());
    auto info = probeSound(bytes.value());
    REQUIRE(info.ok());
    CHECK(info->format == SoundFormat::Vorbis);
    CHECK(info->sampleRate == 44100);
    CHECK(info->channels == 2);
    CHECK(info->seconds() == doctest::Approx(0.5).epsilon(0.02));
    auto pcm = decodeSound(bytes.value());
    REQUIRE(pcm.ok());
    double sum = 0.0;
    for (auto s : pcm->samples) sum += static_cast<double>(s) * s;
    const double level = std::sqrt(sum / static_cast<double>(pcm->samples.size())) / 32768.0;
    CHECK(level == doctest::Approx(0.5 / std::numbers::sqrt2).epsilon(0.1)); // a sine at half volume
    auto damaged = bytes.value();
    damaged.resize(200);
    CHECK_FALSE(decodeSound(damaged).ok());
}

TEST_CASE("Sounds: QOA, as published games carry them") {
    const PcmSound pcm = sine(44100, 2, 1.3, 523.25);
    auto qoa = encodeQoa(pcm);
    REQUIRE(qoa.ok());
    CHECK(qoa->size() < pcm.samples.size() * 2 / 4); // about 5 times smaller than 16-bit PCM
    auto info = probeSound(qoa.value());
    REQUIRE(info.ok());
    CHECK(info->format == SoundFormat::Qoa);
    CHECK(info->frames == pcm.frames());
    auto back = decodeSound(qoa.value());
    REQUIRE(back.ok());
    REQUIRE(back->samples.size() == pcm.samples.size());
    double error = 0.0;
    double signal = 0.0;
    for (std::size_t i = 0; i < pcm.samples.size(); ++i) {
        const double d = static_cast<double>(back->samples[i]) - pcm.samples[i];
        error += d * d;
        signal += static_cast<double>(pcm.samples[i]) * pcm.samples[i];
    }
    CHECK(10.0 * std::log10(signal / error) > 30.0); // better than 30 dB signal to noise

    // 96 kHz becomes 48 kHz.
    auto high = encodeQoa(sine(96000, 1, 0.2, 1000.0));
    REQUIRE(high.ok());
    CHECK(probeSound(high.value())->sampleRate == 48000);
    CHECK(probeSound(high.value())->frames == 9600);

    // Damaged files are refused (and nothing crashes on random changes).
    auto shortened = qoa.value();
    shortened.resize(shortened.size() / 2);
    CHECK_FALSE(decodeSound(shortened).ok());
    std::mt19937 random(7);
    for (int round = 0; round < 300; ++round) {
        auto bytes = qoa.value();
        for (int i = 0; i < 8; ++i) bytes[8 + random() % (bytes.size() - 8)] = static_cast<std::uint8_t>(random());
        (void)decodeSound(bytes);
        auto sound = Sound::load(bytes, "fuzz");
        if (sound) {
            Mixer m(48000);
            (void)m.play(sound.value(), {});
            (void)renderSeconds(m, 0.05);
        }
    }
}

TEST_CASE("Mixer: volume, buses, pan and pitch") {
    Mixer m(48000);
    const auto half = loadPcm(constant(48000, 0.5, 16384)); // 0.5
    CHECK(half->decoded());
    const VoiceId a = m.play(half, {});
    REQUIRE(a != kNoVoice);
    CHECK(m.isPlaying(a));
    auto out = renderSeconds(m, 0.1);
    CHECK(out[1000 * 2] == doctest::Approx(0.5).epsilon(0.001));     // a mono sound in the middle: both sides
    CHECK(out[1000 * 2 + 1] == doctest::Approx(0.5).epsilon(0.001));
    m.setMasterVolume(0.5f);
    m.setBusVolume(Bus::Effects, 0.5f);
    out = renderSeconds(m, 0.1);
    CHECK(out[1000 * 2] == doctest::Approx(0.125).epsilon(0.01));
    m.setVolume(a, 0.0f);
    out = renderSeconds(m, 0.1);
    CHECK(std::fabs(out[1000 * 2]) < 1e-6f);
    m.stopAll();
    (void)renderSeconds(m, 0.05);
    CHECK(m.activeVoices() == 0);

    // Pan: a hard-left sound is (almost) silent on the right.
    Mixer p(48000);
    PlayOptions left;
    left.pan = -1.0f;
    (void)p.play(half, left);
    out = renderSeconds(p, 0.05);
    CHECK(out[500 * 2] > 0.6f);
    CHECK(std::fabs(out[500 * 2 + 1]) < 1e-4f);

    // Pitch 2 plays twice as fast: a 0.5 s sound is over after 0.25 s.
    Mixer q(48000);
    PlayOptions fast;
    fast.pitch = 2.0f;
    const VoiceId f = q.play(half, fast);
    out = renderSeconds(q, 0.3);
    CHECK(out[int(0.2 * 48000) * 2] != 0.0f);
    CHECK(out[int(0.27 * 48000) * 2] == 0.0f);
    CHECK_FALSE(q.isPlaying(f));

    // A sound at another rate is converted: 1 s at 22050 Hz still lasts 1 s.
    Mixer r(48000);
    (void)r.play(loadPcm(constant(22050, 1.0, 16384)), {});
    out = renderSeconds(r, 1.2);
    CHECK(out[int(0.95 * 48000) * 2] != 0.0f);
    CHECK(out[int(1.05 * 48000) * 2] == 0.0f);

    // Loud sounds together are limited, never past 1.
    Mixer loud(48000);
    for (int i = 0; i < 8; ++i) (void)loud.play(loadPcm(constant(48000, 0.2, 30000)), {});
    out = renderSeconds(loud, 0.1);
    for (float s : out) CHECK(std::fabs(s) <= 1.0f);
    CHECK(out[1000] > 0.95f);
}

TEST_CASE("Mixer: loops, fades and running out of voices") {
    Mixer m(48000);
    const auto tone = loadPcm(sine(48000, 1, 0.1, 440.0));
    PlayOptions looped;
    looped.loop = true;
    looped.fadeIn = 0.05f;
    const VoiceId loop = m.play(tone, looped);
    auto out = renderSeconds(m, 1.0);
    CHECK(m.isPlaying(loop));
    CHECK(rms(out, 0, 0, 1200) < rms(out, 0, 12000, 24000)); // fading in
    CHECK(rms(out, 0, 40000, 48000) == doctest::Approx(0.5 / std::numbers::sqrt2).epsilon(0.05)); // still going after 10 rounds
    m.stop(loop, 0.1f);
    out = renderSeconds(m, 0.2);
    CHECK_FALSE(m.isPlaying(loop));
    CHECK(rms(out, 0, 0, 1000) > rms(out, 0, 3000, 4800)); // fading out
    CHECK(rms(out, 0, 5000) == 0.0);
    // Stopping at once still ramps down over a few milliseconds (no click).
    const VoiceId again = m.play(loadPcm(constant(48000, 1.0, 16384)), {});
    (void)renderSeconds(m, 0.05);
    m.stop(again);
    out = renderSeconds(m, 0.02);
    CHECK(out[0] > 0.4f);
    CHECK(out[100 * 2] < 0.4f);
    CHECK(out[300 * 2] == 0.0f);

    // 32 voices at most: the oldest effect makes room; music stays.
    Mixer full(48000);
    PlayOptions music;
    music.bus = Bus::Music;
    music.loop = true;
    const VoiceId song = full.play(tone, music);
    const VoiceId first = full.play(tone, {});
    for (std::size_t i = 0; i < Mixer::kMaxVoices; ++i) REQUIRE(full.play(tone, {}) != kNoVoice);
    CHECK(full.activeVoices() == Mixer::kMaxVoices);
    CHECK(full.isPlaying(song));
    CHECK_FALSE(full.isPlaying(first));
}

TEST_CASE("Mixer: sounds in the world") {
    Listener l;
    l.position = {100.0f, 100.0f};
    l.halfView = {160.0f, 90.0f};
    CHECK(placeSound(l, {100.0f, 100.0f}, 0.0f).gain == 1.0f);
    CHECK(placeSound(l, {250.0f, 150.0f}, 0.0f).gain == 1.0f); // on screen
    CHECK(placeSound(l, {250.0f, 150.0f}, 0.0f).pan > 0.3f);   // on the right
    CHECK(placeSound(l, {-40.0f, 100.0f}, 0.0f).pan < -0.3f);
    CHECK(placeSound(l, {340.0f, 100.0f}, 0.0f).gain == doctest::Approx(0.5f)); // half way out of the margin
    CHECK(placeSound(l, {500.0f, 100.0f}, 0.0f).gain == 0.0f);
    CHECK(placeSound(l, {150.0f, 100.0f}, 100.0f).gain == doctest::Approx(0.5f)); // a range of its own
    CHECK(placeSound(l, {300.0f, 100.0f}, 100.0f).gain == 0.0f);

    Mixer m(48000);
    m.setListener(l);
    PlayOptions far;
    far.positional = true;
    far.position = {800.0f, 100.0f};
    far.loop = true;
    const VoiceId v = m.play(loadPcm(constant(48000, 0.5, 16384)), far);
    auto out = renderSeconds(m, 0.05);
    CHECK(rms(out, 0) == 0.0);
    m.setPosition(v, {110.0f, 100.0f}); // it comes closer
    out = renderSeconds(m, 0.05);
    CHECK(rms(out, 0, 1000) > 0.4);
}

TEST_CASE("Mixer: long sounds play while they are decoded") {
    // 20 s of QOA: longer than kDecodeWholeSeconds, so it streams.
    const PcmSound pcm = sine(24000, 1, 20.0, 220.0);
    auto qoa = encodeQoa(pcm);
    REQUIRE(qoa.ok());
    auto sound = Sound::load(qoa.value(), "music");
    REQUIRE(sound.ok());
    CHECK_FALSE((*sound)->decoded());
    Mixer m(48000);
    PlayOptions options;
    options.loop = true;
    options.bus = Bus::Music;
    options.pitch = 4.0f; // 5 s per round
    const VoiceId v = m.play(sound.value(), options);
    for (int second = 0; second < 12; ++second) {
        auto out = renderSeconds(m, 1.0);
        CHECK(rms(out, 0) == doctest::Approx(0.5 / std::numbers::sqrt2).epsilon(0.05));
    }
    CHECK(m.isPlaying(v)); // looped twice
    options.loop = false;
    Mixer once(48000);
    (void)once.play(sound.value(), options);
    auto out = renderSeconds(once, 6.0);
    CHECK(rms(out, 0, 0, 48000) > 0.3);
    CHECK(rms(out, 0, 5 * 48000 + 1000) == 0.0);
    CHECK(once.activeVoices() == 0);
}

TEST_CASE("Game audio: one-shot sounds, AudioSource components and music") {
    MemoryFileSystem files;
    files.addFile("sounds/beep.wav", encodeWav(constant(48000, 0.2, 8000)));
    files.addFile("sounds/hum.wav", encodeWav(constant(48000, 0.5, 4000)));
    files.addFile("sounds/song.wav", encodeWav(sine(48000, 2, 1.0, 220.0)));
    files.addFile("sounds/other.wav", encodeWav(sine(48000, 2, 1.0, 330.0)));
    files.addFile("sounds/broken.wav", std::string_view("RIFF....WAVEjunk"));
    assets::AssetRegistry registry;
    const assets::AssetId beep{Guid(1, 1)}, hum{Guid(1, 2)}, song{Guid(1, 3)}, other{Guid(1, 4)}, broken{Guid(1, 5)};
    REQUIRE(registry.add({beep, assets::AssetType::Sound, "Beep", "sounds/beep.wav"}));
    REQUIRE(registry.add({hum, assets::AssetType::Sound, "Hum", "sounds/hum.wav"}));
    REQUIRE(registry.add({song, assets::AssetType::Sound, "Song", "sounds/song.wav"}));
    REQUIRE(registry.add({other, assets::AssetType::Sound, "Other", "sounds/other.wav"}));
    REQUIRE(registry.add({broken, assets::AssetType::Sound, "Broken", "sounds/broken.wav"}));
    SoundLibrary library(files, registry);
    CHECK(library.get(broken) == nullptr); // reported once, then silent
    CHECK(library.get(broken) == nullptr);
    library.preload();
    CHECK(library.loaded() == 4);

    Mixer mixer(48000);
    auto audio = std::make_unique<GameAudio>(mixer, library);
    scene::Scene scene;
    const scene::Entity cam = scene.createEntity("Camera");
    scene.set(cam, scene::Transform{});
    scene::Camera camera;
    camera.position = {160, 90};
    camera.zoom = 2.0f;
    scene.set(cam, camera);
    const Listener listener = listenerForScene(scene, {320, 180});
    CHECK(listener.position == Vec2{160, 90});
    CHECK(listener.halfView == Vec2{80, 45});

    // One-shot sounds: everyone's, someone else's (skipped), one from an entity.
    const scene::Entity bird = scene.createEntity("Bird");
    scene.set(bird, scene::Transform{{100, 90}});
    const Guid birdId = scene.get<scene::EntityInfo>(bird).id;
    std::vector<runtime::SoundEvent> sounds(3);
    sounds[0].sound = beep;
    sounds[1].sound = beep;
    sounds[1].player = 2;
    sounds[2].sound = beep;
    sounds[2].entity = birdId;
    sounds[2].position = Vec2{0, 0}; // where it was on the server; the view's position wins
    audio->play(sounds, scene, 1);
    CHECK(mixer.activeVoices() == 2);
    audio->update(scene, {}, listener);
    auto out = renderSeconds(mixer, 0.1);
    // The bird is left of centre (in view): the left channel is louder.
    CHECK(rms(out, 0, 1000) > rms(out, 1, 1000));
    renderSeconds(mixer, 0.2);
    audio->update(scene, {}, listener);
    CHECK(mixer.activeVoices() == 0);

    // A looping AudioSource plays while `playing`; a one-off plays once.
    const scene::Entity fan = scene.createEntity("Fan");
    scene.set(fan, scene::Transform{{160, 90}});
    scene.set(fan, scene::AudioSource{hum});
    const scene::Entity bell = scene.createEntity("Bell");
    scene.set(bell, scene::Transform{{160, 90}});
    scene::AudioSource once{beep};
    once.loop = false;
    scene.set(bell, once);
    audio->update(scene, {}, listener);
    CHECK(audio->sourcesPlaying() == 2);
    renderSeconds(mixer, 0.6);
    audio->update(scene, {}, listener);
    CHECK(audio->sourcesPlaying() == 1); // the bell rang once; the fan loops
    renderSeconds(mixer, 0.6);
    audio->update(scene, {}, listener);
    CHECK(audio->sourcesPlaying() == 1);
    scene.get<scene::AudioSource>(bell).playing = false;
    audio->update(scene, {}, listener);
    scene.get<scene::AudioSource>(bell).playing = true; // rings again
    audio->update(scene, {}, listener);
    CHECK(audio->sourcesPlaying() == 2);
    scene.get<scene::AudioSource>(fan).playing = false;
    scene.destroyEntity(bell);
    audio->update(scene, {}, listener);
    renderSeconds(mixer, 0.1);
    CHECK(audio->sourcesPlaying() == 0);
    CHECK(mixer.activeVoices() == 0);

    // Music: starts, changes volume without restarting, crossfades to another song.
    runtime::MusicState music{song, 0.5f, 0.0f};
    audio->update(scene, music, listener);
    CHECK(mixer.activeVoices() == 1);
    music.volume = 0.25f;
    audio->update(scene, music, listener);
    CHECK(mixer.activeVoices() == 1);
    music = {other, 1.0f, 0.5f};
    audio->update(scene, music, listener);
    CHECK(mixer.activeVoices() == 2); // both while they cross
    renderSeconds(mixer, 0.6);
    CHECK(mixer.activeVoices() == 1);
    CHECK(audio->music().sound == other);
    audio->update(scene, runtime::MusicState{{}, 1.0f, 0.2f}, listener); // silence, after a short fade
    renderSeconds(mixer, 0.3);
    CHECK(mixer.activeVoices() == 0);

    // Going away fades everything it started.
    audio->update(scene, music, listener);
    scene.get<scene::AudioSource>(fan).playing = true;
    audio->update(scene, music, listener);
    CHECK(mixer.activeVoices() == 2);
    audio.reset();
    renderSeconds(mixer, 0.2);
    CHECK(mixer.activeVoices() == 0);
}
