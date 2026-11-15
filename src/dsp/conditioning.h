// Baseband conditioning on the full-rate complex stream (DSP thread):
// DC-spike canceller and automatic I/Q amplitude/phase balance.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>

namespace osx {

// DC blocker: y[n] = x[n] - x[n-1] + R * y[n-1]  (R = 0.9995 by default).
// Zero at DC, pole at R: at 1.024 MSPS the -3 dB notch is ~80 Hz wide.
struct DcBlocker {
    float r = 0.9995f;
    std::complex<float> x1{0.0f, 0.0f}, y1{0.0f, 0.0f};

    void reset() { x1 = y1 = {0.0f, 0.0f}; }
    void process(std::complex<float>* x, size_t n) {
        float xr = x1.real(), xi = x1.imag(), yr = y1.real(), yi = y1.imag();
        const float R = r;
        for (size_t i = 0; i < n; ++i) {
            const float ir = x[i].real(), ii = x[i].imag();
            yr = ir - xr + R * yr;
            yi = ii - xi + R * yi;
            xr = ir;
            xi = ii;
            x[i] = {yr, yi};
        }
        x1 = {xr, xi};
        y1 = {yr, yi};
    }
};

// Blind I/Q imbalance correction (Gram-Schmidt orthogonalisation).
// For a well-spread (noise-like) input E[IQ] = 0 and E[I^2] = E[Q^2]; any
// deviation is front-end imbalance. Per block:
//   Q1 = Q - (E[IQ]/E[I^2]) I        removes the phase error
//   Q' = Q1 * sqrt(E[I^2]/E[Q1^2])    removes the amplitude error
// The statistics are smoothed over ~0.3 s so modulation cannot pump them.
struct IqBalance {
    double ii = 1.0, qq = 1.0, iq = 0.0;
    float a = 0.0f, g = 1.0f;  // current correction: Q' = g * (Q - a I)
    bool primed = false;

    void reset() { ii = qq = 1.0; iq = 0.0; a = 0.0f; g = 1.0f; primed = false; }

    // Estimated front-end error (for display).
    float phaseErrorDeg() const { return float(std::asin(std::clamp(iq / std::sqrt(ii * qq), -1.0, 1.0)) * 57.29577951308232); }
    float gainErrorDb() const { return float(10.0 * std::log10(qq / ii)); }

    void process(std::complex<float>* x, size_t n, double sampleRate) {
        if (n == 0) return;
        double si = 0.0, sq = 0.0, sx = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const float r = x[i].real(), q = x[i].imag();
            si += double(r) * r;
            sq += double(q) * q;
            sx += double(r) * q;
        }
        si /= double(n);
        sq /= double(n);
        sx /= double(n);
        if (si < 1e-12 || sq < 1e-12) return;
        const double k = primed ? std::min(1.0, double(n) / (0.3 * sampleRate)) : 1.0;
        primed = true;
        ii += k * (si - ii);
        qq += k * (sq - qq);
        iq += k * (sx - iq);
        const double alpha = iq / ii;
        const double q1 = qq - iq * iq / ii;  // E[(Q - alpha I)^2]
        a = float(alpha);
        g = q1 > 1e-15 ? float(std::sqrt(ii / q1)) : 1.0f;
        const float A = a, G = g;
        for (size_t i = 0; i < n; ++i) x[i] = {x[i].real(), G * (x[i].imag() - A * x[i].real())};
    }
};

}  // namespace osx
