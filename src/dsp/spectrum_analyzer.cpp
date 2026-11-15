#include "dsp/spectrum_analyzer.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <cmath>
#include <numbers>

#include <fftw3.h>

namespace osx {

const char* fftWindowName(FftWindow w) {
    switch (w) {
        case FftWindow::Rectangular: return "Rectangular";
        case FftWindow::Hann: return "Hann";
        case FftWindow::BlackmanHarris: return "Blackman-Harris";
        case FftWindow::Nuttall: return "Nuttall";
        default: return "?";
    }
}

void makeWindow(FftWindow w, std::vector<float>& out, size_t n) {
    out.resize(n);
    double a0 = 1.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
    switch (w) {
        case FftWindow::Hann: a0 = 0.5; a1 = 0.5; break;
        case FftWindow::BlackmanHarris: a0 = 0.35875; a1 = 0.48829; a2 = 0.14128; a3 = 0.01168; break;
        case FftWindow::Nuttall: a0 = 0.355768; a1 = 0.487396; a2 = 0.144232; a3 = 0.012604; break;
        default: break;
    }
    const double k = 2.0 * std::numbers::pi / double(n);  // periodic (DFT-even) form
    for (size_t i = 0; i < n; ++i) {
        const double x = k * double(i);
        out[i] = float(a0 - a1 * std::cos(x) + a2 * std::cos(2 * x) - a3 * std::cos(3 * x));
    }
}

SpectrumFft::SpectrumFft(size_t n, FftWindow w) : n_(n) {
    makeWindow(w, window_, n);
    double sum = 0.0;
    for (float v : window_) sum += v;
    norm_ = float(1.0 / sum);  // coherent gain: full-scale tone -> 0 dBFS
    in_ = fftwf_malloc(sizeof(fftwf_complex) * n);
    out_ = fftwf_malloc(sizeof(fftwf_complex) * n);
    std::lock_guard<std::mutex> lk(fftwPlannerMutex());
    plan_ = fftwf_plan_dft_1d(int(n), static_cast<fftwf_complex*>(in_),
                              static_cast<fftwf_complex*>(out_), FFTW_FORWARD, FFTW_ESTIMATE);
}

SpectrumFft::~SpectrumFft() {
    {
        std::lock_guard<std::mutex> lk(fftwPlannerMutex());
        fftwf_destroy_plan(static_cast<fftwf_plan>(plan_));
    }
    fftwf_free(in_);
    fftwf_free(out_);
}

void SpectrumFft::compute(const cf32* ring, size_t start, float* outDb) {
    auto* in = static_cast<fftwf_complex*>(in_);
    auto* out = static_cast<fftwf_complex*>(out_);
    for (size_t i = 0; i < n_; ++i) {
        const cf32 s = ring[(start + i) % n_] * window_[i];
        in[i][0] = s.real();
        in[i][1] = s.imag();
    }
    fftwf_execute(static_cast<fftwf_plan>(plan_));
    const float n2 = norm_ * norm_;
    const size_t half = n_ / 2;
    for (size_t i = 0; i < n_; ++i) {
        const fftwf_complex& b = out[(i + half) % n_];  // fftshift: DC in the middle
        outDb[i] = 10.0f * std::log10((b[0] * b[0] + b[1] * b[1]) * n2 + 1e-20f);
    }
}

SpectrumAnalyzer::SpectrumAnalyzer(RingBuffer<cf32>& tap) : tap_(tap) {}

SpectrumAnalyzer::~SpectrumAnalyzer() { stop(); }

void SpectrumAnalyzer::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&SpectrumAnalyzer::run, this);
}

void SpectrumAnalyzer::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

bool SpectrumAnalyzer::popLine(std::vector<float>& out) {
    std::lock_guard<std::mutex> lk(outMutex_);
    if (lines_.empty()) return false;
    out.swap(lines_.front());
    lines_.pop_front();
    return true;
}

void SpectrumAnalyzer::run() {
    using clock = std::chrono::steady_clock;
    constexpr size_t kMaxPending = 8;

    std::unique_ptr<SpectrumFft> fft;
    std::vector<cf32> hist, chunk(16384);
    size_t wr = 0;          // next write position in hist (== oldest sample)
    size_t filled = 0;      // valid samples in hist
    size_t fresh = 0;       // samples received since the last published line
    int curWindow = -1;
    auto nextTick = clock::now();

    while (running_.load(std::memory_order_relaxed)) {
        const size_t n = size_t(fftSize_.load(std::memory_order_relaxed));
        const int w = window_.load(std::memory_order_relaxed);
        if (!fft || fft->size() != n || w != curWindow) {
            fft = std::make_unique<SpectrumFft>(n, FftWindow(w));
            curWindow = w;
            if (hist.size() != n) {
                hist.assign(n, cf32{});
                wr = filled = fresh = 0;
            }
        }

        // Drain the tap into the sliding history (only the newest n matter).
        for (;;) {
            const size_t got = tap_.read(chunk.data(), chunk.size());
            if (got == 0) break;
            for (size_t i = 0; i < got; ++i) {
                hist[wr] = chunk[i];
                wr = (wr + 1 == n) ? 0 : wr + 1;
            }
            filled = std::min(n, filled + got);
            fresh += got;
        }

        const auto period = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double>(1.0 / double(std::max(1, rate_.load()))));
        auto now = clock::now();
        if (now >= nextTick) {
            if (fresh > 0 && filled == n) {
                std::vector<float> line(n);
                fft->compute(hist.data(), wr, line.data());
                fresh = 0;
                std::lock_guard<std::mutex> lk(outMutex_);
                if (lines_.size() >= kMaxPending) lines_.pop_front();
                lines_.push_back(std::move(line));
            }
            nextTick += period;
            if (now - nextTick > period * 4) nextTick = now + period;
        }
        now = clock::now();
        std::this_thread::sleep_for(std::clamp(
            std::chrono::duration_cast<std::chrono::microseconds>(nextTick - now),
            std::chrono::microseconds(200), std::chrono::microseconds(5000)));
    }
}

}  // namespace osx
