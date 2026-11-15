// Thread 3: audio sink. Pulls interleaved stereo float32 from the lock-free
// audio ring and writes 48 kHz S16 to ALSA. If no playback device can be
// opened it keeps draining the ring on a wall-clock timer so the rest of the
// pipeline behaves identically (and reports the failure).
#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "core/ring_buffer.h"

namespace osx {

class AlsaSink {
public:
    explicit AlsaSink(RingBuffer<float>& ring);
    ~AlsaSink();

    bool start(const std::string& device = "plughw:0,0");  // false = running without a device
    void stop();

    std::atomic<float> volume{0.6f};   // 0..1
    std::atomic<bool> mute{false};

    bool deviceOpen() const { return deviceOpen_.load(); }
    const std::string& status() const { return status_; }
    uint64_t underruns() const { return underruns_.load(); }
    float outputPeak() const { return peak_.load(); }

    // Clamp the card's main playback volume control to at most maxDb.
    // Returns a one-line report of what was done.
    static std::string clampMixer(const std::string& card = "hw:0", float maxDb = -6.0f);

private:
    void run();

    RingBuffer<float>& ring_;
    void* pcm_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false}, deviceOpen_{false};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<float> peak_{0.0f};
    std::string status_ = "stopped";
};

}  // namespace osx
