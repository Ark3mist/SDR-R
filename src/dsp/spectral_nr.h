// Spectral noise reduction: 256-point weighted overlap-add (sqrt-Hann
// analysis and synthesis windows, 50 % overlap => perfect reconstruction at
// unity gain) with per-bin spectral-subtraction gains.
//
// Noise per bin is tracked by a fast-down / slow-up minimum follower, so it
// settles on the floor between words. Gains are smoothed over time to keep
// "musical noise" down. Strength 0..1 sets both over-subtraction and the
// gain floor; 0 is transparent. Latency: 256 samples (5.3 ms at 48 kHz).
// Everything is allocated in the constructor; process() never allocates.
#pragma once

#include <array>
#include <cstddef>

namespace osx {

class SpectralNR {
public:
    static constexpr size_t kN = 256, kHop = kN / 2, kBins = kN / 2 + 1;

    SpectralNR();
    ~SpectralNR();
    SpectralNR(const SpectralNR&) = delete;
    SpectralNR& operator=(const SpectralNR&) = delete;

    float strength = 0.5f;
    void reset();
    void process(float* x, size_t n);  // in place, fixed 256-sample latency

private:
    void frame();

    std::array<float, kN> win_{};
    std::array<float, kN> in_{};        // analysis buffer (last kN input samples)
    std::array<float, kN> ola_{};       // overlap-add accumulator
    std::array<float, kHop> out_{};     // finished output hop
    std::array<float, kBins> psd_{}, noise_{}, gain_{};
    size_t fill_ = 0;                   // samples collected toward the next hop
    bool primed_ = false;
    void* fftIn_ = nullptr;
    void* fftOut_ = nullptr;
    void* fwd_ = nullptr;
    void* inv_ = nullptr;
};

}  // namespace osx
