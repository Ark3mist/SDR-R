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

// Streaming FIR with sample type S and tap type T. The delay line is written
// twice so the convolution always runs over contiguous memory.
template <typename S, typename T>
class Fir {
public:
    void setTaps(std::vector<T> taps) {
        if (taps.size() != taps_.size()) {
            delay_.assign(taps.size() * 2, S{});
            pos_ = 0;
        }
        taps_ = std::move(taps);
    }
    size_t size() const { return taps_.size(); }
    void reset() { std::fill(delay_.begin(), delay_.end(), S{}); }

    void push(S x) {
        const size_t n = taps_.size();
        pos_ = (pos_ == 0) ? n - 1 : pos_ - 1;
        delay_[pos_] = x;
        delay_[pos_ + n] = x;
    }
    S compute() const {
        const size_t n = taps_.size();
        const S* d = &delay_[pos_];
        const T* h = taps_.data();
        S acc{};
        for (size_t k = 0; k < n; ++k) acc += d[k] * h[k];
        return acc;
    }
    S filter(S x) {
        push(x);
        return compute();
    }

private:
    std::vector<T> taps_;
    std::vector<S> delay_;
    size_t pos_ = 0;
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
