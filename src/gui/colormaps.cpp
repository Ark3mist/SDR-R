#include "gui/colormaps.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace osx {

namespace {

struct Rgb { float r, g, b; };

Rgb hex(uint32_t v) {
    return {float((v >> 16) & 0xFF) / 255.0f, float((v >> 8) & 0xFF) / 255.0f, float(v & 0xFF) / 255.0f};
}

// Piecewise-linear interpolation through evenly spaced colour stops.
Rgb lerpStops(const std::vector<uint32_t>& stops, float t) {
    const float pos = std::clamp(t, 0.0f, 1.0f) * float(stops.size() - 1);
    const size_t i = std::min(size_t(pos), stops.size() - 2);
    const float f = pos - float(i);
    const Rgb a = hex(stops[i]), b = hex(stops[i + 1]);
    return {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f};
}

// Google "Turbo" polynomial approximation (A. Mikhailov, Apache-2.0).
Rgb turbo(float x) {
    const float r = 0.13572138f + x * (4.61539260f + x * (-42.66032258f + x * (132.13108234f + x * (-152.94239396f + x * 59.28637943f))));
    const float g = 0.09140261f + x * (2.19418839f + x * (4.84296658f + x * (-14.18503333f + x * (4.27729857f + x * 2.82956604f))));
    const float b = 0.10667330f + x * (12.64194608f + x * (-60.58204836f + x * (110.36276771f + x * (-89.90310912f + x * 27.34824973f))));
    return {r, g, b};
}

}  // namespace

const char* colormapName(ColormapId id) {
    switch (id) {
        case ColormapId::ClassicSdrpp: return "Classic (SDR++)";
        case ColormapId::Turbo: return "Turbo";
        case ColormapId::Viridis: return "Viridis";
        case ColormapId::Inferno: return "Inferno";
        case ColormapId::Grayscale: return "Grayscale";
        default: return "?";
    }
}

void buildColormap(ColormapId id, uint8_t* rgb) {
    static const std::vector<uint32_t> classic = {
        0x000020, 0x000030, 0x000050, 0x000091, 0x1E90FF, 0xFFFFFF, 0xFFFF00,
        0xFE6D16, 0xFF0000, 0xC60000, 0x9F0000, 0x750000, 0x4A0000};
    static const std::vector<uint32_t> viridis = {
        0x440154, 0x472D7B, 0x3B528B, 0x2C728E, 0x21918C, 0x28AE80, 0x5EC962, 0xADDC30, 0xFDE725};
    static const std::vector<uint32_t> inferno = {
        0x000004, 0x1B0C41, 0x4A0C6B, 0x781C6D, 0xA52C60, 0xCF4446, 0xED6925, 0xFB9B06, 0xF7D13D, 0xFCFFA4};

    for (int i = 0; i < 256; ++i) {
        const float t = float(i) / 255.0f;
        Rgb c{t, t, t};
        switch (id) {
            case ColormapId::ClassicSdrpp: c = lerpStops(classic, t); break;
            case ColormapId::Turbo: c = turbo(t); break;
            case ColormapId::Viridis: c = lerpStops(viridis, t); break;
            case ColormapId::Inferno: c = lerpStops(inferno, t); break;
            default: break;
        }
        auto q = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
        rgb[3 * i] = q(c.r);
        rgb[3 * i + 1] = q(c.g);
        rgb[3 * i + 2] = q(c.b);
    }
}

}  // namespace osx
