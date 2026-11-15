#include "dsp/spectrum_data.h"

#include <algorithm>
#include <cmath>

namespace osx {

void SpectrumData::feed(SpectrumAnalyzer& analyzer, const std::function<void(const std::vector<float>&)>& onLine) {
    while (analyzer.popLine(line_)) {
        const size_t n = line_.size();
        if (avg_.size() != n) {
            avg_ = line_;
            peak_ = line_;
        } else {
            const float a = std::clamp(smoothing, 0.0f, 0.98f), b = 1.0f - a;
            for (size_t i = 0; i < n; ++i) {
                avg_[i] = a * avg_[i] + b * line_[i];
                peak_[i] = std::max(peak_[i], avg_[i]);
            }
        }
        if (onLine) onLine(line_);
        ++lines_;
    }
}

void SpectrumData::reduce(const std::vector<float>& src, std::vector<float>& out, int widthPx,
                          double loHz, double spanHz, double sampleRate) {
    out.resize(size_t(widthPx));
    if (src.empty()) return;
    const double n = double(src.size());
    const double binsPerPx = spanHz / double(widthPx) / sampleRate * n;
    const double b0 = (loHz / sampleRate + 0.5) * n;
    const int last = int(src.size()) - 1;
    if (binsPerPx >= 1.0) {
        for (int x = 0; x < widthPx; ++x) {
            const int i0 = std::clamp(int(std::floor(b0 + double(x) * binsPerPx + 0.5)), 0, last);
            const int i1 = std::clamp(int(std::floor(b0 + double(x + 1) * binsPerPx + 0.5)), i0 + 1, last + 1);
            float m = src[size_t(i0)];
            for (int i = i0 + 1; i < i1; ++i) m = std::max(m, src[size_t(i)]);
            out[size_t(x)] = m;
        }
    } else {
        for (int x = 0; x < widthPx; ++x) {
            const double pos = std::clamp(b0 + (double(x) + 0.5) * binsPerPx, 0.0, double(last));
            const int i = std::min(int(pos), std::max(last - 1, 0));
            const float f = float(pos - double(i));
            out[size_t(x)] = src[size_t(i)] * (1.0f - f) + src[size_t(std::min(i + 1, last))] * f;
        }
    }
}

void SpectrumData::measure(double vfoOffsetHz, double bandwidthHz, double sampleRate) {
    if (avg_.empty() || measuredAt_ == lines_) return;
    measuredAt_ = lines_;
    const size_t n = avg_.size();
    const size_t stride = std::max<size_t>(1, n / 2048);
    scratch_.clear();
    float mx = -200.0f;
    for (size_t i = 0; i < n; i += stride) {
        scratch_.push_back(avg_[i]);
        mx = std::max(mx, avg_[i]);
    }
    auto nth = scratch_.begin() + std::ptrdiff_t(scratch_.size() / 4);
    std::nth_element(scratch_.begin(), nth, scratch_.end());
    noiseFloorDb = *nth;
    maxDb = mx;

    const double lo = (vfoOffsetHz - bandwidthHz * 0.5) / sampleRate + 0.5;
    const double hi = (vfoOffsetHz + bandwidthHz * 0.5) / sampleRate + 0.5;
    const size_t i0 = size_t(std::clamp(lo * double(n), 0.0, double(n - 1)));
    const size_t i1 = size_t(std::clamp(hi * double(n), double(i0 + 1), double(n)));
    float pk = -200.0f;
    for (size_t i = i0; i < i1; ++i) pk = std::max(pk, avg_[i]);
    snrDb += 0.3f * (std::max(0.0f, pk - noiseFloorDb) - snrDb);
}

}  // namespace osx
