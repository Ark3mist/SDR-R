// SDL2 audio back end (macOS simulator builds; selectable on Linux with
// -DOPENSDRX_AUDIO=sdl). SDL calls render() from its own audio thread.
#include <cstdio>

#include <SDL.h>

#include "audio/audio_sink.h"

namespace osx {

namespace {
void sdlCallback(void* user, Uint8* stream, int len) {
    static_cast<AudioSink*>(user)->render(reinterpret_cast<int16_t*>(stream), size_t(len) / 4);
}
}  // namespace

const char* AudioSink::backend() const { return "SDL2"; }

bool AudioSink::start(const std::string& device) {
    if (running_.exchange(true)) return deviceOpen_.load();
    buffering_ = true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        status_ = std::string("no audio device (SDL: ") + SDL_GetError() + ")";
    } else {
        SDL_AudioSpec want{}, have{};
        want.freq = int(kRate);
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = Uint16(kPeriod);
        want.callback = sdlCallback;
        want.userdata = this;
        // ALSA-style names mean nothing to SDL: use its default device for them.
        const char* name = (device.empty() || device.find(':') != std::string::npos || device == "default") ? nullptr : device.c_str();
        const SDL_AudioDeviceID id = SDL_OpenAudioDevice(name, 0, &want, &have, 0);
        if (id == 0) {
            status_ = std::string("no audio device (SDL: ") + SDL_GetError() + ")";
        } else {
            handle_ = reinterpret_cast<void*>(uintptr_t(id));
            deviceOpen_.store(true);
            char d[96];
            std::snprintf(d, sizeof(d), "SDL %s (%d Hz, %u frames)", SDL_GetCurrentAudioDriver(), have.freq, unsigned(have.samples));
            status_ = d;
            SDL_PauseAudioDevice(id, 0);
            return true;
        }
    }
    std::fprintf(stderr, "[audio] %s\n", status_.c_str());
    thread_ = std::thread(&AudioSink::runTimer, this);
    return false;
}

void AudioSink::stop() {
    if (!running_.exchange(false)) return;
    if (handle_) {
        SDL_CloseAudioDevice(SDL_AudioDeviceID(reinterpret_cast<uintptr_t>(handle_)));
        handle_ = nullptr;
    }
    if (thread_.joinable()) thread_.join();
    deviceOpen_.store(false);
    status_ = "stopped";
}

void AudioSink::runAlsa() {}

std::string AudioSink::clampMixer(const std::string&, float) {
    return "mixer: not available with the SDL audio back end";
}

}  // namespace osx
