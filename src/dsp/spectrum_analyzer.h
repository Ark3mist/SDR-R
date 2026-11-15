// Display FFT worker. Runs on its own thread so neither the DSP engine nor
// the 60 FPS UI thread ever blocks on an FFT. It drains the spectrum tap into
// a sliding history, and at the configured rate windows the newest N samples,
// runs FFTW (fftwf_plan_dft_1d) and publishes a fft-shifted dBFS line.
#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "core/ring_buffer.h"
#include "core/types.h"

namespace osx {

enum class FftWindow { Rectangular = 0, Hann, BlackmanHarris, Nuttall, Count };
const char* fftWindowName(FftWindow w);
void makeWindow(FftWindow w, std::vector<float>& out, size_t n);

// One-shot FFT -> shifted dBFS (a full-scale complex tone reads 0 dBFS).
// Used by the worker and directly by the unit tests.
class SpectrumFft {
public:
    SpectrumFft(size_t n, FftWindow w);
    ~SpectrumFft();
    SpectrumFft(const SpectrumFft&) = delete;
    SpectrumFft& operator=(const SpectrumFft&) = delete;

    size_t size() const { return n_; }
    // `ring` is a circular history of size n whose oldest sample is at `start`.
    void compute(const cf32* ring, size_t start, float* outDb);

private:
    size_t n_;
    std::vector<float> window_;
    float norm_ = 1.0f;
    void* in_ = nullptr;
    void* out_ = nullptr;
    void* plan_ = nullptr;
};

class SpectrumAnalyzer {
public:
    explicit SpectrumAnalyzer(RingBuffer<cf32>& tap);
    ~SpectrumAnalyzer();

    void start();
    void stop();

    void setFftSize(int n) { fftSize_.store(n); }
    int fftSize() const { return fftSize_.load(); }
    void setRate(int linesPerSecond) { rate_.store(linesPerSecond); }
    int rate() const { return rate_.load(); }
    void setWindow(FftWindow w) { window_.store(int(w)); }
    FftWindow window() const { return FftWindow(window_.load()); }

    // UI thread: fetch the next pending line (oldest first). False if none.
    bool popLine(std::vector<float>& out);

private:
    void run();

    RingBuffer<cf32>& tap_;
    std::atomic<int> fftSize_{8192};
    std::atomic<int> rate_{30};
    std::atomic<int> window_{int(FftWindow::BlackmanHarris)};
    std::atomic<bool> running_{false};
    std::thread thread_;

    std::mutex outMutex_;
    std::deque<std::vector<float>> lines_;
};

}  // namespace osx
