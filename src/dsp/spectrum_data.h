// GUI-independent spectrum model: smoothing, peak hold, pixel reduction and
// noise-floor / SNR measurements on top of the SpectrumAnalyzer output.
#pragma once

#include <functional>
#include <vector>

#include "dsp/spectrum_analyzer.h"

namespace osx {

class SpectrumData {
public:
    float smoothing = 0.5f;   // 0 = raw .. 0.98 = heavy averaging

    // Drains pending FFT lines; onLine is called with every raw line.
    void feed(SpectrumAnalyzer& analyzer, const std::function<void(const std::vector<float>&)>& onLine);

    const std::vector<float>& avg() const { return avg_; }
    const std::vector<float>& peak() const { return peak_; }
    bool hasData() const { return !avg_.empty(); }
    uint64_t lines() const { return lines_; }
    void resetPeak() { peak_ = avg_; }
    void clear() { avg_.clear(); peak_.clear(); }

    // Peak-detect (or interpolate) `src` onto widthPx columns covering
    // [loHz, loHz + spanHz) relative to the hardware centre.
    static void reduce(const std::vector<float>& src, std::vector<float>& out, int widthPx,
                       double loHz, double spanHz, double sampleRate);

    // Noise floor = 25th percentile bin; SNR = strongest passband bin above it.
    void measure(double vfoOffsetHz, double bandwidthHz, double sampleRate);
    float snrDb = 0.0f, noiseFloorDb = -100.0f, maxDb = -100.0f;

private:
    std::vector<float> line_, avg_, peak_, scratch_;
    uint64_t lines_ = 0, measuredAt_ = 0;
};

}  // namespace osx
