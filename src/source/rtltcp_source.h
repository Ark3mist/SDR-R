// rtl_tcp network client (same uint8 I/Q wire format as the native driver).
#pragma once

#include <thread>

#include "source/source.h"

namespace osx {

class RtlTcpSource final : public Source {
public:
    RtlTcpSource();
    ~RtlTcpSource() override;

    const char* name() const override { return "rtl_tcp"; }
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
    void setGain(float db) override;
    std::string lastError() const override { return error_; }

    std::string host = "127.0.0.1";
    int port = 1234;
    int bufferKb = 16;          // socket read chunk, 4..256 kB

    void setDirectSampling(int mode);
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
    void command(uint8_t cmd, uint32_t param);
    void run(RingBuffer<uint8_t>* out);

    std::atomic<int> fd_{-1};
    std::thread thread_;
    std::atomic<bool> running_{false};
    double sampleRate_ = 1.024e6, centerHz_ = 100e6;
    std::vector<float> gains_;
    float gainDb_ = 32.8f;
    int directSampling_ = 0, ppm_ = 0;
    bool offsetTuning_ = false, rtlAgc_ = false, tunerAgc_ = false, biasTee_ = false;
    std::string error_;
};

}  // namespace osx
