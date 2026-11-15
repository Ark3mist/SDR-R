// Radio: the complete backend behind any front end.
// Owns the sources, the lock-free rings, the DSP thread (with its receiver),
// the display-FFT worker and the audio sink, and exposes thread-safe controls.
#pragma once

#include <string>

#include "audio/audio_sink.h"
#include "core/ring_buffer.h"
#include "core/types.h"
#include "core/vfo.h"
#include "dsp/dsp_engine.h"
#include "dsp/receiver.h"
#include "dsp/spectrum_analyzer.h"
#include "source/file_source.h"
#include "source/rtlsdr_source.h"
#include "source/rtltcp_source.h"
#include "source/sim_source.h"

namespace osx {

class Radio {
    // Declared first: every thread-owning member below holds a reference to these.
    RingBuffer<uint8_t> hwRing_;   // raw interleaved uint8 I/Q   (I/O -> DSP)
    RingBuffer<cf32> tapRing_;     // corrected baseband          (DSP -> FFT worker)
    RingBuffer<float> audioRing_;  // interleaved stereo float32  (DSP -> audio sink)
    RingBuffer<cf32> fileRing_;    // complex float from the IQ file player (I/O -> DSP)

public:
    enum class SourceKind { Simulated = 0, RtlSdr, RtlTcp, File, Count };
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
    // Like play(), but if the selected hardware cannot start, run the
    // simulator instead and keep looking for the RTL-SDR (see poll()).
    bool playWithFallback();
    void poll();                 // UI thread, once per frame: lost source -> fallback; re-detect
    bool fallbackActive() const { return fallback_; }
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
    FileSource file;
    bool openFile(const std::string& path);   // selects the file source

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
    // Passband edges relative to the VFO (asymmetric for SSB).
    void passband(double& lo, double& hi) const { osx::passband(rx_, lo, hi); }
    // Tune + switch demodulator + bandwidth in one step (bookmarks).
    void recall(double absHz, Demod mode, double bandwidthHz);
    Receiver receiver;

    DspEngine dsp;
    SpectrumAnalyzer analyzer;
    AudioSink audio;
    std::string mixerReport;

private:
    SourceKind kind_ = SourceKind::Simulated;
    RxConfig rx_;
    double centerHz_ = 100.0e6;
    bool playing_ = false;
    bool fallback_ = false;              // running the simulator because the RTL-SDR is missing
    double redetectAt_ = 0.0;
    std::string error_;
};

}  // namespace osx
