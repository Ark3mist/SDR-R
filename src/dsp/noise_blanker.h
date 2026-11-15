// Impulse noise blanker on the complex input, before any filtering (a
// channel filter would smear a 1 us spike into a long ring). A sample whose
// magnitude exceeds `threshold` x the running RMS opens a blanking window of
// `holdoff` seconds; blanked samples are replaced by zero. The RMS tracker
// ignores blanked samples so a pulse train cannot raise its own threshold.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>

namespace osx {

struct NoiseBlanker {
    float threshold = 3.0f;     // x RMS, 1.5 .. 8.0
    float holdoffS = 5e-6f;     // blanking continues this long after the last over-threshold sample
    uint64_t blanked = 0, total = 0;

    void configure(double sampleRate) {
        fs_ = sampleRate;
        avgCoef_ = float(1.0 - std::exp(-1.0 / (0.02 * sampleRate)));  // 20 ms power average
    }
    void reset() { power_ = 0.0f; hold_ = 0; }

    void process(std::complex<float>* x, size_t n) {
        if (n == 0) return;
        float p = power_;
        if (p <= 0.0f) {  // first block: seed the tracker from the block's median-ish level
            double s = 0.0;
            for (size_t i = 0; i < n; ++i) s += std::norm(x[i]);
            p = float(s / double(n)) + 1e-12f;
        }
        const float thr2 = threshold * threshold;
        const int holdN = std::max(1, int(holdoffS * fs_));
        const float c = avgCoef_;
        int hold = hold_;
        size_t cnt = 0;
        for (size_t i = 0; i < n; ++i) {
            const float m = std::norm(x[i]);
            const float lim = thr2 * p;
            if (m > lim) hold = holdN;
            // Each sample may raise the average by at most thr^2 x p, so a burst
            // cannot inflate its own threshold, yet a real level step is
            // followed within a few ms (no lock-up).
            p += c * (std::min(m, lim) - p);
            if (hold > 0) {
                --hold;
                x[i] = {0.0f, 0.0f};
                ++cnt;
            }
        }
        power_ = p;
        hold_ = hold;
        blanked += cnt;
        total += n;
    }

private:
    double fs_ = 1e6;
    float avgCoef_ = 1e-4f, power_ = 0.0f;
    int hold_ = 0;
};

}  // namespace osx
