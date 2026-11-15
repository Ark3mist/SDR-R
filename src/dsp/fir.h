// FIR design + streaming FIR filter primitives.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <vector>

namespace osx {

enum class FirWindow { Hamming, Blackman };

// Approximate tap count for a given transition width (always odd).
inline size_t firTapsFor(double fs, double transitionHz, FirWindow w, size_t maxTaps) {
    const double k = (w == FirWindow::Hamming) ? 3.3 : 5.5;
    size_t n = size_t(std::ceil(k * fs / std::max(transitionHz, 1e-3)));
    n = std::clamp<size_t>(n, 5, maxTaps);
    return n | 1u;
}

// Windowed-sinc low-pass, unity DC gain. fc is the -6 dB point.
inline std::vector<float> designLowpass(double fs, double fc, size_t taps, FirWindow w = FirWindow::Blackman) {
    std::vector<double> h(taps);
    const double mid = double(taps - 1) / 2.0;
    const double wc = 2.0 * std::numbers::pi * std::min(fc, fs * 0.4999) / fs;
    double sum = 0.0;
    for (size_t i = 0; i < taps; ++i) {
        const double m = double(i) - mid;
        const double sinc = (std::fabs(m) < 1e-9) ? wc / std::numbers::pi : std::sin(wc * m) / (std::numbers::pi * m);
        const double x = 2.0 * std::numbers::pi * double(i) / double(taps - 1);
        const double win = (w == FirWindow::Hamming) ? 0.54 - 0.46 * std::cos(x)
                                                     : 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2.0 * x);
        h[i] = sinc * win;
        sum += h[i];
    }
    std::vector<float> out(taps);
    for (size_t i = 0; i < taps; ++i) out[i] = float(h[i] / sum);
    return out;
}

// Streaming FIR filters on complex samples. Delay lines are stored as
// separate I and Q float arrays (structure of arrays), each written twice so
// the convolution always runs over contiguous memory. The inner loops are
// plain float dot products, which GCC/Clang vectorise to NEON (aarch64) or
// SSE/AVX (x86) under -O3 -ffast-math. All storage is sized in setTaps();
// push()/compute() never allocate.

// Complex samples, real taps.
class FirCR {
public:
    void setTaps(const std::vector<float>& taps) {
        n_ = taps.size();
        h_.assign(taps.rbegin(), taps.rend());  // reversed: oldest sample meets h_[0]
        re_.assign(2 * n_, 0.0f);
        im_.assign(2 * n_, 0.0f);
        pos_ = 0;
    }
    size_t size() const { return n_; }
    void reset() {
        std::fill(re_.begin(), re_.end(), 0.0f);
        std::fill(im_.begin(), im_.end(), 0.0f);
    }
    void push(std::complex<float> x) {
        re_[pos_] = re_[pos_ + n_] = x.real();
        im_[pos_] = im_[pos_ + n_] = x.imag();
        pos_ = (pos_ + 1 == n_) ? 0 : pos_ + 1;
    }
    std::complex<float> compute() const {
        const float* r = &re_[pos_];
        const float* q = &im_[pos_];
        const float* h = h_.data();
        float ar = 0.0f, aq = 0.0f;
        for (size_t k = 0; k < n_; ++k) {
            ar += r[k] * h[k];
            aq += q[k] * h[k];
        }
        return {ar, aq};
    }
    std::complex<float> filter(std::complex<float> x) {
        push(x);
        return compute();
    }

private:
    size_t n_ = 0, pos_ = 0;
    std::vector<float> h_, re_, im_;
};

// Complex samples, complex taps (shifted band-pass, e.g. SSB).
class FirCC {
public:
    void setTaps(const std::vector<std::complex<float>>& taps) {
        n_ = taps.size();
        hr_.resize(n_);
        hi_.resize(n_);
        for (size_t k = 0; k < n_; ++k) {
            hr_[k] = taps[n_ - 1 - k].real();
            hi_[k] = taps[n_ - 1 - k].imag();
        }
        re_.assign(2 * n_, 0.0f);
        im_.assign(2 * n_, 0.0f);
        pos_ = 0;
    }
    size_t size() const { return n_; }
    std::complex<float> filter(std::complex<float> x) {
        re_[pos_] = re_[pos_ + n_] = x.real();
        im_[pos_] = im_[pos_ + n_] = x.imag();
        pos_ = (pos_ + 1 == n_) ? 0 : pos_ + 1;
        const float* r = &re_[pos_];
        const float* q = &im_[pos_];
        float rr = 0.0f, qi = 0.0f, ri = 0.0f, qr = 0.0f;
        for (size_t k = 0; k < n_; ++k) {
            rr += r[k] * hr_[k];
            qi += q[k] * hi_[k];
            ri += r[k] * hi_[k];
            qr += q[k] * hr_[k];
        }
        return {rr - qi, ri + qr};
    }

private:
    size_t n_ = 0, pos_ = 0;
    std::vector<float> hr_, hi_, re_, im_;
};

// Real samples, real taps.
class FirRR {
public:
    void setTaps(const std::vector<float>& taps) {
        n_ = taps.size();
        h_.assign(taps.rbegin(), taps.rend());
        d_.assign(2 * n_, 0.0f);
        pos_ = 0;
    }
    float filter(float x) {
        d_[pos_] = d_[pos_ + n_] = x;
        pos_ = (pos_ + 1 == n_) ? 0 : pos_ + 1;
        const float* d = &d_[pos_];
        float acc = 0.0f;
        for (size_t k = 0; k < n_; ++k) acc += d[k] * h_[k];
        return acc;
    }

private:
    size_t n_ = 0, pos_ = 0;
    std::vector<float> h_, d_;
};

// RBJ biquad (direct form I, float).
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    void lowpass(double fs, double fc, double q = 0.7071) { design(fs, fc, q, false); }
    void highpass(double fs, double fc, double q = 0.7071) { design(fs, fc, q, true); }
    void reset() { x1 = x2 = y1 = y2 = 0; }
    float process(float x) {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }

private:
    void design(double fs, double fc, double q, bool hp) {
        const double w = 2.0 * std::numbers::pi * fc / fs, c = std::cos(w), alpha = std::sin(w) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        if (hp) { b0 = float((1 + c) / 2 / a0); b1 = float(-(1 + c) / a0); b2 = b0; }
        else { b0 = float((1 - c) / 2 / a0); b1 = float((1 - c) / a0); b2 = b0; }
        a1 = float(-2 * c / a0);
        a2 = float((1 - alpha) / a0);
    }
};

}  // namespace osx
