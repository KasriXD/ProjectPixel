#include "px/platform_sdl/SdlAudio.hpp"

#include "px/core/Log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <format>

namespace px::platform {

std::unique_ptr<SdlAudio> SdlAudio::open(audio::Mixer& mixer) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        log::info("No sound: {}", SDL_GetError());
        return nullptr;
    }
    auto audio = std::unique_ptr<SdlAudio>(new SdlAudio(mixer));
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, mixer.sampleRate()};
    audio->m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &SdlAudio::feed, audio.get());
    if (audio->m_stream == nullptr) {
        log::info("No sound: {}", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return nullptr;
    }
    audio->m_buffer.resize(4096 * 2);
    SDL_AudioSpec device{};
    int frames = 0;
    const char* driver = SDL_GetCurrentAudioDriver();
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(audio->m_stream), &device, &frames)) {
        audio->m_description = std::format("{} at {} Hz, {} frames", driver != nullptr ? driver : "audio", device.freq, frames);
    } else {
        audio->m_description = driver != nullptr ? driver : "audio";
    }
    SDL_ResumeAudioStreamDevice(audio->m_stream);
    log::debug("Sound: {}", audio->m_description);
    return audio;
}

SdlAudio::~SdlAudio() {
    if (m_stream != nullptr) {
        SDL_DestroyAudioStream(m_stream); // stops the callback before returning
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

void SdlAudio::pause(bool paused) {
    if (m_stream == nullptr) return;
    if (paused) {
        SDL_PauseAudioStreamDevice(m_stream);
    } else {
        SDL_ResumeAudioStreamDevice(m_stream);
    }
}

void SdlAudio::feed(void* self, SDL_AudioStream* stream, int additional, int /*total*/) {
    auto& audio = *static_cast<SdlAudio*>(self);
    auto frames = static_cast<std::size_t>(std::max(additional, 0)) / (2 * sizeof(float));
    while (frames > 0) {
        const std::size_t n = std::min(frames, audio.m_buffer.size() / 2);
        audio.m_mixer.render(audio.m_buffer.data(), n);
        SDL_PutAudioStreamData(stream, audio.m_buffer.data(), static_cast<int>(n * 2 * sizeof(float)));
        frames -= n;
    }
}

} // namespace px::platform
