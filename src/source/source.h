// Source module interface (Thread 1 owners).
//
// Every source delivers raw interleaved unsigned 8-bit I/Q (the native
// RTL-SDR wire format) into a lock-free ring buffer from its own I/O thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core/ring_buffer.h"

namespace osx {

class Source {
public:
    virtual ~Source() = default;

    virtual const char* name() const = 0;

    // Start/stop the I/O thread. `out` must outlive the running source.
    virtual bool start(RingBuffer<uint8_t>& out) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;

    virtual void setCenterFrequency(double hz) = 0;
    virtual double centerFrequency() const = 0;

    // Sample rate may only be changed while stopped.
    virtual void setSampleRate(double sps) = 0;
    virtual double sampleRate() const = 0;
    virtual std::vector<double> sampleRates() const = 0;

    // Bytes that could not be queued because the DSP thread fell behind.
    uint64_t overruns() const { return overruns_.load(std::memory_order_relaxed); }

protected:
    std::atomic<uint64_t> overruns_{0};
};

}  // namespace osx
