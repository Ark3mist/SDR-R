// Source module interface (Thread 1 owners).
//
// Hardware sources (RTL-SDR, rtl_tcp, simulator) deliver raw interleaved
// unsigned 8-bit I/Q, the native RTL-SDR wire format. The file player
// delivers complex float so 16-bit / float recordings keep their dynamic
// range. Each source writes from its own I/O thread into a lock-free ring.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core/ring_buffer.h"
#include "core/types.h"

namespace osx {

enum class SampleFormat { U8, CF32 };

struct SourceRings {
    RingBuffer<uint8_t>* u8 = nullptr;
    RingBuffer<cf32>* f32 = nullptr;
};

class Source {
public:
    virtual ~Source() = default;

    virtual const char* name() const = 0;
    virtual SampleFormat format() const { return SampleFormat::U8; }
    // False when the centre frequency is fixed (IQ recording).
    virtual bool tunable() const { return true; }

    // Start/stop the I/O thread. The rings must outlive the running source.
    virtual bool start(const SourceRings& out) = 0;
    bool start(RingBuffer<uint8_t>& out) { return start(SourceRings{&out, nullptr}); }
    virtual void stop() = 0;
    virtual bool running() const = 0;

    virtual void setCenterFrequency(double hz) = 0;
    virtual double centerFrequency() const = 0;

    // Sample rate may only be changed while stopped.
    virtual void setSampleRate(double sps) = 0;
    virtual double sampleRate() const = 0;
    virtual std::vector<double> sampleRates() const = 0;

    // RF gain, generic across sources. gainSteps() lists the valid values (dB).
    virtual std::vector<float> gainSteps() const { return {}; }
    virtual float gain() const { return 0.0f; }
    virtual void setGain(float /*db*/) {}

    // Human-readable reason for the last start() failure / stream loss.
    virtual std::string lastError() const { return {}; }

    // Bytes that could not be queued because the DSP thread fell behind.
    uint64_t overruns() const { return overruns_.load(std::memory_order_relaxed); }

protected:
    std::atomic<uint64_t> overruns_{0};
};

}  // namespace osx
