#pragma once

#include <chrono>
#include <memory>

#include "core/ring_buffer.h"
#include "core/types.h"
#include "core/vfo.h"
#include "dsp/dsp_engine.h"
#include "dsp/spectrum_analyzer.h"
#include "gui/spectrum_view.h"
#include "gui/theme.h"
#include "source/sim_source.h"

namespace osx {

class App {
public:
    App();
    ~App();

    bool init(float dpiScale);
    void shutdown();
    void frame();  // builds one ImGui frame (UI thread)

    void setPlaying(bool play);
    bool playing() const { return playing_; }
    void tune(double absHz);
    void setCenter(double hz);

private:
    enum class TuningMode { Normal = 0, Center };

    void drawToolbar(float height);
    void drawSidebar();
    void applyStyle();
    void updateStats();

    // Thread 1 -> Thread 2 -> FFT worker plumbing.
    RingBuffer<uint8_t> hwRing_;   // raw interleaved uint8 I/Q
    RingBuffer<cf32> tapRing_;     // corrected complex baseband for the display FFT
    SimSource source_;
    DspEngine dsp_;
    SpectrumAnalyzer analyzer_;

    SpectrumView view_;
    Vfo vfo_;
    double centerHz_ = 100.0e6;
    bool playing_ = false;
    TuningMode tuningMode_ = TuningMode::Normal;

    // UI state
    bool sidebarOpen_ = true;
    float sidebarWidth_ = 0.0f;
    float volume_ = 0.5f;
    bool muted_ = false;
    ThemeId theme_ = ThemeId::SdrppDark;
    float dpiScale_ = 1.0f, uiScale_ = 1.0f, pendingUiScale_ = 1.0f;
    bool styleDirty_ = false;

    // Stats
    std::chrono::steady_clock::time_point statTime_{};
    uint64_t statSamples_ = 0, statLines_ = 0;
    double measuredRate_ = 0.0, measuredFftRate_ = 0.0;
};

}  // namespace osx
