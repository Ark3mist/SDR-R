#include "engine/radio.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace osx {

const char* Radio::sourceKindName(SourceKind k) {
    switch (k) {
        case SourceKind::Simulated: return "Simulated";
        case SourceKind::RtlSdr: return "RTL-SDR";
        case SourceKind::RtlTcp: return "rtl_tcp";
        case SourceKind::File: return "IQ file";
        default: return "?";
    }
}

Radio::Radio()
    : hwRing_(size_t(1) << 23),     // 8 MiB: ~1.3 s of 3.2 MSPS uint8 I/Q
      tapRing_(size_t(1) << 19),
      audioRing_(size_t(1) << 15),  // 16384 stereo frames (341 ms)
      fileRing_(size_t(1) << 20),   // 1 M complex samples (8 MiB)
      receiver(audioRing_),
      dsp(hwRing_, tapRing_, &receiver, &fileRing_),
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
        mixerReport = AudioSink::clampMixer(card, -6.0f);
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
        case SourceKind::File: return file;
        default: return sim;
    }
}

bool Radio::play() {
    if (playing_) return true;
    error_.clear();
    hwRing_.reset();
    fileRing_.reset();
    Source& src = source();
    if (src.tunable()) src.setCenterFrequency(centerHz_);
    else centerHz_ = src.centerFrequency();
    receiver.setOffset(vfo.offsetHz);
    dsp.start(src.sampleRate(), src.format() == SampleFormat::CF32 ? DspEngine::Input::CF32 : DspEngine::Input::U8);
    if (!src.start(SourceRings{&hwRing_, &fileRing_})) {
        dsp.stop();
        error_ = src.lastError().empty() ? std::string("source failed to start") : src.lastError();
        return false;
    }
    playing_ = true;
    return true;
}

bool Radio::playWithFallback() {
    if (play()) return true;
    if (kind_ != SourceKind::RtlSdr) return false;
    const std::string why = error_;
    kind_ = SourceKind::Simulated;
    sim.setSampleRate(rtl.sampleRate());
    fallback_ = true;
    const bool ok = play();
    error_ = why + " - running simulator";
    return ok;
}

void Radio::stop() {
    if (!playing_) return;
    source().stop();
    dsp.stop();
    playing_ = false;
}

void Radio::poll() {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (playing_ && !source().running()) {  // device unplugged / server closed / end of file
        const std::string why = source().lastError().empty() ? std::string("source stopped") : source().lastError();
        stop();
        if (kind_ == SourceKind::RtlSdr) {
            kind_ = SourceKind::Simulated;
            sim.setSampleRate(rtl.sampleRate());
            fallback_ = true;
            play();
            error_ = why + " - running simulator";
            redetectAt_ = now + 2.0;
        } else {
            error_ = why;
        }
    }
    // Re-detect: once the dongle is back, switch to it again.
    if (fallback_ && kind_ == SourceKind::Simulated && now >= redetectAt_) {
        redetectAt_ = now + 2.0;
        if (RtlSdrSource::deviceCount() > 0) {
            const bool was = playing_;
            stop();
            kind_ = SourceKind::RtlSdr;
            rtl.setSampleRate(sim.sampleRate());
            if (!was || play()) {
                fallback_ = false;
                error_ = "RTL-SDR detected";
            } else {
                kind_ = SourceKind::Simulated;  // still busy / no permission: keep simulating
                play();
            }
        }
    }
}

void Radio::selectSource(SourceKind k) {
    fallback_ = false;  // an explicit choice ends automatic fallback / re-detect
    if (k == kind_) return;
    const bool was = playing_;
    const double sr = source().sampleRate();
    stop();
    kind_ = k;
    if (source().tunable()) {
        source().setSampleRate(sr);
        source().setCenterFrequency(centerHz_);
    } else {
        centerHz_ = source().centerFrequency();
        vfo.offsetHz = 0.0;
        receiver.setOffset(0.0);
    }
    if (was) play();
}

bool Radio::openFile(const std::string& path) {
    const bool was = playing_;
    if (kind_ == SourceKind::File) stop();
    if (!file.open(path)) {
        error_ = file.lastError();
        if (was && kind_ == SourceKind::File) play();
        return false;
    }
    if (file.info().centerHz <= 0) file.setCenterFrequency(centerHz_);  // unknown: keep the current label
    if (kind_ != SourceKind::File) selectSource(SourceKind::File);
    else {
        centerHz_ = file.centerFrequency();
        vfo.offsetHz = 0.0;
        receiver.setOffset(0.0);
    }
    if (was) play();
    return true;
}

void Radio::setSampleRate(double sps) {
    if (sps == source().sampleRate()) return;
    const bool was = playing_;
    stop();
    source().setSampleRate(sps);
    tune(vfoHz());  // re-fit the VFO inside the new span
    if (was) play();
}

void Radio::setCenter(double hz) {
    hz = std::clamp(hz, 0.0, 9'999'999'999.0);
    if (hz == centerHz_) return;
    if (!source().tunable()) {  // recording: the centre is fixed; only relabel it while stopped
        if (!playing_) {
            centerHz_ = hz;
            source().setCenterFrequency(hz);
        }
        return;
    }
    centerHz_ = hz;
    source().setCenterFrequency(hz);
}

void Radio::tune(double absHz) {
    absHz = std::clamp(absHz, 0.0, 9'999'999'999.0);
    const double sr = source().sampleRate();
    double lo = 0.0, hi = 0.0;
    passband(lo, hi);
    // VFO offsets that keep the whole passband inside the captured band.
    const double minOff = -sr * 0.5 - lo, maxOff = sr * 0.5 - hi;
    if (!source().tunable()) {  // IQ recording: move the VFO within the file's band only
        vfo.offsetHz = std::clamp(absHz - centerHz_, std::min(minOff, 0.0), std::max(maxOff, 0.0));
    } else if (tuningMode == TuningMode::Center) {
        vfo.offsetHz = 0.0;
        setCenter(absHz);
    } else {
        const double off = absHz - centerHz_;
        if (off >= minOff && off <= maxOff) {
            vfo.offsetHz = off;
        } else {
            const double keep = std::clamp(vfo.offsetHz, std::min(minOff, 0.0), std::max(maxOff, 0.0));
            setCenter(absHz - keep);
            vfo.offsetHz = absHz - centerHz_;
        }
    }
    receiver.setOffset(vfo.offsetHz);
}

void Radio::recall(double absHz, Demod mode, double bandwidthHz) {
    if (mode != rx_.mode) setMode(mode);
    if (bandwidthHz > 0) setBandwidth(bandwidthHz);
    tune(absHz);
}

void Radio::setRx(const RxConfig& c) {
    rx_ = c;
    const DemodLimits& lim = demodLimits(rx_.mode);
    rx_.bandwidthHz = std::clamp(rx_.bandwidthHz, lim.minBw, lim.maxBw);
    vfo.bandwidthHz = rx_.bandwidthHz;
    vfo.minBandwidthHz = lim.minBw;
    vfo.maxBandwidthHz = lim.maxBw;
    receiver.setConfig(rx_);
    tune(vfoHz());  // keep the (possibly asymmetric) passband inside the captured band
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
