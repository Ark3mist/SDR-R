// Adaptive auto-notch: 64-tap normalised LMS (Widrow-Hoff) linear predictor.
// The filter predicts x[n] from samples delayed by `kDelay`; periodic signals
// (carriers, heterodynes, birdies) stay correlated across the delay and are
// predicted, speech and noise are not. Output = x - prediction, so steady
// tones are subtracted and everything else passes.
#pragma once

#include <array>
#include <cstddef>

namespace osx {

class AutoNotch {
public:
    static constexpr size_t kTaps = 64;
    static constexpr size_t kDelay = 16;    // decorrelation delay, samples
    static constexpr size_t kLen = kTaps + kDelay;
    float mu = 0.02f;                       // normalised step size
    float leak = 1e-5f;                     // coefficient leakage keeps the filter bounded

    void reset() {
        w_.fill(0.0f);
        hist_.fill(0.0f);
        p_ = 0;
    }

    void process(float* x, size_t n) {
        const float keep = 1.0f - leak;
        for (size_t i = 0; i < n; ++i) {
            // hist_[p_ + j] == x[n-1-j]; taps see x[n-kDelay-k], k = 0..kTaps-1.
            const float* tap = &hist_[p_ + kDelay - 1];
            float y = 0.0f, energy = 0.0f;
            for (size_t k = 0; k < kTaps; ++k) {
                y += w_[k] * tap[k];
                energy += tap[k] * tap[k];
            }
            const float in = x[i];
            const float e = in - y;
            const float g = mu * e / (energy + 1e-9f);
            for (size_t k = 0; k < kTaps; ++k) w_[k] = keep * w_[k] + g * tap[k];
            p_ = (p_ == 0) ? kLen - 1 : p_ - 1;
            hist_[p_] = hist_[p_ + kLen] = in;
            x[i] = e;
        }
    }

private:
    std::array<float, kTaps> w_{};
    std::array<float, 2 * kLen> hist_{};
    size_t p_ = 0;
};

}  // namespace osx
