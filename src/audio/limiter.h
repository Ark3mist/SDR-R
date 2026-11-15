// Output stage: volume, mute and a hard-ceiling peak limiter.
//
// The Cardputer Zero's ES8389 DAC clips and drops out when driven to 0 dBFS,
// so nothing leaving this stage may exceed kCeiling (-6 dBFS), whatever the
// volume setting or demodulator level.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace osx {

struct OutputLimiter {
    static constexpr float kCeiling = 0.5011872f;  // -6.0 dBFS

    float gain = 1.0f;       // current gain reduction (<= 1)
    float release = 0.0f;    // per-sample release coefficient
    float peak = 0.0f;       // last block's output peak (for metering)

    explicit OutputLimiter(double sampleRate = 48000.0, double releaseSeconds = 0.15)
        : release(float(1.0 - std::exp(-1.0 / (releaseSeconds * sampleRate)))) {}

    // volume01: 0..1 (square-law taper). Processes interleaved stereo in place.
    void process(float* x, size_t frames, float volume01, bool mute) {
        const float vol = mute ? 0.0f : volume01 * volume01;
        float pk = 0.0f;
        for (size_t i = 0; i < frames; ++i) {
            float l = x[2 * i] * vol, r = x[2 * i + 1] * vol;
            const float a = std::max(std::fabs(l), std::fabs(r));
            const float need = a > kCeiling ? kCeiling / a : 1.0f;
            if (need < gain) gain = need;                 // instant attack
            else gain += release * (need - gain);         // smooth release
            l = std::clamp(l * gain, -kCeiling, kCeiling);
            r = std::clamp(r * gain, -kCeiling, kCeiling);
            x[2 * i] = l;
            x[2 * i + 1] = r;
            pk = std::max({pk, std::fabs(l), std::fabs(r)});
        }
        peak = pk;
    }
};

}  // namespace osx
