#include "source/sim_source.h"

#include "core/exp_table.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

namespace osx {

namespace {

constexpr double kTwo32 = 4294967296.0;

inline uint64_t xorshift(uint64_t& s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

// Approximate N(0,1): sum of four 16-bit uniforms (Irwin-Hall), re-centred
// and scaled to unit variance. Cheap enough for 6.4 M draws per second.
inline float gauss(uint64_t& s) {
    const uint64_t r = xorshift(s);
    const int32_t sum = int32_t(r & 0xFFFF) + int32_t((r >> 16) & 0xFFFF) +
                        int32_t((r >> 32) & 0xFFFF) + int32_t(r >> 48);
    return float(sum - 131070) * (1.0f / 37837.2f);
}

}  // namespace

const std::vector<SimSource::Emitter>& SimSource::bandPlan() {
    static const std::vector<Emitter> plan = {
        {"WFM broadcast A", 0.0, -14.0f, Mod::WFM, 55e3f, 1000.0f},
        {"NFM voice ch.", 400e3, -30.0f, Mod::NFM, 3e3f, 1200.0f},
        {"AM carrier", 600e3, -28.0f, Mod::AM, 0.0f, 700.0f},
        {"CW beacon", 750e3, -36.0f, Mod::CW, 0.0f, 9.0f},
        {"WFM broadcast B", 1100e3, -24.0f, Mod::WFM, 45e3f, 3000.0f},
        {"Weak carrier", 1500e3, -62.0f, Mod::Carrier, 0.0f, 0.0f},
        {"Sweeper", 1700e3, -40.0f, Mod::Sweep, 60e3f, 0.25f},
    };
    return plan;
}

SimSource::SimSource() : state_(bandPlan().size()) { expTable(); }

SimSource::~SimSource() { stop(); }

std::vector<double> SimSource::sampleRates() const {
    return {250e3, 1.024e6, 1.536e6, 1.792e6, 1.92e6, 2.048e6, 2.16e6, 2.4e6, 2.56e6, 2.88e6, 3.2e6};
}

std::vector<float> SimSource::gainSteps() const {
    std::vector<float> g;
    for (int db = -40; db <= 20; db += 2) g.push_back(float(db));
    return g;
}

bool SimSource::start(RingBuffer<uint8_t>& out) {
    if (running_.exchange(true)) return true;
    thread_ = std::thread(&SimSource::run, this, &out);
    return true;
}

void SimSource::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void SimSource::generate(uint8_t* dst, size_t n) {
    const auto& tab = expTable();
    const auto& plan = bandPlan();
    const double sr = sampleRate_;
    const double fc = centerHz_.load(std::memory_order_relaxed);
    const float gain = std::pow(10.0f, gainDb.load(std::memory_order_relaxed) / 20.0f);

    acc_.assign(n, {0.0f, 0.0f});
    env_.resize(n);

    if (emittersEnabled.load(std::memory_order_relaxed)) {
        for (size_t e = 0; e < plan.size(); ++e) {
            const Emitter& em = plan[e];
            EmitterState& st = state_[e];

            // Which replicas of this emitter fall inside the passband?
            const double lo = fc - 0.49 * sr, hi = fc + 0.49 * sr;
            const auto kMin = int64_t(std::ceil((lo - em.offsetHz) / kPlanPeriodHz));
            const auto kMax = int64_t(std::floor((hi - em.offsetHz) / kPlanPeriodHz));
            if (kMax < kMin) {
                st.carrier.clear();
                continue;
            }

            // Complex envelope at 0 Hz.
            const uint32_t toneInc = phaseInc(em.toneHz, sr);
            switch (em.mod) {
                case Mod::WFM: {
                    // Real stereo multiplex: L = tone, R = 3 x tone,
                    // mpx = 0.9 (M + S sin 2wp t) + 0.1 sin wp t (19 kHz pilot).
                    const uint32_t pilotInc = phaseInc(19e3, sr);
                    const double k = double(em.devHz) / sr * kTwo32;
                    for (size_t i = 0; i < n; ++i) {
                        const float l = tab[st.modPhase >> 16].imag();
                        const float r = tab[uint32_t(st.modPhase * 3u) >> 16].imag();
                        const float pilot = tab[st.pilotPhase >> 16].imag();
                        const float sub = tab[uint32_t(st.pilotPhase * 2u) >> 16].imag();
                        const float m = 0.9f * (0.5f * (l + r) + 0.5f * (l - r) * sub) + 0.1f * pilot;
                        st.modPhase += toneInc;
                        st.pilotPhase += pilotInc;
                        st.fmPhase += uint32_t(int32_t(k * m));
                        env_[i] = tab[st.fmPhase >> 16];
                    }
                    break;
                }
                case Mod::NFM: {
                    // Voice tone plus a 100.0 Hz CTCSS sub-tone at 15 % deviation.
                    const uint32_t ctcssInc = phaseInc(kSimCtcssHz, sr);
                    const double k = double(em.devHz) / sr * kTwo32;
                    for (size_t i = 0; i < n; ++i) {
                        const float m = 0.8f * tab[st.modPhase >> 16].imag() + 0.15f * tab[st.pilotPhase >> 16].imag();
                        st.modPhase += toneInc;
                        st.pilotPhase += ctcssInc;
                        st.fmPhase += uint32_t(int32_t(k * m));
                        env_[i] = tab[st.fmPhase >> 16];
                    }
                    break;
                }
                case Mod::Sweep: {
                    const double k = double(em.devHz) / sr * kTwo32;
                    for (size_t i = 0; i < n; ++i) {
                        const float m = tab[st.modPhase >> 16].imag();
                        st.modPhase += toneInc;
                        st.fmPhase += uint32_t(int32_t(k * m));
                        env_[i] = tab[st.fmPhase >> 16];
                    }
                    break;
                }
                case Mod::AM:
                    for (size_t i = 0; i < n; ++i) {
                        const float m = tab[st.modPhase >> 16].imag();
                        st.modPhase += toneInc;
                        env_[i] = {0.55f * (1.0f + 0.8f * m), 0.0f};
                    }
                    break;
                case Mod::CW: {
                    // 16-slot keying pattern, toneHz slots per second.
                    constexpr uint16_t pattern = 0b1110'1011'1010'0010;
                    const uint32_t slotInc = phaseInc(em.toneHz / 16.0, sr);
                    const float a = 1.0f - std::exp(-1.0f / (0.004f * float(sr)));  // 4 ms edges
                    for (size_t i = 0; i < n; ++i) {
                        const float key = ((pattern >> (st.modPhase >> 28)) & 1u) ? 1.0f : 0.0f;
                        st.modPhase += slotInc;
                        st.keyAmp += a * (key - st.keyAmp);
                        env_[i] = {st.keyAmp, 0.0f};
                    }
                    break;
                }
                case Mod::Carrier:
                    std::fill(env_.begin(), env_.end(), std::complex<float>(1.0f, 0.0f));
                    break;
            }

            const float amp = gain * std::pow(10.0f, em.levelDb / 20.0f);
            if (st.carrier.size() > 16) st.carrier.clear();
            for (int64_t k = kMin; k <= kMax; ++k) {
                const double f = double(k) * kPlanPeriodHz + em.offsetHz - fc;
                const uint32_t inc = phaseInc(f, sr);
                uint32_t ph = st.carrier[k];
                for (size_t i = 0; i < n; ++i) {
                    acc_[i] += amp * env_[i] * tab[ph >> 16];
                    ph += inc;
                }
                st.carrier[k] = ph;
            }
        }
    }

    const float sigma = std::pow(10.0f, noiseDb.load(std::memory_order_relaxed) / 20.0f) *
                        float(std::numbers::sqrt2 / 2.0);
    const float dc = dcOffset.load(std::memory_order_relaxed);
    auto quant = [](float v) {
        const float q = 127.5f + 127.5f * v;
        return uint8_t(q <= 0.0f ? 0 : (q >= 255.0f ? 255 : int(q + 0.5f)));
    };
    for (size_t i = 0; i < n; ++i) {
        dst[2 * i] = quant(acc_[i].real() + sigma * gauss(rng_) + dc);
        dst[2 * i + 1] = quant(acc_[i].imag() + sigma * gauss(rng_) + dc);
    }
}

void SimSource::run(RingBuffer<uint8_t>* out) {
    using clock = std::chrono::steady_clock;
    const double sr = sampleRate_;
    const size_t block = std::max<size_t>(1024, size_t(sr / 200.0));  // ~5 ms
    std::vector<uint8_t> buf(block * 2);
    const auto period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(double(block) / sr));
    auto next = clock::now();

    while (running_.load(std::memory_order_relaxed)) {
        generate(buf.data(), block);
        const size_t w = out->write(buf.data(), buf.size());
        if (w < buf.size()) overruns_.fetch_add(buf.size() - w, std::memory_order_relaxed);

        next += period;
        const auto now = clock::now();
        if (now - next > std::chrono::milliseconds(250)) next = now;  // fell behind: resync
        std::this_thread::sleep_until(next);
    }
}

}  // namespace osx
