#pragma once

#include <cstdint>
#include <string>

namespace osx {

// A virtual receiver inside the instantaneous hardware bandwidth.
struct Vfo {
    std::string name = "VFO A";
    double offsetHz = 0.0;         // relative to the hardware centre frequency
    double bandwidthHz = 200e3;
    double minBandwidthHz = 500.0;
    double maxBandwidthHz = 250e3;
    double snapHz = 100e3;         // tuning step
    uint32_t color = 0xFFFFFFFF;   // ABGR (ImU32 layout)
};

}  // namespace osx
