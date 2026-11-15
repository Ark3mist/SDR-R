// One VFO receive chain (runs on the DSP thread):
//
//   IQ @ fs_in -> [noise blanker] -> NCO mix -> stage 1: polyphase decimating
//   FIR (integer D, to the IF rate) -> stage 2: channel filter (Hamming /
//   Blackman, complex-shifted for SSB) -> demodulator [+ RDS / CTCSS]
//   -> arbitrary-ratio resampler to 48 kHz -> de-emphasis -> [auto-notch]
//   -> [spectral NR] -> AGC -> squelch gate -> stereo float audio ring.
//
// All buffers, filters and FFT plans are created when the mode, bandwidth or
// input rate changes (rebuild). process() performs no heap allocation.
#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include "core/ring_buffer.h"
#include "core/types.h"
#include "dsp/auto_notch.h"
#include "dsp/fir.h"
#include "dsp/noise_blanker.h"
#include "dsp/rds.h"
#include "dsp/resampler.h"
#include "dsp/spectral_nr.h"

namespace osx {

enum class Demod { WFM = 0, NFM, AM, SAM, USB, LSB, DSB, CW, RAW, Count };
const char* demodName(Demod m);
bool demodFromName(const std::string& name, Demod& out);

struct DemodLimits {
    double minBw, maxBw, defaultBw, bwStep, defaultSnap;
};
const DemodLimits& demodLimits(Demod m);

struct RxConfig {
    Demod mode = Demod::WFM;
    double bandwidthHz = 200e3;  // SSB: upper audio edge; others: total width
    double lowCutHz = 200.0;     // SSB low audio edge, 100..300 Hz
    bool stereo = true;          // WFM: decode L-R when the 19 kHz pilot locks
    bool rds = true;             // WFM: RDS decoder
    int deemphasis = 1;          // 0 = none, 1 = 50 us, 2 = 75 us (FM modes)
    bool squelch = false;
    float squelchDb = -50.0f;    // channel power threshold, dBFS
    int agc = 1;                 // 0 fast, 1 medium, 2 slow, 3 off
    float afGainDb = 20.0f;      // fixed audio gain when AGC is off
    float cwPitchHz = 700.0f;    // 300..1000
    bool ctcss = true;           // NFM sub-tone detector
    // Interference removal
    bool nb = false;
    float nbThreshold = 3.0f;    // x RMS, 1.5..8.0
    float nbHoldoffUs = 5.0f;    // 1..200 us
    bool anf = false;            // LMS auto-notch (voice modes)
    bool nr = false;             // spectral noise reduction (voice modes)
    float nrStrength = 0.5f;     // 0..1

    bool operator==(const RxConfig&) const = default;
};

// Passband edges relative to the VFO frequency.
void passband(const RxConfig& c, double& loHz, double& hiHz);

// The N-key presets: Off -> NB -> NB + auto-notch -> full NR.
enum class NrPreset { Off = 0, NB, NBNotch, Full, Custom };
NrPreset nrPreset(const RxConfig& c);
void applyNrPreset(RxConfig& c, NrPreset p);
const char* nrPresetName(NrPreset p);

class Receiver {
public:
    static constexpr double kAudioRate = 48000.0;
    static constexpr size_t kAudioTargetFrames = 4800;  // 100 ms: covers 64 ms USB bursts
    static constexpr size_t kMaxBlock = 32768;          // input samples per internal chunk

    explicit Receiver(RingBuffer<float>& audioOut);

    void setInputRate(double fs);            // only while the DSP thread is stopped
    void setConfig(const RxConfig& c);       // any thread
    RxConfig config() const;
    void setOffset(double hz) { offsetHz_.store(hz, std::memory_order_relaxed); }
    void setClockTracking(bool on) { clockTracking_ = on; }  // off for offline tests

    void process(const cf32* in, size_t n);  // DSP thread

    // Telemetry (any thread).
    std::atomic<float> levelDb{-120.0f};
    std::atomic<bool> squelchOpen{true};
    std::atomic<bool> stereoLocked{false};
    std::atomic<float> ctcssHz{0.0f};        // 0 = none detected
    std::atomic<float> nbBlankedPct{0.0f};
    std::atomic<uint64_t> audioDrops{0};
    RdsInfo rdsInfo() const { return rds_.info(); }
    double ifRate() const { return ifRate_; }

private:
    void rebuild();
    void applyLive();
    void processChunk(const cf32* in, size_t n);
    void demodulate(size_t n);
    void postProcess();
    void runCtcss(const float* audio, size_t n);

    RingBuffer<float>& audio_;
    mutable std::mutex cfgMutex_;
    RxConfig pending_;
    std::atomic<bool> dirty_{true};
    std::atomic<double> offsetHz_{0.0};
    bool clockTracking_ = true;
    bool built_ = false;

    // --- DSP-thread state ---
    RxConfig cfg_;
    double inRate_ = 1.024e6, ifRate_ = 256e3, lastOffset_ = 0.0;
    size_t decim_ = 4, decimPhase_ = 0;
    uint32_t ncoPhase_ = 0;
    FirCR stage1_;
    FirCC channel_;
    std::vector<cf32> nbBuf_, if_;
    std::vector<float> l_, r_, mpx_, la_, ra_, out_;
    ArbResampler resL_, resR_;
    NoiseBlanker nb_;
    AutoNotch anf_;
    SpectralNR nr_;
    RdsDecoder rds_;

    cf32 fmPrev_{1.0f, 0.0f};
    float fmScale_ = 1.0f;
    float pilotPhase_ = 0.0f, pilotFreq_ = 0.0f, pilotNominal_ = 0.0f, pilotAlpha_ = 0.0f, pilotBeta_ = 0.0f;
    cf32 pilotLp_{0.0f, 0.0f};
    float pilotLpCoef_ = 0.0f, pilotMag_ = 0.0f;
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
    std::vector<float> ctcssBuf_;
    float ctcssAcc_ = 0.0f;
    int ctcssAccN_ = 0, ctcssLast_ = -1, ctcssMiss_ = 0;
};

const std::vector<float>& ctcssTones();

}  // namespace osx
