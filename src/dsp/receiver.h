// One VFO receive chain (runs on the DSP thread):
//
//   IQ @ fs_in -> NCO mix -> decimating FIR (integer D) -> channel filter
//   -> demodulator -> arbitrary resampler to 48 kHz -> de-emphasis / AGC /
//   squelch -> interleaved stereo float into the audio ring.
#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include "core/ring_buffer.h"
#include "core/types.h"
#include "dsp/fir.h"
#include "dsp/resampler.h"

namespace osx {

enum class Demod { WFM = 0, NFM, AM, SAM, USB, LSB, CW, RAW, Count };
const char* demodName(Demod m);

struct DemodLimits {
    double minBw, maxBw, defaultBw, bwStep, defaultSnap;
};
const DemodLimits& demodLimits(Demod m);

struct RxConfig {
    Demod mode = Demod::WFM;
    double bandwidthHz = 200e3;
    bool stereo = true;          // WFM: decode the 38 kHz L-R subcarrier when the pilot locks
    int deemphasis = 1;          // 0 = none, 1 = 50 us, 2 = 75 us (FM modes)
    bool squelch = false;
    float squelchDb = -50.0f;    // channel power threshold, dBFS
    int agc = 1;                 // 0 fast, 1 medium, 2 slow (AM / SAM / SSB / CW)
    float cwPitchHz = 700.0f;
    bool ctcss = true;           // NFM sub-tone detector

    bool operator==(const RxConfig&) const = default;
};

class Receiver {
public:
    static constexpr double kAudioRate = 48000.0;
    static constexpr size_t kAudioTargetFrames = 3840;  // 80 ms of buffered audio

    explicit Receiver(RingBuffer<float>& audioOut);

    void setInputRate(double fs);            // only while the DSP thread is stopped
    void setConfig(const RxConfig& c);       // any thread
    RxConfig config() const;
    void setOffset(double hz) { offsetHz_.store(hz, std::memory_order_relaxed); }
    // When false the resampler ratio is not steered by ring fill (offline tests).
    void setClockTracking(bool on) { clockTracking_ = on; }

    void process(const cf32* in, size_t n);  // DSP thread

    // Telemetry (any thread).
    std::atomic<float> levelDb{-120.0f};     // channel power after the channel filter
    std::atomic<bool> squelchOpen{true};
    std::atomic<bool> stereoLocked{false};
    std::atomic<float> ctcssHz{0.0f};        // 0 = none detected
    std::atomic<uint64_t> audioDrops{0};     // frames dropped because the sink stalled
    double ifRate() const { return ifRate_; }

private:
    void rebuild();
    void demodulate(size_t n);
    void postProcess();
    void runCtcss(const float* audio, size_t n);

    RingBuffer<float>& audio_;
    mutable std::mutex cfgMutex_;
    RxConfig pending_;
    std::atomic<bool> dirty_{true};
    std::atomic<double> offsetHz_{0.0};
    bool clockTracking_ = true;

    // --- DSP-thread state ---
    RxConfig cfg_;
    double inRate_ = 1.024e6, ifRate_ = 256e3;
    size_t decim_ = 4, decimPhase_ = 0;
    uint32_t ncoPhase_ = 0;
    Fir<cf32, float> stage1_;
    Fir<cf32, cf32> channel_;
    std::vector<cf32> if_;
    std::vector<float> l_, r_, la_, ra_, out_;
    ArbResampler resL_, resR_;

    cf32 fmPrev_{1.0f, 0.0f};
    float fmScale_ = 1.0f;
    // 19 kHz pilot PLL
    float pilotPhase_ = 0.0f, pilotFreq_ = 0.0f, pilotNominal_ = 0.0f, pilotAlpha_ = 0.0f, pilotBeta_ = 0.0f;
    cf32 pilotLp_{0.0f, 0.0f};
    float pilotLpCoef_ = 0.0f, pilotMag_ = 0.0f;
    // SAM carrier PLL
    float samPhase_ = 0.0f, samFreq_ = 0.0f, samAlpha_ = 0.0f, samBeta_ = 0.0f, samMaxFreq_ = 0.0f;
    uint32_t bfoPhase_ = 0, bfoInc_ = 0;

    float powerAvg_ = 0.0f;
    bool gateOpen_ = true;
    float gateGain_ = 1.0f;
    float deL_ = 0.0f, deR_ = 0.0f, deCoef_ = 1.0f;
    float dcAvg_ = 0.0f;
    float agcEnv_ = 1e-3f, agcDecay_ = 0.0f, agcAttack_ = 0.0f;
    int agcHang_ = 0, agcHangSamples_ = 0;
    Biquad nfmHp_, ctcssLp_;
    // CTCSS Goertzel bank (runs at 6 kHz)
    std::vector<float> ctcssBuf_;
    float ctcssAcc_ = 0.0f;
    int ctcssAccN_ = 0, ctcssLast_ = -1, ctcssMiss_ = 0;
};

// Standard EIA CTCSS tone table.
const std::vector<float>& ctcssTones();

}  // namespace osx
