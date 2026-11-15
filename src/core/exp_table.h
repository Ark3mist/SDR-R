#pragma once

#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <numbers>

namespace osx {

// 65536-entry complex exponential table. Phase is a uint32 accumulator whose
// top 16 bits index the table (spurs ~ -96 dBc). One turn == 2^32.
inline const std::array<std::complex<float>, 65536>& expTable() {
    static const auto table = [] {
        auto t = std::make_unique<std::array<std::complex<float>, 65536>>();
        for (size_t i = 0; i < t->size(); ++i) {
            const double a = 2.0 * std::numbers::pi * double(i) / double(t->size());
            (*t)[i] = {float(std::cos(a)), float(std::sin(a))};
        }
        return t;
    }();
    return *table;
}

inline uint32_t phaseInc(double hz, double sampleRate) {
    return uint32_t(int64_t(std::llround(hz / sampleRate * 4294967296.0)));
}

}  // namespace osx
