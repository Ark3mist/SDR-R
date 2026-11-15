#include "gui/spectrum_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "gui/theme.h"

namespace osx {

namespace {

inline bool inRect(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    return p.x >= a.x && p.x < b.x && p.y >= a.y && p.y < b.y;
}

// Smallest 1 / 2 / 2.5 / 5 x 10^k that is >= raw.
double niceStep(double raw) {
    const double mag = std::pow(10.0, std::floor(std::log10(std::max(raw, 1e-9))));
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (m * mag >= raw) return m * mag;
    return 10.0 * mag;
}

void formatMHz(char* buf, size_t len, double hz, double stepHz) {
    const int decimals = std::clamp(int(std::ceil(-std::log10(stepHz / 1e6) - 1e-9)), 0, 6);
    std::snprintf(buf, len, "%.*f", decimals, hz / 1e6);
}

double snapTo(double hz, double step) { return step > 0.0 ? std::round(hz / step) * step : hz; }

ImU32 withAlpha(ImU32 col, float a) {
    return (col & 0x00FFFFFFu) | (ImU32(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24);
}

}  // namespace

bool SpectrumView::init() { return waterfall_.init(); }
void SpectrumView::shutdown() { waterfall_.destroy(); }

void SpectrumView::setColormap(ColormapId id) {
    colormap = id;
    waterfall_.setColormap(id);
}

void SpectrumView::resetPeakHold() { peak_ = avg_; }

void SpectrumView::clearHistory() {
    waterfall_.clear();
    line_.clear();
    avg_.clear();
    peak_.clear();
}

double SpectrumView::viewSpanHz(double sampleRate) const {
    const int n = std::max(waterfall_.fftSize(), 1024);
    const double minSpan = std::max(sampleRate / double(n) * 32.0, 500.0);
    return sampleRate * std::pow(std::min(minSpan / sampleRate, 1.0), double(std::clamp(zoom, 0.0f, 1.0f)));
}

void SpectrumView::ensureVisible(double offsetHz, double sampleRate) {
    const double span = viewSpanHz(sampleRate);
    if (std::fabs(offsetHz - viewOffsetHz) > span * 0.45) viewOffsetHz = offsetHz;
    const double lim = (sampleRate - span) * 0.5;
    viewOffsetHz = std::clamp(viewOffsetHz, -lim, lim);
}

void SpectrumView::autoRange() {
    if (avg_.empty()) return;
    fftMinDb = std::floor((noiseFloorDb_ - 15.0f) / 5.0f) * 5.0f;
    fftMaxDb = std::min(10.0f, std::ceil((maxDb_ + 12.0f) / 5.0f) * 5.0f);
    wfMinDb = noiseFloorDb_ - 3.0f;
    wfMaxDb = std::max(wfMinDb + 20.0f, maxDb_ + 3.0f);
    linkWaterfallRange = false;
}

void SpectrumView::feed(SpectrumAnalyzer& analyzer) {
    while (analyzer.popLine(line_)) {
        const size_t n = line_.size();
        if (int(n) != waterfall_.fftSize()) waterfall_.configure(int(n));
        if (avg_.size() != n) {
            avg_ = line_;
            peak_ = line_;
        } else {
            const float a = std::clamp(smoothing, 0.0f, 0.98f), b = 1.0f - a;
            for (size_t i = 0; i < n; ++i) {
                avg_[i] = a * avg_[i] + b * line_[i];
                peak_[i] = std::max(peak_[i], avg_[i]);
            }
        }
        waterfall_.pushLine(line_.data());
        ++lines_;
    }
}

void SpectrumView::reduceToPixels(const std::vector<float>& src, std::vector<float>& out, int widthPx,
                                  double loHz, double spanHz, double sampleRate) const {
    out.resize(size_t(widthPx));
    const double n = double(src.size());
    const double binsPerPx = spanHz / double(widthPx) / sampleRate * n;
    const double b0 = (loHz / sampleRate + 0.5) * n;
    const int last = int(src.size()) - 1;
    if (binsPerPx >= 1.0) {
        // Peak-detect: several bins per pixel column.
        for (int x = 0; x < widthPx; ++x) {
            int i0 = std::clamp(int(std::floor(b0 + double(x) * binsPerPx + 0.5)), 0, last);
            int i1 = std::clamp(int(std::floor(b0 + double(x + 1) * binsPerPx + 0.5)), i0 + 1, last + 1);
            float m = src[size_t(i0)];
            for (int i = i0 + 1; i < i1; ++i) m = std::max(m, src[size_t(i)]);
            out[size_t(x)] = m;
        }
    } else {
        // Zoomed past bin resolution: linear interpolation between bins.
        for (int x = 0; x < widthPx; ++x) {
            const double pos = std::clamp(b0 + (double(x) + 0.5) * binsPerPx, 0.0, double(last));
            const int i = std::min(int(pos), last - 1 < 0 ? 0 : last - 1);
            const float f = float(pos - double(i));
            out[size_t(x)] = src[size_t(i)] * (1.0f - f) + src[size_t(std::min(i + 1, last))] * f;
        }
    }
}

// SNR estimate: strongest bin inside the VFO passband vs. the 25th-percentile
// bin level across the whole spectrum (robust against strong signals).
void SpectrumView::updateMeasurements(const SpectrumViewIO& io) {
    if (avg_.empty() || measuredAt_ == lines_) return;
    measuredAt_ = lines_;
    const size_t n = avg_.size();
    const size_t stride = std::max<size_t>(1, n / 2048);
    scratch_.clear();
    float mx = -200.0f;
    for (size_t i = 0; i < n; i += stride) {
        scratch_.push_back(avg_[i]);
        mx = std::max(mx, avg_[i]);
    }
    auto nth = scratch_.begin() + std::ptrdiff_t(scratch_.size() / 4);
    std::nth_element(scratch_.begin(), nth, scratch_.end());
    noiseFloorDb_ = *nth;
    maxDb_ = mx;

    if (io.vfo) {
        const double lo = (io.vfo->offsetHz - io.vfo->bandwidthHz * 0.5) / io.sampleRate + 0.5;
        const double hi = (io.vfo->offsetHz + io.vfo->bandwidthHz * 0.5) / io.sampleRate + 0.5;
        const size_t i0 = size_t(std::clamp(lo * double(n), 0.0, double(n - 1)));
        const size_t i1 = size_t(std::clamp(hi * double(n), double(i0 + 1), double(n)));
        float pk = -200.0f;
        for (size_t i = i0; i < i1; ++i) pk = std::max(pk, avg_[i]);
        const float snr = std::max(0.0f, pk - noiseFloorDb_);
        snrDb_ += 0.3f * (snr - snrDb_);
    }
}

void SpectrumView::draw(SpectrumViewIO& io) {
    const Palette& pal = palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float em = ImGui::GetFontSize();
    const float dbW = em * 3.0f, stripW = em * 6.3f, freqH = em * 1.8f, splitH = std::max(4.0f, em * 0.35f);
    const float topPad = em * 0.5f;
    const float plotW = std::floor(avail.x - dbW - stripW);
    const float usableH = avail.y - freqH - splitH - topPad;
    if (plotW < 64.0f || usableH < 96.0f) return;

    updateMeasurements(io);

    const double sr = io.sampleRate;
    Vfo& vfo = *io.vfo;

    // ---- geometry -----------------------------------------------------------
    const float fftH = std::floor(std::clamp(usableH * fftPaneFrac, 40.0f, usableH - 40.0f));
    const ImVec2 fftA{p0.x + dbW, p0.y + topPad}, fftB{fftA.x + plotW, fftA.y + fftH};
    const ImVec2 frqA{fftA.x, fftB.y}, frqB{fftB.x, fftB.y + freqH};
    const ImVec2 splA{p0.x, frqB.y}, splB{fftB.x, frqB.y + splitH};
    const ImVec2 wfA{fftA.x, splB.y}, wfB{fftB.x, p0.y + avail.y};
    const float wfH = wfB.y - wfA.y;

    // ---- view window ----------------------------------------------------------
    if (zoom != lastZoom_ && drag_ == Drag::None) {
        // Zoom changed from a slider: keep the VFO in view, as SDR++ does.
        viewOffsetHz = vfo.offsetHz;
        lastZoom_ = zoom;
    }
    double span = viewSpanHz(sr);
    auto clampView = [&] {
        const double lim = (sr - span) * 0.5;
        viewOffsetHz = std::clamp(viewOffsetHz, -lim, lim);
    };
    clampView();
    double lo = viewOffsetHz - span * 0.5;
    auto hzToX = [&](double f) { return fftA.x + float((f - lo) / span) * plotW; };
    auto xToHz = [&](float x) { return lo + double(x - fftA.x) / double(plotW) * span; };

    // ---- input ----------------------------------------------------------------
    ImGui::InvisibleButton("##spectrum", {dbW + plotW, avail.y},
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& gio = ImGui::GetIO();
    const ImVec2 m = gio.MousePos;
    const bool inFft = inRect(m, fftA, fftB), inWf = inRect(m, wfA, wfB);
    const bool inPlot = inFft || inWf;
    const float xl = hzToX(vfo.offsetHz - vfo.bandwidthHz * 0.5);
    const float xr = hzToX(vfo.offsetHz + vfo.bandwidthHz * 0.5);
    const float grab = std::max(5.0f, em * 0.35f);
    const bool nearL = inPlot && std::fabs(m.x - xl) <= grab;
    const bool nearR = inPlot && std::fabs(m.x - xr) <= grab;
    const bool onSplit = inRect(m, splA, splB);
    const bool onFreq = inRect(m, frqA, frqB);
    const bool onDb = inRect(m, {p0.x, fftA.y}, {fftA.x, fftB.y});

    if (hovered && drag_ == Drag::None) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            dragStartX_ = m.x;
            dragStartY_ = m.y;
            if (onSplit) drag_ = Drag::Splitter;
            else if (onDb) { drag_ = Drag::DbScale; dragStartMin_ = fftMinDb; dragStartMax_ = fftMaxDb; }
            else if (onFreq) { drag_ = Drag::FreqScale; dragStartCenter_ = io.centerHz; }
            else if (nearL) drag_ = Drag::BwLeft;
            else if (nearR) drag_ = Drag::BwRight;
            else if (inPlot) drag_ = Drag::VfoMove;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && (inPlot || onFreq)) {
            drag_ = Drag::ViewPan;
            dragStartX_ = m.x;
            dragStartView_ = viewOffsetHz;
        }
    }

    const bool leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool midDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    switch (drag_) {
        case Drag::VfoMove: {
            const double f = snapTo(io.centerHz + xToHz(m.x), vfo.snapHz);
            if (f != io.centerHz + vfo.offsetHz) { io.tuneRequested = true; io.tuneHz = f; }
            break;
        }
        case Drag::BwLeft:
        case Drag::BwRight: {
            const double bw = 2.0 * std::fabs(xToHz(m.x) - vfo.offsetHz);
            vfo.bandwidthHz = std::clamp(bw, vfo.minBandwidthHz, std::min(vfo.maxBandwidthHz, sr));
            break;
        }
        case Drag::FreqScale: {
            const double dHz = double(m.x - dragStartX_) / double(plotW) * span;
            const double c = std::max(0.0, snapTo(dragStartCenter_ - dHz, vfo.snapHz));
            if (c != io.centerHz) { io.centerRequested = true; io.newCenterHz = c; }
            break;
        }
        case Drag::ViewPan:
            viewOffsetHz = dragStartView_ - double(m.x - dragStartX_) / double(plotW) * span;
            clampView();
            break;
        case Drag::DbScale: {
            const float d = (m.y - dragStartY_) / fftH * (dragStartMax_ - dragStartMin_);
            fftMinDb = dragStartMin_ + d;
            fftMaxDb = dragStartMax_ + d;
            break;
        }
        case Drag::Splitter:
            fftPaneFrac = std::clamp((m.y - p0.y - topPad - freqH * 0.5f) / usableH, 0.1f, 0.9f);
            break;
        case Drag::None: break;
    }
    if (drag_ == Drag::ViewPan ? !midDown : !leftDown) drag_ = Drag::None;

    if (hovered && gio.MouseWheel != 0.0f && (inPlot || onFreq)) {
        if (gio.KeyCtrl) {
            // Zoom about the cursor.
            const double fCur = xToHz(m.x);
            zoom = std::clamp(zoom + gio.MouseWheel * 0.04f, 0.0f, 1.0f);
            lastZoom_ = zoom;
            span = viewSpanHz(sr);
            viewOffsetHz = fCur - double(m.x - fftA.x) / double(plotW) * span + span * 0.5;
            clampView();
        } else {
            io.tuneRequested = true;
            io.tuneHz = snapTo(io.centerHz + vfo.offsetHz, vfo.snapHz) + double(gio.MouseWheel) * vfo.snapHz;
        }
    }
    lo = viewOffsetHz - span * 0.5;

    if (drag_ == Drag::Splitter || (drag_ == Drag::None && hovered && onSplit))
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    else if (drag_ == Drag::BwLeft || drag_ == Drag::BwRight || (drag_ == Drag::None && hovered && (nearL || nearR)))
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    else if (drag_ == Drag::FreqScale || drag_ == Drag::ViewPan || (drag_ == Drag::None && hovered && onFreq))
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    else if (drag_ == Drag::DbScale || (drag_ == Drag::None && hovered && onDb))
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    if (fftMaxDb - fftMinDb < 10.0f) fftMaxDb = fftMinDb + 10.0f;
    if (linkWaterfallRange) { wfMinDb = fftMinDb; wfMaxDb = fftMaxDb; }
    if (wfMaxDb - wfMinDb < 5.0f) wfMaxDb = wfMinDb + 5.0f;

    // ---- FFT pane: background, grid, scales -----------------------------------
    dl->AddRectFilled(fftA, fftB, pal.plotBg);
    const float dbRange = fftMaxDb - fftMinDb;
    auto dbToY = [&](float db) { return fftB.y - (db - fftMinDb) / dbRange * fftH; };
    char buf[64];

    float dbStep = 10.0f;
    for (float s : {1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 50.0f}) {
        dbStep = s;
        if (s / dbRange * fftH >= em * 1.5f) break;
    }
    for (float db = std::ceil(fftMinDb / dbStep) * dbStep; db <= fftMaxDb + 0.01f; db += dbStep) {
        const float y = std::round(dbToY(db));
        dl->AddLine({fftA.x, y}, {fftB.x, y}, pal.grid);
        std::snprintf(buf, sizeof(buf), "%.0f", double(db));
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        const float ty = std::clamp(y - ts.y * 0.5f, fftA.y - topPad, fftB.y - ts.y);
        dl->AddText({fftA.x - ts.x - em * 0.35f, ty}, pal.gridText, buf);
    }

    const double fStep = niceStep(span * double(em * 7.0f / plotW));
    const double absLo = io.centerHz + lo;
    for (double f = std::ceil(absLo / fStep) * fStep; f <= absLo + span; f += fStep) {
        const float x = std::round(hzToX(f - io.centerHz));
        dl->AddLine({x, fftA.y}, {x, fftB.y + em * 0.3f}, pal.grid);
        formatMHz(buf, sizeof(buf), f, fStep);
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        const float tx = std::clamp(x - ts.x * 0.5f, fftA.x, fftB.x - ts.x);
        dl->AddText({tx, frqA.y + em * 0.4f}, pal.gridText, buf);
    }
    dl->AddText({p0.x + em * 0.2f, frqA.y + em * 0.4f}, withAlpha(pal.gridText, 0.6f), "MHz");

    // ---- traces ---------------------------------------------------------------
    const int wpx = int(plotW);
    dl->PushClipRect(fftA, fftB, true);
    if (!avg_.empty()) {
        reduceToPixels(avg_, tracePx_, wpx, lo, span, sr);
        pts_.resize(size_t(wpx));
        for (int x = 0; x < wpx; ++x)
            pts_[size_t(x)] = {fftA.x + float(x) + 0.5f,
                               std::clamp(dbToY(tracePx_[size_t(x)]), fftA.y - 2.0f, fftB.y + 2.0f)};
        if (fillTrace) {
            const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
            dl->PrimReserve((wpx - 1) * 6, (wpx - 1) * 4);
            for (int x = 0; x + 1 < wpx; ++x) {
                const ImVec2& a = pts_[size_t(x)];
                const ImVec2& b = pts_[size_t(x + 1)];
                dl->PrimQuadUV(a, b, {b.x, fftB.y}, {a.x, fftB.y}, uv, uv, uv, uv, pal.traceFill);
            }
        }
        if (peakHold && peak_.size() == avg_.size()) {
            reduceToPixels(peak_, peakPx_, wpx, lo, span, sr);
            peakPts_.resize(size_t(wpx));
            for (int x = 0; x < wpx; ++x)
                peakPts_[size_t(x)] = {pts_[size_t(x)].x,
                                       std::clamp(dbToY(peakPx_[size_t(x)]), fftA.y - 2.0f, fftB.y + 2.0f)};
            dl->AddPolyline(peakPts_.data(), wpx, pal.peakHold, ImDrawFlags_None, 1.0f);
        }
        dl->AddPolyline(pts_.data(), wpx, pal.trace, ImDrawFlags_None, std::max(1.0f, em / 14.0f));
    }

    // ---- VFO overlay (FFT pane) -------------------------------------------------
    const float xc = hzToX(vfo.offsetHz);
    const float vxl = hzToX(vfo.offsetHz - vfo.bandwidthHz * 0.5);
    const float vxr = hzToX(vfo.offsetHz + vfo.bandwidthHz * 0.5);
    const bool edgeHot = drag_ == Drag::BwLeft || drag_ == Drag::BwRight || (hovered && (nearL || nearR));
    dl->AddRectFilled({vxl, fftA.y}, {vxr, fftB.y}, withAlpha(vfo.color, 0.16f));
    dl->AddLine({vxl, fftA.y}, {vxl, fftB.y}, withAlpha(vfo.color, edgeHot ? 0.95f : 0.45f), edgeHot ? 2.0f : 1.0f);
    dl->AddLine({vxr, fftA.y}, {vxr, fftB.y}, withAlpha(vfo.color, edgeHot ? 0.95f : 0.45f), edgeHot ? 2.0f : 1.0f);
    dl->AddLine({xc, fftA.y}, {xc, fftB.y}, pal.vfoCenter, std::max(1.0f, em / 12.0f));
    {
        const ImVec2 ts = ImGui::CalcTextSize(vfo.name.c_str());
        const ImVec2 tp{xc + em * 0.3f, fftA.y + em * 0.15f};
        dl->AddRectFilled({tp.x - 2.0f, tp.y}, {tp.x + ts.x + 2.0f, tp.y + ts.y}, withAlpha(pal.plotBg, 0.7f));
        dl->AddText(tp, vfo.color, vfo.name.c_str());
    }
    dl->PopClipRect();
    dl->AddRect(fftA, fftB, pal.grid);

    // ---- splitter ---------------------------------------------------------------
    {
        const bool hot = drag_ == Drag::Splitter || (hovered && onSplit && drag_ == Drag::None);
        const float cy = (splA.y + splB.y) * 0.5f;
        dl->AddRectFilled({fftA.x, cy - 1.0f}, {fftB.x, cy + 1.0f}, hot ? pal.accent : pal.grid);
    }

    // ---- waterfall --------------------------------------------------------------
    const float u0 = float(lo / sr + 0.5), u1 = float((lo + span) / sr + 0.5);
    const GLuint tex = waterfall_.render(wpx, int(wfH), u0, u1, wfMinDb, wfMaxDb);
    dl->AddImage(ImTextureRef(ImTextureID(tex)), wfA, {wfA.x + float(wpx), wfB.y}, {0.0f, 1.0f}, {1.0f, 0.0f});
    dl->PushClipRect(wfA, wfB, true);
    dl->AddLine({vxl, wfA.y}, {vxl, wfB.y}, withAlpha(vfo.color, 0.30f));
    dl->AddLine({vxr, wfA.y}, {vxr, wfB.y}, withAlpha(vfo.color, 0.30f));
    dl->AddLine({xc, wfA.y}, {xc, wfB.y}, withAlpha(pal.vfoCenter, 0.75f));
    dl->PopClipRect();

    // ---- cursor readout -----------------------------------------------------------
    if (hovered && inPlot && drag_ == Drag::None) {
        dl->AddLine({m.x, fftA.y}, {m.x, fftB.y}, pal.cursor);
        dl->AddLine({m.x, wfA.y}, {m.x, wfB.y}, pal.cursor);
        const double f = io.centerHz + xToHz(m.x);
        const int col = std::clamp(int(m.x - fftA.x), 0, wpx - 1);
        if (!tracePx_.empty() && size_t(col) < tracePx_.size())
            std::snprintf(buf, sizeof(buf), "%.6f MHz   %.1f dBFS", f / 1e6, double(tracePx_[size_t(col)]));
        else
            std::snprintf(buf, sizeof(buf), "%.6f MHz", f / 1e6);
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        const ImVec2 tp{fftB.x - ts.x - em * 0.5f, fftA.y + em * 0.25f};
        dl->AddRectFilled({tp.x - 4.0f, tp.y - 1.0f}, {tp.x + ts.x + 4.0f, tp.y + ts.y + 1.0f}, withAlpha(pal.plotBg, 0.8f));
        dl->AddText(tp, pal.gridText, buf);
    }

    // ---- right strip: Zoom / Max / Min ------------------------------------------
    const float colW = stripW / 3.0f, sliderW = em * 1.25f;
    const float labelY = p0.y + em * 0.2f, sliderY = labelY + em * 1.4f;
    const float sliderH = p0.y + avail.y - sliderY - em * 0.4f;
    struct Strip { const char* label; const char* id; float* v; float lo, hi; };
    const Strip strips[3] = {{"Zoom", "##zoom", &zoom, 0.0f, 1.0f},
                             {"Max", "##max", &fftMaxDb, -100.0f, 20.0f},
                             {"Min", "##min", &fftMinDb, -160.0f, -20.0f}};
    for (int i = 0; i < 3; ++i) {
        const float cx = fftB.x + colW * (float(i) + 0.5f) + em * 0.15f;
        const ImVec2 ts = ImGui::CalcTextSize(strips[i].label);
        dl->AddText({cx - ts.x * 0.5f, labelY}, pal.gridText, strips[i].label);
        ImGui::SetCursorScreenPos({cx - sliderW * 0.5f, sliderY});
        ImGui::VSliderFloat(strips[i].id, {sliderW, sliderH}, strips[i].v, strips[i].lo, strips[i].hi, "");
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            if (i == 0) ImGui::SetTooltip("Span %.1f kHz", viewSpanHz(sr) / 1e3);
            else ImGui::SetTooltip("%.0f dB", double(*strips[i].v));
        }
    }
    if (fftMinDb > fftMaxDb - 10.0f) fftMinDb = fftMaxDb - 10.0f;
}

}  // namespace osx
