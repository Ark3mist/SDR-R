#include "dsp/dsp_engine.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "dsp/receiver.h"

namespace osx {

namespace {
const std::array<float, 256>& u8Lut() {
    static const auto lut = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) t[size_t(i)] = (float(i) - 127.5f) / 127.5f;
        return t;
    }();
    return lut;
}
}  // namespace

void convertU8ToCf32(const uint8_t* src, cf32* dst, size_t nSamples, bool swapIq) {
    const auto& lut = u8Lut();
    if (!swapIq) {
        for (size_t i = 0; i < nSamples; ++i) dst[i] = {lut[src[2 * i]], lut[src[2 * i + 1]]};
    } else {
        for (size_t i = 0; i < nSamples; ++i) dst[i] = {lut[src[2 * i + 1]], lut[src[2 * i]]};
    }
}

DspEngine::DspEngine(RingBuffer<uint8_t>& in, RingBuffer<cf32>& spectrumTap, Receiver* rx, RingBuffer<cf32>* inF32)
    : in_(in), inF32_(inF32), tap_(spectrumTap), rx_(rx) {
    u8Lut();
}

DspEngine::~DspEngine() { stop(); }

void DspEngine::start(double sampleRate, Input input) {
    if (running_.exchange(true)) return;
    sampleRate_ = sampleRate;
    input_ = (input == Input::CF32 && inF32_) ? Input::CF32 : Input::U8;
    if (rx_) rx_->setInputRate(sampleRate);
    thread_ = std::thread(&DspEngine::run, this);
}

void DspEngine::stop() {
    if (!running_.exchange(false)) return;
    in_.wake();
    if (inF32_) inF32_->wake();
    if (thread_.joinable()) thread_.join();
}

void DspEngine::run() {
    constexpr size_t kMaxBlock = 32768;  // complex samples per iteration
    std::vector<uint8_t> raw(kMaxBlock * 2);
    std::vector<cf32> iq(kMaxBlock);
    DcBlocker dcBlock;
    IqBalance iqBal;
    float peak = 0.0f;
    const bool f32 = (input_ == Input::CF32);

    while (running_.load(std::memory_order_relaxed)) {
        size_t n = 0;
        if (f32) {
            inF32_->waitReadable(1);
            if (!running_.load(std::memory_order_relaxed)) break;
            n = inF32_->read(iq.data(), kMaxBlock);
            if (n == 0) continue;
            if (invertIq.load(std::memory_order_relaxed))
                for (size_t i = 0; i < n; ++i) iq[i] = {iq[i].imag(), iq[i].real()};
        } else {
            size_t avail = in_.waitReadable(2);
            if (!running_.load(std::memory_order_relaxed)) break;
            avail &= ~size_t(1);  // whole I/Q pairs only
            if (avail == 0) continue;
            n = in_.read(raw.data(), std::min(avail, raw.size())) / 2;
            convertU8ToCf32(raw.data(), iq.data(), n, invertIq.load(std::memory_order_relaxed));
        }

        // ADC peak before any correction (what the tuner actually delivered).
        float blockPeak = 0.0f;
        for (size_t i = 0; i < n; i += 16)
            blockPeak = std::max({blockPeak, std::fabs(iq[i].real()), std::fabs(iq[i].imag())});
        peak = std::max(blockPeak, peak * 0.98f);
        peak_.store(peak, std::memory_order_relaxed);

        if (dcRemoval.load(std::memory_order_relaxed)) dcBlock.process(iq.data(), n);
        else dcBlock.reset();
        if (iqCorrection.load(std::memory_order_relaxed)) {
            iqBal.process(iq.data(), n, sampleRate_);
            iqPhaseErrDeg.store(iqBal.phaseErrorDeg(), std::memory_order_relaxed);
            iqGainErrDb.store(iqBal.gainErrorDb(), std::memory_order_relaxed);
        } else {
            iqBal.reset();
        }

        // Spectrum tap is best-effort: if the display falls behind, drop.
        tap_.write(iq.data(), n);
        if (rx_) rx_->process(iq.data(), n);
        samples_.fetch_add(n, std::memory_order_relaxed);
    }
}

}  // namespace osx
