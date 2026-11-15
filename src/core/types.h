#pragma once

#include <complex>
#include <mutex>

namespace osx {

using cf32 = std::complex<float>;

// FFTW's planner is not thread-safe; every fftwf_plan_* / fftwf_destroy_plan
// call in the process must hold this mutex. fftwf_execute is thread-safe.
inline std::mutex& fftwPlannerMutex() {
    static std::mutex m;
    return m;
}

}  // namespace osx
