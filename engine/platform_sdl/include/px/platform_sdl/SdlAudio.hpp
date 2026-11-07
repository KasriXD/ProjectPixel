#pragma once

#include "px/audio/Mixer.hpp"

#include <memory>
#include <string>
#include <vector>

struct SDL_AudioStream;

namespace px::platform {

// Plays a mixer through the default audio device. SDL's audio thread asks the
// mixer for samples as the device needs them.
class SdlAudio {
public:
    // nullptr when there is no audio device (a server, a CI machine, a
    // browser that blocks sound): the game then plays silently. Logged.
    [[nodiscard]] static std::unique_ptr<SdlAudio> open(audio::Mixer& mixer);
    ~SdlAudio();
    SdlAudio(const SdlAudio&) = delete;
    SdlAudio& operator=(const SdlAudio&) = delete;

    // Silences the device while the app is in the background (phones).
    void pause(bool paused);
    [[nodiscard]] const std::string& description() const { return m_description; }

private:
    explicit SdlAudio(audio::Mixer& mixer) : m_mixer(mixer) {}
    static void feed(void* self, SDL_AudioStream* stream, int additional, int total);

    audio::Mixer& m_mixer;
    SDL_AudioStream* m_stream = nullptr;
    std::vector<float> m_buffer; // used on the audio thread only
    std::string m_description;
};

} // namespace px::platform
