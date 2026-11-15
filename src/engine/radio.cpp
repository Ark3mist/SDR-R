#include "engine/radio.h"

#include <algorithm>
#include <cmath>

namespace osx {

const char* Radio::sourceKindName(SourceKind k) {
    switch (k) {
        case SourceKind::Simulated: return "Simulated";
        case SourceKind::RtlSdr: return "RTL-SDR";
        case SourceKind::RtlTcp: return "rtl_tcp";
        default: return "?";
    }
}

Radio::Radio()
    : hwRing_(size_t(1) << 23),     // 8 MiB: ~1.3 s of 3.2 MSPS uint8 I/Q
      tapRing_(size_t(1) << 19),
      audioRing_(size_t(1) << 15),  // 16384 stereo frames (341 ms)
      receiver(audioRing_),
      dsp(hwRing_, tapRing_, &receiver),
      analyzer(tapRing_),
      audio(audioRing_) {
    // CM0-safe boot defaults; everything stays adjustable from the menu.
    for (Source* s : {static_cast<Source*>(&sim), static_cast<Source*>(&rtl), static_cast<Source*>(&tcp)}) {
        s->setSampleRate(1.024e6);
        s->setCenterFrequency(centerHz_);
    }
    analyzer.setFftSize(1024);
    analyzer.setRate(30);
    vfo.color = 0xFFFFFFFF;
    setMode(Demod::WFM);
}

Radio::~Radio() { shutdown(); }

void Radio::init(const std::string& audioDevice, bool clampMixer) {
    if (clampMixer) {
        // "plughw:0,0" -> control device "hw:0"
        std::string card = "hw:0";
        const size_t colon = audioDevice.find(':');
        if (colon != std::string::npos) card = "hw:" + audioDevice.substr(colon + 1, audioDevice.find(',') - colon - 1);
        mixerReport = AlsaSink::clampMixer(card, -6.0f);
    }
    analyzer.start();
    audio.start(audioDevice);
}

void Radio::shutdown() {
    stop();
    audio.stop();
    analyzer.stop();
}

Source& Radio::source() {
    switch (kind_) {
        case SourceKind::RtlSdr: return rtl;
        case SourceKind::RtlTcp: return tcp;
        default: return sim;
    }
}

bool Radio::play() {
    if (playing_) return true;
    error_.clear();
    hwRing_.reset();
    source().setCenterFrequency(centerHz_);
    receiver.setOffset(vfo.offsetHz);
    dsp.start(source().sampleRate());
    if (!source().start(hwRing_)) {
        dsp.stop();
        error_ = source().lastError().empty() ? std::string("source failed to start") : source().lastError();
        return false;
    }
    playing_ = true;
    return true;
}

void Radio::stop() {
    if (!playing_) return;
    source().stop();
    dsp.stop();
    playing_ = false;
}

void Radio::poll() {
    if (playing_ && !source().running()) {  // device unplugged / server closed
        error_ = source().lastError().empty() ? std::string("source stopped") : source().lastError();
        dsp.stop();
        playing_ = false;
    }
}

void Radio::selectSource(SourceKind k) {
    if (k == kind_) return;
    const bool was = playing_;
    const double sr = source().sampleRate();
    stop();
    kind_ = k;
    source().setSampleRate(sr);
    source().setCenterFrequency(centerHz_);
    if (was) play();
}

void Radio::setSampleRate(double sps) {
    if (sps == source().sampleRate()) return;
    const bool was = playing_;
    stop();
    source().setSampleRate(sps);
    const double lim = std::max(0.0, sps * 0.5 - vfo.bandwidthHz * 0.5);
    vfo.offsetHz = std::clamp(vfo.offsetHz, -lim, lim);
    receiver.setOffset(vfo.offsetHz);
    if (was) play();
}

void Radio::setCenter(double hz) {
    hz = std::clamp(hz, 0.0, 9'999'999'999.0);
    if (hz == centerHz_) return;
    centerHz_ = hz;
    source().setCenterFrequency(hz);
}

void Radio::tune(double absHz) {
    absHz = std::clamp(absHz, 0.0, 9'999'999'999.0);
    const double sr = source().sampleRate();
    if (tuningMode == TuningMode::Center) {
        vfo.offsetHz = 0.0;
        setCenter(absHz);
    } else {
        const double lim = std::max(0.0, sr * 0.5 - vfo.bandwidthHz * 0.5);
        const double off = absHz - centerHz_;
        if (std::fabs(off) <= lim) {
            vfo.offsetHz = off;
        } else {
            const double keep = std::clamp(vfo.offsetHz, -lim, lim);
            setCenter(absHz - keep);
            vfo.offsetHz = absHz - centerHz_;
        }
    }
    receiver.setOffset(vfo.offsetHz);
}

void Radio::setRx(const RxConfig& c) {
    rx_ = c;
    const DemodLimits& lim = demodLimits(rx_.mode);
    rx_.bandwidthHz = std::clamp(rx_.bandwidthHz, lim.minBw, lim.maxBw);
    vfo.bandwidthHz = rx_.bandwidthHz;
    vfo.minBandwidthHz = lim.minBw;
    vfo.maxBandwidthHz = lim.maxBw;
    receiver.setConfig(rx_);
}

void Radio::setMode(Demod m) {
    RxConfig c = rx_;
    c.mode = m;
    const DemodLimits& lim = demodLimits(m);
    c.bandwidthHz = lim.defaultBw;
    c.deemphasis = (m == Demod::WFM) ? 1 : 0;
    vfo.snapHz = lim.defaultSnap;
    setRx(c);
}

void Radio::setBandwidth(double hz) {
    RxConfig c = rx_;
    c.bandwidthHz = hz;
    setRx(c);
}

}  // namespace osx
