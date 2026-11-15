// Radio: the complete backend behind any front end.
// Owns the sources, the lock-free rings, the DSP thread (with its receiver),
// the display-FFT worker and the audio sink, and exposes thread-safe controls.
#pragma once

#include <string>

#include "audio/alsa_sink.h"
#include "core/ring_buffer.h"
#include "core/types.h"
#include "core/vfo.h"
#include "dsp/dsp_engine.h"
#include "dsp/receiver.h"
#include "dsp/spectrum_analyzer.h"
#include "source/rtlsdr_source.h"
#include "source/rtltcp_source.h"
#include "source/sim_source.h"

namespace osx {

class Radio {
    // Declared first: every thread-owning member below holds a reference to these.
    RingBuffer<uint8_t> hwRing_;   // raw interleaved uint8 I/Q   (I/O -> DSP)
    RingBuffer<cf32> tapRing_;     // corrected baseband          (DSP -> FFT worker)
    RingBuffer<float> audioRing_;  // interleaved stereo float32  (DSP -> audio sink)

public:
    enum class SourceKind { Simulated = 0, RtlSdr, RtlTcp, Count };
    enum class TuningMode { Normal = 0, Center };
    static const char* sourceKindName(SourceKind k);

    Radio();
    ~Radio();

    // Brings up the FFT worker and the audio sink (call once at boot).
    void init(const std::string& audioDevice, bool clampMixer);
    void shutdown();

    // ---- transport -------------------------------------------------------------
    bool play();                 // false: see lastError()
    void stop();
    bool playing() const { return playing_; }
    void poll();                 // UI thread, once per frame: detects a lost source
    const std::string& lastError() const { return error_; }

    // ---- source ----------------------------------------------------------------
    void selectSource(SourceKind k);
    SourceKind sourceKind() const { return kind_; }
    Source& source();
    void setSampleRate(double sps);
    double sampleRate() { return source().sampleRate(); }
    SimSource sim;
    RtlSdrSource rtl;
    RtlTcpSource tcp;

    // ---- tuning ----------------------------------------------------------------
    void tune(double absHz);
    void setCenter(double hz);
    double centerHz() const { return centerHz_; }
    double vfoHz() const { return centerHz_ + vfo.offsetHz; }
    TuningMode tuningMode = TuningMode::Normal;
    Vfo vfo;

    // ---- receiver --------------------------------------------------------------
    const RxConfig& rx() const { return rx_; }
    void setRx(const RxConfig& c);              // pushes to the DSP thread
    void setMode(Demod m);                      // also loads the mode's default bandwidth / step
    void setBandwidth(double hz);
    Receiver receiver;

    DspEngine dsp;
    SpectrumAnalyzer analyzer;
    AlsaSink audio;
    std::string mixerReport;

private:
    SourceKind kind_ = SourceKind::Simulated;
    RxConfig rx_;
    double centerHz_ = 100.0e6;
    bool playing_ = false;
    std::string error_;
};

}  // namespace osx
