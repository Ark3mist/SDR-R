// IQ baseband file player: RIFF/WAVE, 2 channels (I, Q), 8-bit unsigned,
// 16-bit signed or 32-bit float samples (WAVE_FORMAT_PCM / IEEE_FLOAT /
// EXTENSIBLE). Paced in real time on its own thread, with loop and seek.
// The centre frequency is read from the file name ("..._100000000Hz_...",
// the SDR++ / SDR# convention) when present.
#pragma once

#include <cstdio>
#include <thread>

#include "source/source.h"

namespace osx {

struct WavInfo {
    int encoding = 0;          // 8 = u8, 16 = s16, 32 = f32
    double sampleRate = 0.0;
    uint64_t dataOffset = 0, frames = 0;
    double centerHz = 0.0;     // 0 if unknown
};

// Parses the header; returns false with `error` set if the file is unusable.
bool parseIqWav(const std::string& path, WavInfo& info, std::string& error);
double centerFromFilename(const std::string& path);

class FileSource final : public Source {
public:
    ~FileSource() override;

    const char* name() const override { return "IQ file"; }
    SampleFormat format() const override { return SampleFormat::CF32; }
    bool tunable() const override { return false; }

    bool open(const std::string& path);   // only while stopped
    const std::string& path() const { return path_; }
    const WavInfo& info() const { return info_; }
    bool isOpen() const { return file_ != nullptr; }

    using Source::start;
    bool start(const SourceRings& out) override;
    void stop() override;
    bool running() const override { return running_.load(); }

    void setCenterFrequency(double hz) override { centerHz_ = hz; }  // label only
    double centerFrequency() const override { return centerHz_; }
    void setSampleRate(double) override {}
    double sampleRate() const override { return info_.sampleRate > 0 ? info_.sampleRate : 1.024e6; }
    std::vector<double> sampleRates() const override { return {sampleRate()}; }
    std::string lastError() const override { return error_; }

    std::atomic<bool> loop{true};
    double position() const;              // 0..1
    double durationSeconds() const { return info_.sampleRate > 0 ? double(info_.frames) / info_.sampleRate : 0.0; }
    void seek(double fraction);           // any thread

    // Reads up to n frames at the current position (honours loop). Used by
    // the player thread; public for the tests. Returns frames read.
    size_t read(cf32* dst, size_t n);

private:
    void run(RingBuffer<cf32>* out);

    std::FILE* file_ = nullptr;
    std::string path_, error_;
    WavInfo info_;
    double centerHz_ = 100e6;
    std::atomic<uint64_t> pos_{0};
    std::atomic<int64_t> seekTo_{-1};
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::vector<uint8_t> raw_;
};

}  // namespace osx
