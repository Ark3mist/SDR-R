#include "source/rtlsdr_source.h"

#include <algorithm>
#include <cmath>

#include <rtl-sdr.h>

namespace osx {

namespace {
rtlsdr_dev_t* D(void* p) { return static_cast<rtlsdr_dev_t*>(p); }
}  // namespace

RtlSdrSource::RtlSdrSource()
    // R820T/R820T2/R828D gain table; replaced by the tuner's own list on open.
    : gains_{0.0f, 0.9f, 1.4f, 2.7f, 3.7f, 7.7f, 8.7f, 12.5f, 14.4f, 15.7f, 16.6f, 19.7f, 20.7f, 22.9f, 25.4f,
             28.0f, 29.7f, 32.8f, 33.8f, 36.4f, 37.2f, 38.6f, 40.2f, 42.1f, 43.4f, 43.9f, 44.5f, 48.0f, 49.6f} {}

RtlSdrSource::~RtlSdrSource() { stop(); }

std::vector<std::string> RtlSdrSource::listDevices() {
    std::vector<std::string> out;
    const uint32_t n = rtlsdr_get_device_count();
    for (uint32_t i = 0; i < n; ++i) {
        char manufact[256] = {0}, product[256] = {0}, serial[256] = {0};
        rtlsdr_get_device_usb_strings(i, manufact, product, serial);
        out.push_back(std::string(rtlsdr_get_device_name(i)) + " [" + serial + "]");
    }
    return out;
}

std::vector<double> RtlSdrSource::sampleRates() const {
    return {250e3, 1.024e6, 1.536e6, 1.792e6, 1.92e6, 2.048e6, 2.16e6, 2.4e6, 2.56e6, 2.88e6, 3.2e6};
}

void RtlSdrSource::callback(unsigned char* buf, uint32_t len, void* ctx) {
    auto* self = static_cast<RtlSdrSource*>(ctx);
    const size_t w = self->out_->write(buf, len);
    if (w < len) self->overruns_.fetch_add(len - w, std::memory_order_relaxed);
}

bool RtlSdrSource::start(RingBuffer<uint8_t>& out) {
    if (running_.load()) return true;
    std::lock_guard<std::mutex> lk(devMutex_);
    error_.clear();
    rtlsdr_dev_t* dev = nullptr;
    if (rtlsdr_get_device_count() == 0) {
        error_ = "no RTL-SDR device found";
        return false;
    }
    if (rtlsdr_open(&dev, uint32_t(std::max(deviceIndex, 0))) < 0 || !dev) {
        error_ = "cannot open RTL-SDR #" + std::to_string(deviceIndex) + " (busy or no permission)";
        return false;
    }
    dev_ = dev;
    out_ = &out;

    int n = rtlsdr_get_tuner_gains(dev, nullptr);
    if (n > 0) {
        std::vector<int> tenths(size_t(n), 0);
        rtlsdr_get_tuner_gains(dev, tenths.data());
        gains_.clear();
        for (int g : tenths) gains_.push_back(float(g) / 10.0f);
        std::sort(gains_.begin(), gains_.end());
    }
    rtlsdr_set_sample_rate(dev, uint32_t(sampleRate_));
    rtlsdr_set_direct_sampling(dev, directSampling_);
    if (directSampling_ == 0) rtlsdr_set_offset_tuning(dev, offsetTuning_ ? 1 : 0);
    rtlsdr_set_freq_correction(dev, ppm_);
    rtlsdr_set_center_freq(dev, uint32_t(centerHz_));
    rtlsdr_set_agc_mode(dev, rtlAgc_ ? 1 : 0);
    applyGain();
    rtlsdr_set_bias_tee(dev, biasTee_ ? 1 : 0);
    rtlsdr_reset_buffer(dev);

    running_.store(true);
    thread_ = std::thread([this, dev] {
        // Blocks until rtlsdr_cancel_async() or the device disappears.
        rtlsdr_read_async(dev, &RtlSdrSource::callback, this, 0, 16384);
        if (running_.exchange(false)) error_ = "RTL-SDR stream stopped (device unplugged?)";
    });
    return true;
}

void RtlSdrSource::stop() {
    {
        std::lock_guard<std::mutex> lk(devMutex_);
        if (!dev_) return;
        running_.store(false);
        rtlsdr_cancel_async(D(dev_));
    }
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lk(devMutex_);
    rtlsdr_set_bias_tee(D(dev_), 0);  // never leave DC on the antenna port
    rtlsdr_close(D(dev_));
    dev_ = nullptr;
}

void RtlSdrSource::setCenterFrequency(double hz) {
    centerHz_ = hz;
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_) rtlsdr_set_center_freq(D(dev_), uint32_t(std::clamp(hz, 0.0, 4294967295.0)));
}

void RtlSdrSource::applyGain() {
    if (!dev_) return;
    rtlsdr_set_tuner_gain_mode(D(dev_), tunerAgc_ ? 0 : 1);
    if (!tunerAgc_) rtlsdr_set_tuner_gain(D(dev_), int(std::lround(gainDb_ * 10.0f)));
}

void RtlSdrSource::setGain(float db) {
    if (!gains_.empty()) {  // snap to the nearest valid tuner step
        float best = gains_.front();
        for (float g : gains_)
            if (std::fabs(g - db) < std::fabs(best - db)) best = g;
        db = best;
    }
    gainDb_ = db;
    std::lock_guard<std::mutex> lk(devMutex_);
    applyGain();
}

void RtlSdrSource::setDirectSampling(int mode) {
    directSampling_ = std::clamp(mode, 0, 2);
    std::lock_guard<std::mutex> lk(devMutex_);
    if (!dev_) return;
    rtlsdr_set_direct_sampling(D(dev_), directSampling_);
    rtlsdr_set_center_freq(D(dev_), uint32_t(centerHz_));
}

void RtlSdrSource::setOffsetTuning(bool on) {
    offsetTuning_ = on;
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ && directSampling_ == 0) rtlsdr_set_offset_tuning(D(dev_), on ? 1 : 0);
}

void RtlSdrSource::setRtlAgc(bool on) {
    rtlAgc_ = on;
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_) rtlsdr_set_agc_mode(D(dev_), on ? 1 : 0);
}

void RtlSdrSource::setTunerAgc(bool on) {
    tunerAgc_ = on;
    std::lock_guard<std::mutex> lk(devMutex_);
    applyGain();
}

void RtlSdrSource::setPpm(int ppm) {
    ppm_ = std::clamp(ppm, -200, 200);
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_) rtlsdr_set_freq_correction(D(dev_), ppm_);
}

void RtlSdrSource::setBiasTee(bool on) {
    biasTee_ = on;
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_) rtlsdr_set_bias_tee(D(dev_), on ? 1 : 0);
}

}  // namespace osx
