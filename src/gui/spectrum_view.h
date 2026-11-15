// Main viewport: FFT spectrum (upper pane) + scrolling waterfall (lower pane)
// separated by a draggable splitter, with the SDR++-style Zoom / Max / Min
// strip on the right and an interactive VFO overlay.
#pragma once

#include <vector>

#include <imgui.h>

#include "core/vfo.h"
#include "dsp/spectrum_analyzer.h"
#include "gui/waterfall_gl.h"

namespace osx {

struct SpectrumViewIO {
    // in
    double centerHz = 100e6;
    double sampleRate = 2.4e6;
    Vfo* vfo = nullptr;
    // out (events raised by mouse interaction this frame)
    bool tuneRequested = false;    // user wants the VFO at tuneHz (absolute)
    double tuneHz = 0.0;
    bool centerRequested = false;  // user dragged the frequency scale
    double newCenterHz = 0.0;
};

class SpectrumView {
public:
    bool init();
    void shutdown();

    // Pull finished FFT lines from the analyzer; updates traces + waterfall.
    void feed(SpectrumAnalyzer& analyzer);
    void draw(SpectrumViewIO& io);

    void resetPeakHold();
    void clearHistory();                       // e.g. after a sample-rate change
    void ensureVisible(double offsetHz, double sampleRate);
    void resetView() { zoom = 0.0f; viewOffsetHz = 0.0; }
    void autoRange();                          // fit dB ranges to the current spectrum
    void setColormap(ColormapId id);

    // ---- user settings -------------------------------------------------------
    float fftMinDb = -110.0f, fftMaxDb = 0.0f;
    float wfMinDb = -95.0f, wfMaxDb = -25.0f;
    bool linkWaterfallRange = false;           // waterfall follows the FFT Min/Max
    float zoom = 0.0f;                         // 0 = full bandwidth .. 1 = max zoom
    double viewOffsetHz = 0.0;                 // view centre relative to hardware centre
    float smoothing = 0.6f;                    // 0 = raw .. 0.98 = heavy averaging
    bool peakHold = true;
    bool fillTrace = true;
    float fftPaneFrac = 0.42f;
    ColormapId colormap = ColormapId::ClassicSdrpp;

    // ---- measurements --------------------------------------------------------
    float snrDb() const { return snrDb_; }
    float noiseFloorDb() const { return noiseFloorDb_; }
    bool hasData() const { return !avg_.empty(); }
    uint64_t linesReceived() const { return lines_; }

    double viewSpanHz(double sampleRate) const;

private:
    enum class Drag { None, VfoMove, BwLeft, BwRight, FreqScale, ViewPan, DbScale, Splitter };

    void reduceToPixels(const std::vector<float>& src, std::vector<float>& out, int widthPx,
                        double loHz, double spanHz, double sampleRate) const;
    void updateMeasurements(const SpectrumViewIO& io);

    WaterfallGL waterfall_;
    std::vector<float> line_, avg_, peak_;
    std::vector<float> tracePx_, peakPx_;
    std::vector<ImVec2> pts_, peakPts_;
    std::vector<float> scratch_;
    uint64_t lines_ = 0, measuredAt_ = 0;
    float snrDb_ = 0.0f, noiseFloorDb_ = -100.0f, maxDb_ = -100.0f;

    Drag drag_ = Drag::None;
    double dragStartHz_ = 0.0, dragStartCenter_ = 0.0, dragStartView_ = 0.0;
    float dragStartMin_ = 0.0f, dragStartMax_ = 0.0f, dragStartY_ = 0.0f, dragStartX_ = 0.0f;
    float lastZoom_ = 0.0f;
};

}  // namespace osx
