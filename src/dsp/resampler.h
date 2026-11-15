// Arbitrary-ratio polyphase resampler (real float).
//
// A 64-phase windowed-sinc bank with linear interpolation between adjacent
// phases, so any input/output ratio works (the IF rate is whatever integer
// decimation of the hardware rate comes out as) and the ratio can be trimmed
// at run time to track the sound card clock.
#pragma once

#include <cmath>
#include <vector>

#include "dsp/fir.h"

namespace osx {

class ArbResampler {
public:
    static constexpr size_t kPhases = 64;

    // passHz / stopHz are in Hz at the input rate.
    void design(double inRate, double outRate, double passHz, double stopHz, size_t maxTapsPerPhase = 512) {
        step_ = inRate / outRate;
        taps_ = std::clamp<size_t>(size_t(std::ceil(3.3 * inRate / std::max(stopHz - passHz, 1.0))), 8, maxTapsPerPhase);
        const size_t total = taps_ * kPhases;
        std::vector<float> proto = designLowpass(inRate * double(kPhases), 0.5 * (passHz + stopHz), total | 1u, FirWindow::Hamming);
        proto.resize(total + kPhases + 1, 0.0f);
        // bank_[p][j] multiplies x[i - (taps-1) + j]; phase kPhases is phase 0 advanced one sample.
        bank_.assign((kPhases + 1) * taps_, 0.0f);
        for (size_t p = 0; p <= kPhases; ++p)
            for (size_t k = 0; k < taps_; ++k) {
                const size_t idx = p + k * kPhases;
                bank_[p * taps_ + (taps_ - 1 - k)] = (idx < total ? proto[idx] : 0.0f) * float(kPhases);
            }
        buf_.assign(taps_ - 1, 0.0f);
        tpos_ = double(taps_ - 1);
    }

    // Multiplies the nominal step (>1 consumes input faster => fewer outputs).
    void setTrim(double trim) { trim_ = trim; }
    size_t tapsPerPhase() const { return taps_; }

    // Appends resampled output to `out`.
    void process(const float* in, size_t n, std::vector<float>& out) {
        buf_.insert(buf_.end(), in, in + n);
        const double step = step_ * trim_;
        const size_t size = buf_.size();
        for (;;) {
            const size_t i = size_t(tpos_);
            if (i >= size) break;
            const double frac = (tpos_ - double(i)) * double(kPhases);
            const size_t p = size_t(frac);
            const float a = float(frac - double(p));
            const float* x = &buf_[i - (taps_ - 1)];
            const float* h0 = &bank_[p * taps_];
            const float* h1 = h0 + taps_;
            float s0 = 0.0f, s1 = 0.0f;
            for (size_t k = 0; k < taps_; ++k) {
                s0 += x[k] * h0[k];
                s1 += x[k] * h1[k];
            }
            out.push_back(s0 + a * (s1 - s0));
            tpos_ += step;
        }
        const size_t keepFrom = std::min(size, size_t(tpos_) - (taps_ - 1));
        buf_.erase(buf_.begin(), buf_.begin() + std::ptrdiff_t(keepFrom));
        tpos_ -= double(keepFrom);
    }

private:
    std::vector<float> bank_, buf_;
    size_t taps_ = 8;
    double step_ = 1.0, trim_ = 1.0, tpos_ = 7.0;
};

}  // namespace osx
