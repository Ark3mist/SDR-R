// Thread 2: the DSP engine.
//
// Pulls I/Q from the source ring (raw uint8 from RTL-SDR / rtl_tcp / the
// simulator, or complex float from the file player), converts to complex
// float, applies IQ inversion, DC-spike removal and I/Q balance correction,
// feeds the display-FFT tap, and runs the VFO receive chain.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "core/ring_buffer.h"
#include "core/types.h"
#include "dsp/conditioning.h"

namespace osx { class Receiver; }

namespace osx {

// uint8 offset-binary I/Q -> complex float in [-1, 1].
void convertU8ToCf32(const uint8_t* src, cf32* dst, size_t nSamples, bool swapIq);

class DspEngine {
public:
    // `rx` (optional) is the VFO receive chain driven from this thread.
    enum class Input { U8, CF32 };
    DspEngine(RingBuffer<uint8_t>& in, RingBuffer<cf32>& spectrumTap, Receiver* rx = nullptr,
              RingBuffer<cf32>* inF32 = nullptr);
    ~DspEngine();

    void start(double sampleRate, Input input = Input::U8);
    void stop();
    bool running() const { return running_.load(); }

    std::atomic<bool> dcRemoval{true};
    std::atomic<bool> invertIq{false};
    std::atomic<bool> iqCorrection{true};

    // Estimated front-end I/Q error (valid while iqCorrection is on).
    std::atomic<float> iqPhaseErrDeg{0.0f}, iqGainErrDb{0.0f};

    uint64_t samplesProcessed() const { return samples_.load(std::memory_order_relaxed); }
    // Peak |I| or |Q| over the most recent blocks, 0..1 (ADC clip indicator).
    float peakLevel() const { return peak_.load(std::memory_order_relaxed); }

private:
    void run();

    RingBuffer<uint8_t>& in_;
    RingBuffer<cf32>* inF32_ = nullptr;
    Input input_ = Input::U8;
    RingBuffer<cf32>& tap_;
    Receiver* rx_ = nullptr;
    double sampleRate_ = 2.4e6;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::atomic<uint64_t> samples_{0};
    std::atomic<float> peak_{0.0f};
};

}  // namespace osx
