// Thread 3: audio sink. Pulls interleaved stereo float32 from the lock-free
// audio ring, applies master volume and the -6 dBFS output limiter, and
// plays 48 kHz stereo S16. Back ends:
//   ALSA (Linux / Cardputer Zero): own thread, blocking snd_pcm_writei,
//        1024-frame periods, snd_pcm_recover() on XRUN; "plughw:0,0" with
//        fallback to "default".
//   SDL2 (macOS simulator builds, or -DOPENSDRX_AUDIO=sdl): SDL's callback.
// With no usable device the sink keeps draining the ring on a timer, so the
// rest of the pipeline behaves identically, and reports why.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "audio/limiter.h"
#include "core/ring_buffer.h"

namespace osx {

class AudioSink {
public:
    static constexpr unsigned kRate = 48000;
    static constexpr size_t kPeriod = 1024;  // frames

    explicit AudioSink(RingBuffer<float>& ring);
    ~AudioSink();

    bool start(const std::string& device = "plughw:0,0");  // false = running without a device
    void stop();

    // Master volume 0..1 (default 0.75), logarithmic: 100 % = 0 dB, each
    // step below is a constant dB change, 0 % = silence. 100 % lets the
    // loudest audio reach the -6 dBFS limiter ceiling.
    std::atomic<float> volume{0.75f};
    std::atomic<bool> mute{false};
    static float volumeToGain(float v);
    static constexpr float kVolumeSpanDb = 40.0f;

    bool deviceOpen() const { return deviceOpen_.load(); }
    const std::string& status() const { return status_; }
    const char* backend() const;
    uint64_t underruns() const { return underruns_.load(); }
    float outputPeak() const { return peak_.load(); }

    // Set the card's main playback volume control to exactly maxDb (ES8389
    // protection). Returns a one-line report. ALSA only.
    static std::string clampMixer(const std::string& card = "hw:0", float maxDb = -6.0f);

    // Pull `frames` stereo frames from the ring into S16 (volume + limiter
    // applied). Real-time safe: no allocation, no locks. Back ends call it.
    void render(int16_t* out, size_t frames);

private:
    void runAlsa();
    void runTimer();

    RingBuffer<float>& ring_;
    void* handle_ = nullptr;     // snd_pcm_t* or SDL_AudioDeviceID
    std::thread thread_;
    std::atomic<bool> running_{false}, deviceOpen_{false};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<float> peak_{0.0f};
    std::string status_ = "stopped";

    // render() state (audio thread only)
    OutputLimiter limiter_;
    float gain_ = 0.0f;
    bool buffering_ = true;
    float scratch_[kPeriod * 2 * 4] = {};
};

}  // namespace osx
