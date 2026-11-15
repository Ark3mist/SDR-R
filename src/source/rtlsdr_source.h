// Native RTL-SDR driver (librtlsdr, async callback -> lock-free ring).
#pragma once

#include <mutex>
#include <thread>

#include "source/source.h"

namespace osx {

class RtlSdrSource final : public Source {
public:
    RtlSdrSource();
    ~RtlSdrSource() override;

    static std::vector<std::string> listDevices();
    static int deviceCount();
    // USB transfer size for a sample rate: 262144 bytes (64 ms) at 2.048 MSPS,
    // scaled down at lower rates so a block never exceeds ~64 ms of I/Q.
    static uint32_t usbBlockBytes(double sampleRate);

    const char* name() const override { return "RTL-SDR"; }
    using Source::start;
    bool start(const SourceRings& out) override;
    void stop() override;
    bool running() const override { return running_.load(); }

    void setCenterFrequency(double hz) override;
    double centerFrequency() const override { return centerHz_; }
    void setSampleRate(double sps) override { if (!running()) sampleRate_ = sps; }
    double sampleRate() const override { return sampleRate_; }
    std::vector<double> sampleRates() const override;

    std::vector<float> gainSteps() const override { return gains_; }
    float gain() const override { return gainDb_; }
    void setGain(float db) override;      // snapped to the tuner's tenth-dB steps
    std::string lastError() const override { return error_; }

    // Settings: usable stopped (applied on start) or live.
    int deviceIndex = 0;
    void setDirectSampling(int mode);     // 0 off, 1 I-ADC, 2 Q-ADC
    void setOffsetTuning(bool on);
    void setRtlAgc(bool on);
    void setTunerAgc(bool on);
    void setPpm(int ppm);
    void setBiasTee(bool on);
    int directSampling() const { return directSampling_; }
    bool offsetTuning() const { return offsetTuning_; }
    bool rtlAgc() const { return rtlAgc_; }
    bool tunerAgc() const { return tunerAgc_; }
    int ppm() const { return ppm_; }
    bool biasTee() const { return biasTee_; }

private:
    static void callback(unsigned char* buf, uint32_t len, void* ctx);
    void applyGain();

    void* dev_ = nullptr;
    std::mutex devMutex_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    RingBuffer<uint8_t>* out_ = nullptr;

    double sampleRate_ = 1.024e6, centerHz_ = 100e6;
    std::vector<float> gains_;
    float gainDb_ = 32.8f;
    int directSampling_ = 0, ppm_ = 0;
    bool offsetTuning_ = false, rtlAgc_ = false, tunerAgc_ = false, biasTee_ = false;
    std::string error_;
};

}  // namespace osx
