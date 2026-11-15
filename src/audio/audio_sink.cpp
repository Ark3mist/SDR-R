// Back-end independent part of the audio sink.
#include "audio/audio_sink.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "dsp/receiver.h"

namespace osx {

AudioSink::AudioSink(RingBuffer<float>& ring) : ring_(ring), limiter_(double(kRate)) {}
AudioSink::~AudioSink() { stop(); }

float AudioSink::volumeToGain(float v) {
    if (v <= 0.0f) return 0.0f;
    return std::pow(10.0f, -kVolumeSpanDb * (1.0f - std::min(v, 1.0f)) / 20.0f);
}

void AudioSink::render(int16_t* out, size_t frames) {
    size_t done = 0;
    while (done < frames) {
        const size_t chunk = std::min(frames - done, sizeof(scratch_) / sizeof(float) / 2);
        float* buf = scratch_;
        const size_t availFrames = ring_.readable() / 2;
        size_t got = 0;
        if (buffering_ && availFrames >= Receiver::kAudioTargetFrames) buffering_ = false;
        if (!buffering_) {
            got = ring_.read(buf, std::min(availFrames, chunk) * 2) / 2;
            if (got < chunk) {
                if (got > 0 || availFrames > 0) underruns_.fetch_add(1, std::memory_order_relaxed);
                buffering_ = true;  // refill to the target before resuming: no stutter
            }
        }
        std::fill(buf + got * 2, buf + chunk * 2, 0.0f);

        // Master volume, ramped per sample (~10 ms) so steps never click.
        const float target = mute.load(std::memory_order_relaxed) ? 0.0f : volumeToGain(volume.load(std::memory_order_relaxed));
        float g = gain_;
        for (size_t i = 0; i < chunk; ++i) {
            g += 0.002f * (target - g);
            buf[2 * i] *= g;
            buf[2 * i + 1] *= g;
        }
        gain_ = g;
        limiter_.process(buf, chunk);
        peak_.store(limiter_.peak, std::memory_order_relaxed);
        for (size_t i = 0; i < chunk * 2; ++i) out[done * 2 + i] = int16_t(std::lrintf(buf[i] * 32767.0f));
        done += chunk;
    }
}

void AudioSink::runTimer() {
    int16_t pcm[kPeriod * 2];
    auto next = std::chrono::steady_clock::now();
    const auto period = std::chrono::microseconds(int64_t(1e6 * double(kPeriod) / double(kRate)));
    while (running_.load(std::memory_order_relaxed)) {
        render(pcm, kPeriod);
        next += period;
        const auto now = std::chrono::steady_clock::now();
        if (now - next > std::chrono::milliseconds(200)) next = now;
        std::this_thread::sleep_until(next);
    }
}

}  // namespace osx
