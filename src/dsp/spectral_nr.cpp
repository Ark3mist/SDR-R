#include "dsp/spectral_nr.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numbers>

#include <fftw3.h>

#include "core/types.h"

namespace osx {

SpectralNR::SpectralNR() {
    for (size_t i = 0; i < kN; ++i)  // periodic sqrt-Hann: w^2 sums to 1 at 50 % overlap
        win_[i] = float(std::sqrt(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(kN))));
    fftIn_ = fftwf_malloc(sizeof(float) * kN);
    fftOut_ = fftwf_malloc(sizeof(fftwf_complex) * kBins);
    std::lock_guard<std::mutex> lk(fftwPlannerMutex());
    fwd_ = fftwf_plan_dft_r2c_1d(int(kN), static_cast<float*>(fftIn_), static_cast<fftwf_complex*>(fftOut_), FFTW_ESTIMATE);
    inv_ = fftwf_plan_dft_c2r_1d(int(kN), static_cast<fftwf_complex*>(fftOut_), static_cast<float*>(fftIn_), FFTW_ESTIMATE);
    reset();
}

SpectralNR::~SpectralNR() {
    {
        std::lock_guard<std::mutex> lk(fftwPlannerMutex());
        fftwf_destroy_plan(static_cast<fftwf_plan>(fwd_));
        fftwf_destroy_plan(static_cast<fftwf_plan>(inv_));
    }
    fftwf_free(fftIn_);
    fftwf_free(fftOut_);
}

void SpectralNR::reset() {
    in_.fill(0.0f);
    ola_.fill(0.0f);
    out_.fill(0.0f);
    psd_.fill(0.0f);
    noise_.fill(0.0f);
    gain_.fill(1.0f);
    fill_ = 0;
    primed_ = false;
}

void SpectralNR::process(float* x, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        in_[kHop + fill_] = x[i];
        x[i] = out_[fill_];
        if (++fill_ == kHop) {
            frame();
            fill_ = 0;
        }
    }
}

void SpectralNR::frame() {
    auto* t = static_cast<float*>(fftIn_);
    auto* f = static_cast<fftwf_complex*>(fftOut_);
    for (size_t i = 0; i < kN; ++i) t[i] = in_[i] * win_[i];
    fftwf_execute(static_cast<fftwf_plan>(fwd_));

    const float s = std::clamp(strength, 0.0f, 1.0f);
    const float over = 1.0f + 2.0f * s;                          // over-subtraction factor
    const float floorG = std::pow(10.0f, -24.0f * s / 20.0f);    // up to 24 dB of attenuation
    constexpr float kPsdSmooth = 0.6f, kGainSmooth = 0.5f;
    constexpr float kUp = 1.003f;                                 // ~+3.4 dB/s noise-floor rise at 375 frames/s
    for (size_t k = 0; k < kBins; ++k) {
        const float p = f[k][0] * f[k][0] + f[k][1] * f[k][1];
        psd_[k] = primed_ ? kPsdSmooth * psd_[k] + (1.0f - kPsdSmooth) * p : p;
        if (!primed_ || psd_[k] < noise_[k]) noise_[k] = psd_[k];
        else noise_[k] *= kUp;
        float g = 1.0f - over * noise_[k] / (psd_[k] + 1e-20f);
        g = std::max(g, floorG);
        if (s <= 0.0f) g = 1.0f;
        gain_[k] = kGainSmooth * gain_[k] + (1.0f - kGainSmooth) * g;
        f[k][0] *= gain_[k];
        f[k][1] *= gain_[k];
    }
    primed_ = true;

    fftwf_execute(static_cast<fftwf_plan>(inv_));
    const float scale = 1.0f / float(kN);
    for (size_t i = 0; i < kN; ++i) ola_[i] += t[i] * scale * win_[i];
    for (size_t i = 0; i < kHop; ++i) out_[i] = ola_[i];
    for (size_t i = 0; i < kHop; ++i) {
        ola_[i] = ola_[i + kHop];
        ola_[i + kHop] = 0.0f;
        in_[i] = in_[i + kHop];
    }
}

}  // namespace osx
