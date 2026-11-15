#include "dsp/receiver.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

#include "core/exp_table.h"

namespace osx {

namespace {
constexpr float kTwoPi = float(2.0 * std::numbers::pi);
constexpr float kTwo32f = 4294967296.0f;
constexpr double kCtcssRate = 6000.0;   // audio / 8
constexpr size_t kCtcssWindow = 3000;   // 0.5 s -> 2 Hz resolution

inline uint32_t turnsToPhase(float turns) { return uint32_t(int64_t(turns * kTwo32f)); }
}  // namespace

const char* demodName(Demod m) {
    static const char* names[] = {"WFM", "NFM", "AM", "SAM", "USB", "LSB", "DSB", "CW", "RAW"};
    return int(m) < int(Demod::Count) ? names[int(m)] : "?";
}

bool demodFromName(const std::string& name, Demod& out) {
    for (int i = 0; i < int(Demod::Count); ++i)
        if (name == demodName(Demod(i))) {
            out = Demod(i);
            return true;
        }
    return false;
}

const DemodLimits& demodLimits(Demod m) {
    static const DemodLimits table[] = {
        /* WFM */ {50e3, 200e3, 200e3, 10e3, 100e3},
        /* NFM */ {4e3, 25e3, 12.5e3, 500.0, 12.5e3},
        /* AM  */ {2.5e3, 15e3, 10e3, 500.0, 1e3},
        /* SAM */ {2.5e3, 15e3, 10e3, 500.0, 1e3},
        /* USB */ {1.8e3, 4e3, 2.8e3, 100.0, 100.0},
        /* LSB */ {1.8e3, 4e3, 2.8e3, 100.0, 100.0},
        /* DSB */ {2e3, 10e3, 5e3, 500.0, 100.0},
        /* CW  */ {100.0, 1000.0, 500.0, 50.0, 10.0},
        /* RAW */ {1e3, 48e3, 48e3, 1e3, 1e3},
    };
    return table[std::min(int(m), int(Demod::Count) - 1)];
}

void passband(const RxConfig& c, double& lo, double& hi) {
    const double bw = c.bandwidthHz, cut = std::min(c.lowCutHz, bw - 100.0);
    switch (c.mode) {
        case Demod::USB: lo = cut; hi = bw; break;
        case Demod::LSB: lo = -bw; hi = -cut; break;
        default: lo = -bw * 0.5; hi = bw * 0.5; break;
    }
}

NrPreset nrPreset(const RxConfig& c) {
    if (!c.nb && !c.anf && !c.nr) return NrPreset::Off;
    if (c.nb && !c.anf && !c.nr) return NrPreset::NB;
    if (c.nb && c.anf && !c.nr) return NrPreset::NBNotch;
    if (c.nb && c.anf && c.nr) return NrPreset::Full;
    return NrPreset::Custom;
}

void applyNrPreset(RxConfig& c, NrPreset p) {
    c.nb = (p != NrPreset::Off);
    c.anf = (p == NrPreset::NBNotch || p == NrPreset::Full);
    c.nr = (p == NrPreset::Full);
}

const char* nrPresetName(NrPreset p) {
    switch (p) {
        case NrPreset::Off: return "Off";
        case NrPreset::NB: return "NB";
        case NrPreset::NBNotch: return "NB+Notch";
        case NrPreset::Full: return "Full NR";
        default: return "Custom";
    }
}

const std::vector<float>& ctcssTones() {
    static const std::vector<float> tones = {
        67.0f, 69.3f, 71.9f, 74.4f, 77.0f, 79.7f, 82.5f, 85.4f, 88.5f, 91.5f, 94.8f, 97.4f, 100.0f,
        103.5f, 107.2f, 110.9f, 114.8f, 118.8f, 123.0f, 127.3f, 131.8f, 136.5f, 141.3f, 146.2f,
        151.4f, 156.7f, 159.8f, 162.2f, 165.5f, 167.9f, 171.3f, 173.8f, 177.3f, 179.9f, 183.5f,
        186.2f, 189.9f, 192.8f, 196.6f, 199.5f, 203.5f, 206.5f, 210.7f, 218.1f, 225.7f, 229.1f,
        233.6f, 241.8f, 250.3f, 254.1f};
    return tones;
}

Receiver::Receiver(RingBuffer<float>& audioOut) : audio_(audioOut) {
    expTable();
    // Worst-case sizes, so nothing in process() ever grows a vector.
    nbBuf_.reserve(kMaxBlock);
    if_.reserve(kMaxBlock);
    l_.reserve(kMaxBlock);
    r_.reserve(kMaxBlock);
    mpx_.reserve(kMaxBlock);
    la_.reserve(4 * kMaxBlock);
    ra_.reserve(4 * kMaxBlock);
    out_.reserve(8 * kMaxBlock);
    ctcssBuf_.reserve(8192);
}

void Receiver::setInputRate(double fs) {
    inRate_ = fs;
    dirty_.store(true);
}

void Receiver::setConfig(const RxConfig& c) {
    std::lock_guard<std::mutex> lk(cfgMutex_);
    pending_ = c;
    dirty_.store(true);
}

RxConfig Receiver::config() const {
    std::lock_guard<std::mutex> lk(cfgMutex_);
    return pending_;
}

void Receiver::rebuild() {
    RxConfig next;
    {
        std::lock_guard<std::mutex> lk(cfgMutex_);
        next = pending_;
        dirty_.store(false);
    }
    const bool structural = !built_ || next.mode != cfg_.mode || next.bandwidthHz != cfg_.bandwidthHz ||
                            next.lowCutHz != cfg_.lowCutHz || next.cwPitchHz != cfg_.cwPitchHz;
    cfg_ = next;
    if (!structural) {  // squelch, AGC, NB/ANF/NR, de-emphasis...: no filter reset, no glitch
        applyLive();
        return;
    }
    built_ = true;
    const Demod m = cfg_.mode;
    const DemodLimits& lim = demodLimits(m);
    const double bw = std::clamp(cfg_.bandwidthHz, lim.minBw, lim.maxBw);
    const bool ssb = (m == Demod::USB || m == Demod::LSB);
    double pbLo = 0.0, pbHi = 0.0;
    passband(cfg_, pbLo, pbHi);

    // ---- stage 1: integer decimation to the IF rate -------------------------
    double modeMin = 16e3;
    switch (m) {
        case Demod::WFM: modeMin = 200e3; break;
        case Demod::NFM: modeMin = 24e3; break;
        case Demod::USB: case Demod::LSB: modeMin = 12e3; break;
        case Demod::DSB: modeMin = 16e3; break;
        case Demod::CW: modeMin = 8e3; break;
        case Demod::RAW: modeMin = 48e3; break;
        default: break;
    }
    const double occupied = 2.0 * std::max(std::fabs(pbLo), std::fabs(pbHi));
    const double need = std::max(modeMin, 1.4 * occupied);
    decim_ = std::clamp<size_t>(size_t(inRate_ / need), 1, 256);
    ifRate_ = inRate_ / double(decim_);
    decimPhase_ = 0;
    if (decim_ > 1)  // pass to 0.36 fs_if, stop from 0.64 fs_if
        stage1_.setTaps(designLowpass(inRate_, 0.5 * ifRate_, 12 * decim_ + 1, FirWindow::Hamming));

    // ---- stage 2: channel filter (complex band-pass at the IF rate) --------
    const FirWindow win = (m == Demod::WFM) ? FirWindow::Hamming : FirWindow::Blackman;
    const double width = pbHi - pbLo, shift = 0.5 * (pbLo + pbHi);
    const double tw = ssb ? 200.0 : std::max(width * 0.2, 25.0);
    const size_t nTaps = firTapsFor(ifRate_, tw, win, 1025);
    const std::vector<float> proto = designLowpass(ifRate_, width * 0.5, nTaps, win);
    std::vector<cf32> taps(nTaps);
    for (size_t k = 0; k < nTaps; ++k) {
        const double a = 2.0 * std::numbers::pi * shift * (double(k) - double(nTaps - 1) / 2.0) / ifRate_;
        taps[k] = cf32(float(std::cos(a)), float(std::sin(a))) * proto[k];
    }
    channel_.setTaps(taps);

    // ---- audio resampler ------------------------------------------------------
    double pass = 5e3, stop = 7e3;
    switch (m) {
        case Demod::WFM: pass = 15e3; stop = 19e3; break;
        case Demod::NFM: pass = std::min(bw * 0.5, 5e3); stop = pass * 1.4; break;
        case Demod::AM: case Demod::SAM: pass = bw * 0.5; stop = pass * 1.3; break;
        case Demod::USB: case Demod::LSB: pass = bw; stop = pass * 1.25; break;
        case Demod::DSB: pass = bw * 0.5; stop = pass * 1.3; break;
        case Demod::CW: pass = double(cfg_.cwPitchHz) + bw * 0.5 + 100.0; stop = pass * 1.5; break;
        case Demod::RAW: pass = bw * 0.5; stop = pass * 1.2; break;
        default: break;
    }
    stop = std::min(stop, 0.5 * std::min(ifRate_, kAudioRate));
    pass = std::min(pass, 0.85 * stop);
    resL_.design(ifRate_, kAudioRate, pass, stop);
    resR_.design(ifRate_, kAudioRate, pass, stop);

    // ---- demodulator state ----------------------------------------------------
    nb_.configure(inRate_);
    nb_.reset();
    rds_.configure(ifRate_);
    lastOffset_ = offsetHz_.load();

    const double dev = (m == Demod::WFM) ? 75e3 : bw * 0.5;
    fmScale_ = float(ifRate_ / (2.0 * std::numbers::pi * dev));
    fmPrev_ = {1.0f, 0.0f};

    auto loopGains = [&](double noiseBwHz, float& alpha, float& beta) {
        const double wn = 2.0 * std::numbers::pi * noiseBwHz / ifRate_;
        alpha = float(2.0 * 0.7071 * wn);
        beta = float(wn * wn);
    };
    pilotNominal_ = float(19e3 / ifRate_);
    pilotFreq_ = pilotNominal_;
    pilotPhase_ = 0.0f;
    pilotLp_ = {0.0f, 0.0f};
    pilotMag_ = 0.0f;
    pilotLpCoef_ = float(1.0 - std::exp(-2.0 * std::numbers::pi * 100.0 / ifRate_));
    loopGains(8.0, pilotAlpha_, pilotBeta_);
    stereoLocked.store(false);

    samPhase_ = samFreq_ = 0.0f;
    samMaxFreq_ = float(bw * 0.25 / ifRate_);
    loopGains(30.0, samAlpha_, samBeta_);
    bfoPhase_ = 0;
    bfoInc_ = phaseInc(cfg_.cwPitchHz, ifRate_);

    agcEnv_ = 1e-3f;
    agcHang_ = 0;
    deL_ = deR_ = 0.0f;
    dcAvg_ = 0.0f;
    nfmHp_.highpass(kAudioRate, 300.0);
    nfmHp_.reset();
    ctcssLp_.lowpass(kAudioRate, 300.0);
    ctcssLp_.reset();
    ctcssBuf_.clear();
    ctcssAcc_ = 0.0f;
    ctcssAccN_ = 0;
    ctcssLast_ = -1;
    ctcssMiss_ = 0;
    ctcssHz.store(0.0f);
    anf_.reset();
    nr_.reset();
    applyLive();
}

void Receiver::applyLive() {
    const Demod m = cfg_.mode;
    const bool fm = (m == Demod::WFM || m == Demod::NFM);
    const double tau = cfg_.deemphasis == 1 ? 50e-6 : 75e-6;
    deCoef_ = (fm && cfg_.deemphasis != 0) ? float(1.0 - std::exp(-1.0 / (tau * kAudioRate))) : 1.0f;
    static const double decayS[] = {0.1, 0.5, 2.0}, hangS[] = {0.05, 0.25, 1.0};
    const int a = std::clamp(cfg_.agc, 0, 2);
    agcDecay_ = float(std::exp(-1.0 / (decayS[a] * kAudioRate)));
    agcAttack_ = float(1.0 - std::exp(-1.0 / (0.002 * kAudioRate)));
    agcHangSamples_ = int(hangS[a] * kAudioRate);
    nb_.threshold = std::clamp(cfg_.nbThreshold, 1.5f, 8.0f);
    nb_.holdoffS = std::clamp(cfg_.nbHoldoffUs, 1.0f, 200.0f) * 1e-6f;
    nr_.strength = std::clamp(cfg_.nrStrength, 0.0f, 1.0f);
    if (!cfg_.anf) anf_.reset();
    if (!cfg_.nr) nr_.reset();
    if (m != Demod::NFM || !cfg_.ctcss) ctcssHz.store(0.0f);
}

void Receiver::process(const cf32* in, size_t n) {
    if (dirty_.load(std::memory_order_acquire)) rebuild();
    while (n > 0) {
        const size_t c = std::min(n, kMaxBlock);
        processChunk(in, c);
        in += c;
        n -= c;
    }
}

void Receiver::processChunk(const cf32* in, size_t n) {
    const auto& tab = expTable();
    const double offset = offsetHz_.load(std::memory_order_relaxed);
    if (std::fabs(offset - lastOffset_) > 1e3) {  // retuned: drop stale RDS / CTCSS state
        rds_.reset();
        ctcssHz.store(0.0f, std::memory_order_relaxed);
        lastOffset_ = offset;
    }

    // ---- impulse noise blanker (wideband, before any filter) ----------------
    if (cfg_.nb) {
        nbBuf_.assign(in, in + n);
        nb_.process(nbBuf_.data(), n);
        in = nbBuf_.data();
        if (nb_.total > uint64_t(inRate_ / 4)) {
            nbBlankedPct.store(100.0f * float(nb_.blanked) / float(nb_.total), std::memory_order_relaxed);
            nb_.blanked = nb_.total = 0;
        }
    } else {
        nbBlankedPct.store(0.0f, std::memory_order_relaxed);
    }

    // ---- mix to baseband and decimate ---------------------------------------
    const uint32_t inc = phaseInc(-offset, inRate_);
    uint32_t ph = ncoPhase_;
    if_.clear();
    if (decim_ == 1) {
        for (size_t i = 0; i < n; ++i) {
            if_.push_back(channel_.filter(in[i] * tab[ph >> 16]));
            ph += inc;
        }
    } else {
        for (size_t i = 0; i < n; ++i) {
            stage1_.push(in[i] * tab[ph >> 16]);
            ph += inc;
            if (++decimPhase_ == decim_) {
                decimPhase_ = 0;
                if_.push_back(channel_.filter(stage1_.compute()));
            }
        }
    }
    ncoPhase_ = ph;
    const size_t m = if_.size();
    if (m == 0) return;

    // ---- channel power + squelch decision -------------------------------------
    float p = 0.0f;
    for (const cf32& s : if_) p += std::norm(s);
    p /= float(m);
    const float k = float(1.0 - std::exp(-double(m) / (0.05 * ifRate_)));
    powerAvg_ += k * (p - powerAvg_);
    const float db = 10.0f * std::log10(powerAvg_ + 1e-20f);
    levelDb.store(db, std::memory_order_relaxed);
    if (!cfg_.squelch) gateOpen_ = true;
    else if (db > cfg_.squelchDb) gateOpen_ = true;
    else if (db < cfg_.squelchDb - 2.0f) gateOpen_ = false;  // 2 dB hysteresis
    squelchOpen.store(gateOpen_, std::memory_order_relaxed);

    demodulate(m);

    // ---- resample to 48 kHz, tracking the sound card clock ----------------------
    if (clockTracking_) {
        const double fill = double(audio_.readable() / 2);
        const double err = std::clamp((fill - double(kAudioTargetFrames)) / double(kAudioTargetFrames), -1.0, 1.0);
        const double trim = 1.0 + 0.003 * err;
        resL_.setTrim(trim);
        resR_.setTrim(trim);
    }
    la_.clear();
    ra_.clear();
    resL_.process(l_.data(), m, la_);
    const bool twoCh = !r_.empty();
    if (twoCh) resR_.process(r_.data(), m, ra_);

    postProcess();
}

void Receiver::demodulate(size_t n) {
    const auto& tab = expTable();
    l_.resize(n);
    r_.clear();

    switch (cfg_.mode) {
        case Demod::WFM: {
            // Discriminator -> multiplex. A PLL tracks the 19 kHz pilot; the
            // regenerated 38 kHz carrier demodulates L-R synchronously.
            const bool wantStereo = cfg_.stereo;
            if (wantStereo) r_.resize(n);
            mpx_.resize(n);
            const float lockCoef = float(1.0 - std::exp(-2.0 * std::numbers::pi * 3.0 / ifRate_));
            const float fMin = pilotNominal_ - float(60.0 / ifRate_), fMax = pilotNominal_ + float(60.0 / ifRate_);
            bool locked = stereoLocked.load(std::memory_order_relaxed);
            for (size_t i = 0; i < n; ++i) {
                const cf32 d = if_[i] * std::conj(fmPrev_);
                fmPrev_ = if_[i];
                const float mpx = std::atan2(d.imag(), d.real()) * fmScale_;
                mpx_[i] = mpx;
                if (!wantStereo) {
                    l_[i] = mpx;
                    continue;
                }
                const uint32_t idx = turnsToPhase(pilotPhase_);
                const cf32 e = tab[idx >> 16];
                pilotLp_ += pilotLpCoef_ * (cf32(mpx * e.real(), -mpx * e.imag()) - pilotLp_);
                // Linear phase detector: audio that leaks through the pilot
                // low-pass averages out instead of biasing the loop.
                const float err = std::clamp(pilotLp_.imag() * 4.0f, -0.1f, 0.1f);
                pilotFreq_ = std::clamp(pilotFreq_ + pilotBeta_ * err, fMin, fMax);
                pilotPhase_ += pilotFreq_ + pilotAlpha_ * err;
                pilotPhase_ -= std::floor(pilotPhase_);
                pilotMag_ += lockCoef * (pilotLp_.real() - pilotMag_);  // coherent pilot amplitude / 2
                if (locked) {
                    const float s = mpx * (-2.0f * tab[uint32_t(idx * 2u) >> 16].imag());
                    l_[i] = mpx + s;
                    r_[i] = mpx - s;
                } else {
                    l_[i] = r_[i] = mpx;
                }
            }
            if (wantStereo) locked = locked ? pilotMag_ > 0.012f : pilotMag_ > 0.02f;
            else locked = false;
            stereoLocked.store(locked, std::memory_order_relaxed);
            if (cfg_.rds) rds_.process(mpx_.data(), n);
            break;
        }
        case Demod::NFM:
            for (size_t i = 0; i < n; ++i) {
                const cf32 d = if_[i] * std::conj(fmPrev_);
                fmPrev_ = if_[i];
                l_[i] = std::atan2(d.imag(), d.real()) * fmScale_;
            }
            break;
        case Demod::AM:
            for (size_t i = 0; i < n; ++i) l_[i] = std::abs(if_[i]);
            break;
        case Demod::SAM:
            // Carrier-locked PLL; the in-phase product is the audio.
            for (size_t i = 0; i < n; ++i) {
                const cf32 y = if_[i] * std::conj(tab[turnsToPhase(samPhase_) >> 16]);
                const float err = std::atan2(y.imag(), y.real()) * (1.0f / kTwoPi);
                samFreq_ = std::clamp(samFreq_ + samBeta_ * err, -samMaxFreq_, samMaxFreq_);
                samPhase_ += samFreq_ + samAlpha_ * err;
                samPhase_ -= std::floor(samPhase_);
                l_[i] = y.real();
            }
            break;
        case Demod::USB:
        case Demod::LSB:
        case Demod::DSB:  // product detector at the VFO frequency
            for (size_t i = 0; i < n; ++i) l_[i] = if_[i].real();
            break;
        case Demod::CW:
            for (size_t i = 0; i < n; ++i) {
                l_[i] = (if_[i] * tab[bfoPhase_ >> 16]).real();
                bfoPhase_ += bfoInc_;
            }
            break;
        case Demod::RAW:
            r_.resize(n);
            for (size_t i = 0; i < n; ++i) {
                l_[i] = if_[i].real();
                r_[i] = if_[i].imag();
            }
            break;
        default: break;
    }
}

void Receiver::runCtcss(const float* audio, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        ctcssAcc_ += ctcssLp_.process(audio[i]);
        if (++ctcssAccN_ == 8) {
            ctcssBuf_.push_back(ctcssAcc_ * 0.125f);
            ctcssAcc_ = 0.0f;
            ctcssAccN_ = 0;
        }
    }
    if (ctcssBuf_.size() < kCtcssWindow) return;

    float mean = 0.0f;
    for (size_t i = 0; i < kCtcssWindow; ++i) mean += ctcssBuf_[i];
    mean /= float(kCtcssWindow);
    float energy = 0.0f;
    for (size_t i = 0; i < kCtcssWindow; ++i) {
        ctcssBuf_[i] -= mean;
        energy += ctcssBuf_[i] * ctcssBuf_[i];
    }
    const auto& tones = ctcssTones();
    int best = -1;
    float bestP = 0.0f;
    for (size_t t = 0; t < tones.size(); ++t) {
        const float c = 2.0f * std::cos(kTwoPi * tones[t] / float(kCtcssRate));
        float s1 = 0.0f, s2 = 0.0f;
        for (size_t i = 0; i < kCtcssWindow; ++i) {
            const float s0 = ctcssBuf_[i] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const float pw = s1 * s1 + s2 * s2 - c * s1 * s2;
        if (pw > bestP) { bestP = pw; best = int(t); }
    }
    ctcssBuf_.erase(ctcssBuf_.begin(), ctcssBuf_.begin() + std::ptrdiff_t(kCtcssWindow));

    // Fraction of the sub-300 Hz energy that sits in the strongest tone bin.
    const float ratio = bestP / (energy * float(kCtcssWindow) * 0.5f + 1e-20f);
    const bool hit = gateOpen_ && best >= 0 && ratio > 0.4f && energy / float(kCtcssWindow) > 1e-6f;
    if (hit && best == ctcssLast_) {
        ctcssHz.store(tones[size_t(best)], std::memory_order_relaxed);
        ctcssMiss_ = 0;
    } else if (!hit && ++ctcssMiss_ >= 2) {
        ctcssHz.store(0.0f, std::memory_order_relaxed);
    }
    ctcssLast_ = hit ? best : -1;
}

void Receiver::postProcess() {
    const size_t n = la_.size();
    if (n == 0) return;
    const bool twoCh = ra_.size() == n;
    const Demod m = cfg_.mode;

    if (m == Demod::NFM) {
        if (cfg_.ctcss) runCtcss(la_.data(), n);
        for (size_t i = 0; i < n; ++i) la_[i] = nfmHp_.process(la_[i]);  // strip sub-audible tones
    }
    if (m == Demod::AM) {  // remove the carrier term from the envelope
        const float a = float(1.0 - std::exp(-2.0 * std::numbers::pi * 20.0 / kAudioRate));
        for (size_t i = 0; i < n; ++i) {
            dcAvg_ += a * (la_[i] - dcAvg_);
            la_[i] -= dcAvg_;
        }
    }
    if (m == Demod::SAM) {
        const float a = float(1.0 - std::exp(-2.0 * std::numbers::pi * 20.0 / kAudioRate));
        for (size_t i = 0; i < n; ++i) {
            dcAvg_ += a * (la_[i] - dcAvg_);
            la_[i] -= dcAvg_;
        }
    }
    if (deCoef_ < 1.0f) {
        for (size_t i = 0; i < n; ++i) {
            deL_ += deCoef_ * (la_[i] - deL_);
            la_[i] = deL_;
        }
        if (twoCh)
            for (size_t i = 0; i < n; ++i) {
                deR_ += deCoef_ * (ra_[i] - deR_);
                ra_[i] = deR_;
            }
    }
    // Interference removal on voice audio. The auto-notch is kept off CW (it
    // would remove the wanted tone) and WFM / RAW (music, IQ data).
    const bool voice = (m == Demod::NFM || m == Demod::AM || m == Demod::SAM || m == Demod::USB || m == Demod::LSB ||
                        m == Demod::DSB);
    if (cfg_.anf && voice) anf_.process(la_.data(), n);
    if (cfg_.nr && (voice || m == Demod::CW)) nr_.process(la_.data(), n);

    const bool agcMode = (m == Demod::AM || m == Demod::SAM || m == Demod::USB || m == Demod::LSB || m == Demod::DSB ||
                          m == Demod::CW);
    if (agcMode && cfg_.agc == 3) {
        const float g = std::pow(10.0f, cfg_.afGainDb / 20.0f);
        for (size_t i = 0; i < n; ++i) la_[i] *= g;
    } else if (agcMode) {
        constexpr float target = 0.25f, maxGain = 1e4f;
        for (size_t i = 0; i < n; ++i) {
            const float a = std::fabs(la_[i]);
            if (a > agcEnv_) {
                agcEnv_ += agcAttack_ * (a - agcEnv_);
                agcHang_ = agcHangSamples_;
            } else if (agcHang_ > 0) {
                --agcHang_;
            } else {
                agcEnv_ *= agcDecay_;
            }
            la_[i] *= std::min(target / std::max(agcEnv_, 1e-7f), maxGain);
        }
    }

    const float modeGain = (m == Demod::WFM) ? 0.7f : 1.0f;
    const float gateTarget = gateOpen_ ? 1.0f : 0.0f;
    out_.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        gateGain_ += 0.01f * (gateTarget - gateGain_);  // ~2 ms click-free gate
        const float g = gateGain_ * modeGain;
        out_[2 * i] = la_[i] * g;
        out_[2 * i + 1] = (twoCh ? ra_[i] : la_[i]) * g;
    }
    const size_t fit = std::min(out_.size(), audio_.writable() & ~size_t(1));
    audio_.write(out_.data(), fit);
    if (fit < out_.size()) audioDrops.fetch_add((out_.size() - fit) / 2, std::memory_order_relaxed);
}

}  // namespace osx
