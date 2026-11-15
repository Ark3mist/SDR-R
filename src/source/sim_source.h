// Synthetic RF environment: a repeating band plan of modulated emitters plus
// Gaussian noise, a DC offset and 8-bit quantisation, paced in real time.
// It exercises the exact data path a real RTL-SDR uses (uint8 I/Q -> ring).
#pragma once

#include <complex>
#include <thread>
#include <unordered_map>

#include "source/source.h"

namespace osx {

class SimSource final : public Source {
public:
    enum class Mod { WFM, NFM, AM, CW, Carrier, Sweep };

    struct Emitter {
        const char* label;
        double offsetHz;   // offset inside the repeating band plan
        float levelDb;     // carrier level, dBFS
        Mod mod;
        float devHz;       // FM deviation / sweep half-width
        float toneHz;      // modulating tone / sweep or keying rate
    };

    // The band plan repeats every kPlanPeriodHz, anchored at 0 Hz RF.
    static constexpr double kPlanPeriodHz = 2.0e6;
    static const std::vector<Emitter>& bandPlan();

    SimSource();
    ~SimSource() override;

    const char* name() const override { return "Simulated (Signal + Noise)"; }
    bool start(RingBuffer<uint8_t>& out) override;
    void stop() override;
    bool running() const override { return running_.load(); }

    void setCenterFrequency(double hz) override { centerHz_.store(hz); }
    double centerFrequency() const override { return centerHz_.load(); }
    void setSampleRate(double sps) override { if (!running()) sampleRate_ = sps; }
    double sampleRate() const override { return sampleRate_; }
    std::vector<double> sampleRates() const override;

    // Live-adjustable parameters (UI thread writes, I/O thread reads).
    std::atomic<float> noiseDb{-48.0f};     // total noise power, dBFS
    std::atomic<float> gainDb{0.0f};        // applied to emitters only
    std::atomic<float> dcOffset{0.012f};    // simulated ADC DC offset (full scale = 1)
    std::atomic<bool> emittersEnabled{true};

    // Render `n` complex samples (2n bytes) for the current settings.
    // Public so the unit tests can drive the generator without a thread.
    void generate(uint8_t* dst, size_t n);

private:
    void run(RingBuffer<uint8_t>* out);

    struct EmitterState {
        uint32_t modPhase = 0;                          // audio / keying phase
        uint32_t pilotPhase = 0;                        // 19 kHz stereo pilot (WFM)
        uint32_t fmPhase = 0;                           // integrated FM phase
        float keyAmp = 0.0f;                            // smoothed CW keying envelope
        std::unordered_map<int64_t, uint32_t> carrier;  // per-replica NCO phase
    };

    double sampleRate_ = 2.4e6;
    std::atomic<double> centerHz_{100.0e6};
    std::atomic<bool> running_{false};
    std::thread thread_;

    std::vector<EmitterState> state_;
    std::vector<std::complex<float>> acc_, env_;
    uint64_t rng_ = 0x9E3779B97F4A7C15ull;
};

}  // namespace osx
