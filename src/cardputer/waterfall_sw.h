// Software waterfall for the 320-pixel display.
//
// History is kept as quantised dB (uint8, 0.63 dB steps) at 4x the display
// width together with each row's centre frequency and sample rate. Rendering
// maps absolute frequency -> history column per row, so retuning, zoom,
// contrast and colormap changes all re-draw the existing history correctly.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace osx {

class WaterfallSW {
public:
    static constexpr int kCols = 1280;
    static constexpr float kDbFloor = -150.0f, kDbRange = 160.0f;

    explicit WaterfallSW(int rows) : rows_(rows), data_(size_t(rows) * kCols, 0), meta_(size_t(rows)) {}

    void clear() {
        std::fill(data_.begin(), data_.end(), uint8_t(0));
        for (auto& m : meta_) m = {};
    }

    // line: fft-shifted dB, n bins spanning sampleRate around centerHz.
    void push(const float* line, size_t n, double centerHz, double sampleRate) {
        head_ = (head_ + rows_ - 1) % rows_;
        uint8_t* row = &data_[size_t(head_) * kCols];
        const double binsPerCol = double(n) / double(kCols);
        for (int c = 0; c < kCols; ++c) {
            const size_t i0 = std::min(n - 1, size_t(double(c) * binsPerCol));
            const size_t i1 = std::clamp(size_t(double(c + 1) * binsPerCol), i0 + 1, n);
            float m = line[i0];
            for (size_t i = i0 + 1; i < i1; ++i) m = std::max(m, line[i]);
            row[c] = uint8_t(std::clamp((m - kDbFloor) * (255.0f / kDbRange), 0.0f, 255.0f));
        }
        meta_[size_t(head_)] = {centerHz, sampleRate, true};
    }

    // Paints w x min(h, rows) ARGB pixels (newest row on top). The view shows
    // absolute frequencies [loHz, loHz + spanHz). lut = 256-entry colormap.
    void render(uint32_t* dst, int pitchPx, int w, int h, double loHz, double spanHz,
                float minDb, float maxDb, const uint32_t* lut) const {
        uint32_t color[256];
        const float range = std::max(maxDb - minDb, 1.0f);
        for (int q = 0; q < 256; ++q) {
            const float db = kDbFloor + float(q) * (kDbRange / 255.0f);
            color[q] = lut[int(std::clamp((db - minDb) / range, 0.0f, 1.0f) * 255.0f)];
        }
        for (int y = 0; y < h; ++y) {
            uint32_t* out = dst + size_t(y) * size_t(pitchPx);
            const int r = (head_ + y) % rows_;
            const Meta& m = meta_[size_t(r)];
            if (y >= rows_ || !m.valid) {
                std::fill(out, out + w, lut[0]);
                continue;
            }
            const uint8_t* row = &data_[size_t(r) * kCols];
            // column(x) = a + b * x
            const double b = spanHz / double(w) / m.sampleRate * kCols;
            const double a = ((loHz - m.centerHz) / m.sampleRate + 0.5) * kCols;
            for (int x = 0; x < w; ++x) {
                const double c0 = a + b * double(x), c1 = c0 + b;
                if (c1 <= 0.0 || c0 >= double(kCols)) {
                    out[x] = lut[0];  // outside what was captured at the time
                    continue;
                }
                const int i0 = std::clamp(int(c0), 0, kCols - 1);
                const int i1 = std::clamp(int(c1), i0 + 1, kCols);
                uint8_t q = row[i0];
                for (int i = i0 + 1; i < i1; ++i) q = std::max(q, row[i]);
                out[x] = color[q];
            }
        }
    }

private:
    struct Meta { double centerHz = 0.0, sampleRate = 1.0; bool valid = false; };
    int rows_, head_ = 0;
    std::vector<uint8_t> data_;
    std::vector<Meta> meta_;
};

}  // namespace osx
