// Thread 2: the DSP engine.
//
// Pulls raw uint8 I/Q from the hardware ring, converts to complex float,
// applies IQ inversion and DC-spike removal, and feeds the spectrum tap.
// (The per-VFO DDC / demodulator chain hangs off this thread from Phase 2.)
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "core/ring_buffer.h"
#include "core/types.h"

namespace osx {

// uint8 offset-binary I/Q -> complex float in [-1, 1].
void convertU8ToCf32(const uint8_t* src, cf32* dst, size_t nSamples, bool swapIq);

// Single-pole DC tracker: y[n] = x[n] - dc[n], dc += alpha * (x - dc).
struct DcBlocker {
    cf32 dc{0.0f, 0.0f};
    float alpha = 1e-5f;

    void setTimeConstant(double seconds, double sampleRate) {
        alpha = float(1.0 / (seconds * sampleRate));
    }
    void reset() { dc = {0.0f, 0.0f}; }
    void process(cf32* x, size_t n) {
        float dr = dc.real(), di = dc.imag();
        const float a = alpha;
        for (size_t i = 0; i < n; ++i) {
            const float r = x[i].real(), q = x[i].imag();
            dr += a * (r - dr);
            di += a * (q - di);
            x[i] = {r - dr, q - di};
        }
        dc = {dr, di};
    }
};

class DspEngine {
public:
    DspEngine(RingBuffer<uint8_t>& in, RingBuffer<cf32>& spectrumTap);
    ~DspEngine();

    void start(double sampleRate);
    void stop();
    bool running() const { return running_.load(); }

    std::atomic<bool> dcRemoval{true};
    std::atomic<bool> invertIq{false};

    uint64_t samplesProcessed() const { return samples_.load(std::memory_order_relaxed); }
    // Peak |I| or |Q| over the most recent blocks, 0..1 (ADC clip indicator).
    float peakLevel() const { return peak_.load(std::memory_order_relaxed); }

private:
    void run();

    RingBuffer<uint8_t>& in_;
    RingBuffer<cf32>& tap_;
    double sampleRate_ = 2.4e6;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::atomic<uint64_t> samples_{0};
    std::atomic<float> peak_{0.0f};
};

}  // namespace osx
