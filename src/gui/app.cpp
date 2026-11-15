#include "gui/app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include "gui/frequency_select.h"

namespace osx {

namespace {

// Square toolbar button with a vector icon drawn by `icon(dl, centre, radius, colour)`.
template <typename F>
bool IconButton(const char* id, float size, F&& icon) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::Button(id, {size, size});
    icon(ImGui::GetWindowDrawList(), ImVec2{p.x + size * 0.5f, p.y + size * 0.5f}, size * 0.26f, palette().icon);
    return pressed;
}

void formatRate(char* buf, size_t n, double sps) {
    if (sps >= 1e6) std::snprintf(buf, n, "%.3f MSPS", sps / 1e6);
    else std::snprintf(buf, n, "%.0f kSPS", sps / 1e3);
}

}  // namespace

App::App()
    : hwRing_(size_t(1) << 24),   // 16 MiB: ~2.6 s of 3.2 MSPS uint8 I/Q
      tapRing_(size_t(1) << 20),
      dsp_(hwRing_, tapRing_),
      analyzer_(tapRing_) {
    vfo_.color = IM_COL32(255, 255, 255, 255);
}

App::~App() { shutdown(); }

bool App::init(float dpiScale) {
    dpiScale_ = dpiScale;
    applyStyle();
    if (!view_.init()) return false;
    source_.setCenterFrequency(centerHz_);
    analyzer_.start();
    statTime_ = std::chrono::steady_clock::now();
    return true;
}

void App::shutdown() {
    setPlaying(false);
    analyzer_.stop();
    view_.shutdown();
}

void App::applyStyle() { applyTheme(theme_, dpiScale_ * uiScale_); }

void App::setPlaying(bool play) {
    if (play == playing_) return;
    if (play) {
        hwRing_.reset();
        dsp_.start(source_.sampleRate());
        source_.start(hwRing_);
    } else {
        source_.stop();
        dsp_.stop();
    }
    playing_ = play;
}

void App::setCenter(double hz) {
    hz = std::clamp(hz, 0.0, 9'999'999'999.0);
    if (hz == centerHz_) return;
    centerHz_ = hz;
    source_.setCenterFrequency(hz);
    view_.resetPeakHold();  // the whole spectrum just shifted under the trace
}

void App::tune(double absHz) {
    absHz = std::clamp(absHz, 0.0, 9'999'999'999.0);
    const double sr = source_.sampleRate();
    if (tuningMode_ == TuningMode::Center) {
        vfo_.offsetHz = 0.0;
        setCenter(absHz);
    } else {
        // Normal mode: move the VFO inside the current bandwidth when it fits,
        // otherwise drag the hardware centre along, keeping the VFO offset.
        const double lim = std::max(0.0, sr * 0.5 - vfo_.bandwidthHz * 0.5);
        const double off = absHz - centerHz_;
        if (std::fabs(off) <= lim) {
            vfo_.offsetHz = off;
        } else {
            const double keep = std::clamp(vfo_.offsetHz, -lim, lim);
            setCenter(absHz - keep);
            vfo_.offsetHz = absHz - centerHz_;
        }
    }
    view_.ensureVisible(vfo_.offsetHz, sr);
}

void App::updateStats() {
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - statTime_).count();
    if (dt < 0.5) return;
    const uint64_t s = dsp_.samplesProcessed(), l = view_.linesReceived();
    measuredRate_ = double(s - statSamples_) / dt;
    measuredFftRate_ = double(l - statLines_) / dt;
    statSamples_ = s;
    statLines_ = l;
    statTime_ = now;
}

void App::drawToolbar(float height) {
    const Palette& pal = palette();
    const float em = ImGui::GetFontSize();
    const float btn = height - ImGui::GetStyle().WindowPadding.y * 2.0f;

    if (IconButton("##menu", btn, [](ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
            for (int i = -1; i <= 1; ++i)
                dl->AddLine({c.x - r, c.y + float(i) * r * 0.7f}, {c.x + r, c.y + float(i) * r * 0.7f}, col, std::max(1.5f, r * 0.2f));
        }))
        sidebarOpen_ = !sidebarOpen_;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show / hide the sidebar");

    ImGui::SameLine();
    const bool wasPlaying = playing_;
    if (IconButton("##play", btn, [wasPlaying](ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
            if (wasPlaying) dl->AddRectFilled({c.x - r * 0.8f, c.y - r * 0.8f}, {c.x + r * 0.8f, c.y + r * 0.8f}, col, 1.0f);
            else dl->AddTriangleFilled({c.x - r * 0.7f, c.y - r}, {c.x - r * 0.7f, c.y + r}, {c.x + r, c.y}, col);
        }))
        setPlaying(!playing_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(playing_ ? "Stop" : "Play");

    ImGui::SameLine();
    const bool muted = muted_ || volume_ <= 0.0f;
    if (IconButton("##mute", btn, [muted](ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
            const float t = std::max(1.5f, r * 0.18f);
            dl->AddRectFilled({c.x - r, c.y - r * 0.4f}, {c.x - r * 0.45f, c.y + r * 0.4f}, col);
            dl->AddTriangleFilled({c.x - r * 0.5f, c.y - r * 0.4f}, {c.x + r * 0.1f, c.y - r}, {c.x + r * 0.1f, c.y + r}, col);
            dl->AddTriangleFilled({c.x - r * 0.5f, c.y - r * 0.4f}, {c.x + r * 0.1f, c.y + r}, {c.x - r * 0.5f, c.y + r * 0.4f}, col);
            if (muted) {
                dl->AddLine({c.x + r * 0.4f, c.y - r * 0.45f}, {c.x + r * 1.1f, c.y + r * 0.45f}, col, t);
                dl->AddLine({c.x + r * 0.4f, c.y + r * 0.45f}, {c.x + r * 1.1f, c.y - r * 0.45f}, col, t);
            } else {
                dl->PathArcTo({c.x + r * 0.1f, c.y}, r * 0.55f, -0.8f, 0.8f, 8);
                dl->PathStroke(col, ImDrawFlags_None, t);
                dl->PathArcTo({c.x + r * 0.1f, c.y}, r * 1.0f, -0.8f, 0.8f, 10);
                dl->PathStroke(col, ImDrawFlags_None, t);
            }
        }))
        muted_ = !muted_;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(muted_ ? "Unmute" : "Mute");

    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (btn - ImGui::GetFrameHeight()) * 0.5f);
    ImGui::SetNextItemWidth(em * 11.0f);
    ImGui::SliderFloat("##volume", &volume_, 0.0f, 1.0f, "");
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetTooltip("Volume %.0f %%", double(volume_ * 100.0f));

    // Big frequency display.
    ImGui::SameLine(0.0f, em * 1.2f);
    const float digitSize = 34.0f;
    ImGui::SetCursorPosY((height - digitSize * dpiScale_ * uiScale_) * 0.5f);
    double freq = centerHz_ + vfo_.offsetHz;
    if (FrequencySelect("##freq", &freq, digitSize)) tune(freq);

    // SNR meter, right-aligned.
    const float meterW = em * 17.0f;
    const float avail = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    ImGui::SameLine();
    if (avail - meterW > ImGui::GetCursorPosX() + em) {
        ImGui::SetCursorPosX(avail - meterW);
        ImGui::SetCursorPosY(ImGui::GetStyle().WindowPadding.y);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float labelW = ImGui::CalcTextSize("SNR").x + em * 0.6f;
        const float x0 = p.x + labelW, x1 = p.x + meterW - em * 0.8f;
        const float barH = btn * 0.30f, y0 = p.y + btn * 0.12f;
        const float snr = playing_ ? std::clamp(view_.snrDb(), 0.0f, 90.0f) : 0.0f;
        dl->AddText({p.x, y0 + (barH - em) * 0.5f}, pal.gridText, "SNR");
        dl->AddRectFilled({x0, y0}, {x1, y0 + barH}, ImGui::GetColorU32(ImGuiCol_FrameBg), 2.0f);
        dl->AddRectFilled({x0, y0}, {x0 + (x1 - x0) * snr / 90.0f, y0 + barH}, pal.accent, 2.0f);
        ImGui::PushFont(nullptr, 12.0f);
        for (int db = 0; db <= 90; db += 10) {
            const float x = x0 + (x1 - x0) * float(db) / 90.0f;
            dl->AddLine({x, y0 + barH}, {x, y0 + barH + btn * 0.10f}, pal.gridText);
            char t[8];
            std::snprintf(t, sizeof(t), "%d", db);
            const ImVec2 ts = ImGui::CalcTextSize(t);
            dl->AddText({x - ts.x * 0.5f, y0 + barH + btn * 0.12f}, pal.gridText, t);
        }
        ImGui::PopFont();
        ImGui::Dummy({meterW, btn});
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("SNR estimate in VFO passband: %.1f dB", double(view_.snrDb()));
    }
}

void App::drawSidebar() {
    const float em = ImGui::GetFontSize();
    const ImGuiTreeNodeFlags open = ImGuiTreeNodeFlags_DefaultOpen;
    ImGui::PushItemWidth(-em * 7.5f);
    char buf[96];

    // ---- Source --------------------------------------------------------------
    if (ImGui::CollapsingHeader("Source", open)) {
        ImGui::PushID("source");
        if (ImGui::BeginCombo("Device", source_.name())) {
            ImGui::Selectable(source_.name(), true);
            ImGui::EndCombo();
        }
        const auto rates = source_.sampleRates();
        formatRate(buf, sizeof(buf), source_.sampleRate());
        if (ImGui::BeginCombo("Sample rate", buf)) {
            for (double r : rates) {
                char item[32];
                formatRate(item, sizeof(item), r);
                if (ImGui::Selectable(item, r == source_.sampleRate()) && r != source_.sampleRate()) {
                    const bool was = playing_;
                    setPlaying(false);
                    source_.setSampleRate(r);
                    view_.clearHistory();
                    view_.resetView();
                    const double lim = std::max(0.0, r * 0.5 - vfo_.bandwidthHz * 0.5);
                    vfo_.offsetHz = std::clamp(vfo_.offsetHz, -lim, lim);
                    setPlaying(was);
                }
            }
            ImGui::EndCombo();
        }
        float v = source_.gainDb.load();
        if (ImGui::SliderFloat("Signal gain", &v, -40.0f, 20.0f, "%.1f dB")) source_.gainDb = v;
        v = source_.noiseDb.load();
        if (ImGui::SliderFloat("Noise power", &v, -90.0f, -10.0f, "%.1f dBFS")) source_.noiseDb = v;
        v = source_.dcOffset.load() * 100.0f;
        if (ImGui::SliderFloat("ADC DC offset", &v, 0.0f, 5.0f, "%.2f %%FS")) source_.dcOffset = v / 100.0f;
        bool en = source_.emittersEnabled.load();
        if (ImGui::Checkbox("Emitters enabled", &en)) source_.emittersEnabled = en;
        if (ImGui::TreeNode("Band plan (repeats every 2 MHz)")) {
            for (const auto& e : SimSource::bandPlan())
                ImGui::BulletText("+%4.0f kHz  %5.0f dBFS  %s", e.offsetHz / 1e3, double(e.levelDb), e.label);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    // ---- Radio ---------------------------------------------------------------
    if (ImGui::CollapsingHeader("Radio", open)) {
        ImGui::PushID("radio");
        const double sr = source_.sampleRate();
        ImGui::Text("%s  %.6f MHz", vfo_.name.c_str(), (centerHz_ + vfo_.offsetHz) / 1e6);
        ImGui::TextDisabled("Centre %.6f MHz   offset %+.1f kHz", centerHz_ / 1e6, vfo_.offsetHz / 1e3);

        float bwK = float(vfo_.bandwidthHz / 1e3);
        const float maxK = float(std::min(vfo_.maxBandwidthHz, sr) / 1e3);
        if (ImGui::SliderFloat("Bandwidth", &bwK, float(vfo_.minBandwidthHz / 1e3), maxK, "%.2f kHz", ImGuiSliderFlags_Logarithmic))
            vfo_.bandwidthHz = std::clamp(double(bwK) * 1e3, vfo_.minBandwidthHz, vfo_.maxBandwidthHz);

        static const double snaps[] = {1, 10, 100, 500, 1e3, 2.5e3, 5e3, 6.25e3, 8.33e3, 9e3, 10e3, 12.5e3, 25e3, 50e3, 100e3, 200e3};
        auto snapLabel = [](double s, char* out, size_t n) {
            if (s >= 1e3) std::snprintf(out, n, "%g kHz", s / 1e3);
            else std::snprintf(out, n, "%g Hz", s);
        };
        snapLabel(vfo_.snapHz, buf, sizeof(buf));
        if (ImGui::BeginCombo("Snap interval", buf)) {
            for (double s : snaps) {
                char item[32];
                snapLabel(s, item, sizeof(item));
                if (ImGui::Selectable(item, s == vfo_.snapHz)) vfo_.snapHz = s;
            }
            ImGui::EndCombo();
        }
        int mode = int(tuningMode_);
        if (ImGui::Combo("Tuning mode", &mode, "Normal\0Centre\0")) {
            tuningMode_ = TuningMode(mode);
            if (tuningMode_ == TuningMode::Center) tune(centerHz_ + vfo_.offsetHz);
        }
        ImVec4 col = ImGui::ColorConvertU32ToFloat4(vfo_.color);
        if (ImGui::ColorEdit3("VFO colour", &col.x, ImGuiColorEditFlags_NoInputs)) {
            col.w = 1.0f;
            vfo_.color = ImGui::ColorConvertFloat4ToU32(col);
        }
        ImGui::PopID();
    }

    // ---- DSP -----------------------------------------------------------------
    if (ImGui::CollapsingHeader("DSP", open)) {
        ImGui::PushID("dsp");
        bool b = dsp_.dcRemoval.load();
        if (ImGui::Checkbox("DC spike removal", &b)) dsp_.dcRemoval = b;
        b = dsp_.invertIq.load();
        if (ImGui::Checkbox("Invert IQ", &b)) {
            dsp_.invertIq = b;
            view_.resetPeakHold();
        }
        ImGui::PopID();
    }

    // ---- Display -------------------------------------------------------------
    if (ImGui::CollapsingHeader("Display", open)) {
        ImGui::PushID("display");
        std::snprintf(buf, sizeof(buf), "%d", analyzer_.fftSize());
        if (ImGui::BeginCombo("FFT size", buf)) {
            for (int n = 1024; n <= 65536; n *= 2) {
                char item[16];
                std::snprintf(item, sizeof(item), "%d", n);
                if (ImGui::Selectable(item, n == analyzer_.fftSize())) analyzer_.setFftSize(n);
            }
            ImGui::EndCombo();
        }
        int rate = analyzer_.rate();
        if (ImGui::SliderInt("FFT rate", &rate, 5, 60, "%d fps")) analyzer_.setRate(rate);
        if (ImGui::BeginCombo("FFT window", fftWindowName(analyzer_.window()))) {
            for (int w = 0; w < int(FftWindow::Count); ++w)
                if (ImGui::Selectable(fftWindowName(FftWindow(w)), FftWindow(w) == analyzer_.window()))
                    analyzer_.setWindow(FftWindow(w));
            ImGui::EndCombo();
        }
        ImGui::SliderFloat("Smoothing", &view_.smoothing, 0.0f, 0.98f, "%.2f");
        ImGui::Checkbox("Peak hold", &view_.peakHold);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) view_.resetPeakHold();
        ImGui::SameLine();
        ImGui::Checkbox("Fill", &view_.fillTrace);

        ImGui::SeparatorText("Levels");
        ImGui::SliderFloat("FFT max", &view_.fftMaxDb, -100.0f, 20.0f, "%.0f dB");
        ImGui::SliderFloat("FFT min", &view_.fftMinDb, -160.0f, -20.0f, "%.0f dB");
        ImGui::Checkbox("Waterfall follows FFT range", &view_.linkWaterfallRange);
        ImGui::BeginDisabled(view_.linkWaterfallRange);
        ImGui::SliderFloat("Waterfall max", &view_.wfMaxDb, -100.0f, 20.0f, "%.0f dB");
        ImGui::SliderFloat("Waterfall min", &view_.wfMinDb, -160.0f, -20.0f, "%.0f dB");
        ImGui::EndDisabled();
        if (ImGui::Button("Auto range")) view_.autoRange();

        ImGui::SeparatorText("Waterfall / view");
        if (ImGui::BeginCombo("Colormap", colormapName(view_.colormap))) {
            for (int c = 0; c < int(ColormapId::Count); ++c)
                if (ImGui::Selectable(colormapName(ColormapId(c)), ColormapId(c) == view_.colormap))
                    view_.setColormap(ColormapId(c));
            ImGui::EndCombo();
        }
        ImGui::SliderFloat("Zoom", &view_.zoom, 0.0f, 1.0f, "%.2f");
        const double span = view_.viewSpanHz(source_.sampleRate());
        const float lim = float((source_.sampleRate() - span) * 0.5 / 1e3);
        float pan = float(view_.viewOffsetHz / 1e3);
        ImGui::BeginDisabled(lim <= 0.0f);
        if (ImGui::SliderFloat("Pan", &pan, -lim, lim, "%.1f kHz")) view_.viewOffsetHz = double(pan) * 1e3;
        ImGui::EndDisabled();
        if (ImGui::Button("Reset view")) view_.resetView();
        ImGui::SameLine();
        if (ImGui::Button("Clear waterfall")) view_.clearHistory();
        ImGui::PopID();
    }

    // ---- Appearance ----------------------------------------------------------
    if (ImGui::CollapsingHeader("Appearance")) {
        ImGui::PushID("appearance");
        if (ImGui::BeginCombo("Theme", themeName(theme_))) {
            for (int t = 0; t < int(ThemeId::Count); ++t)
                if (ImGui::Selectable(themeName(ThemeId(t)), ThemeId(t) == theme_)) {
                    theme_ = ThemeId(t);
                    styleDirty_ = true;
                }
            ImGui::EndCombo();
        }
        ImGui::SliderFloat("UI scale", &pendingUiScale_, 0.75f, 2.5f, "%.2fx");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            uiScale_ = pendingUiScale_;
            styleDirty_ = true;
        }
        ImGui::TextDisabled("Monitor DPI scale: %.2fx", double(dpiScale_));
        ImGui::PopID();
    }

    // ---- Status --------------------------------------------------------------
    if (ImGui::CollapsingHeader("Status", open)) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("UI: %.1f FPS (%.2f ms)", double(io.Framerate), 1000.0 / double(std::max(io.Framerate, 1.0f)));
        formatRate(buf, sizeof(buf), measuredRate_);
        ImGui::Text("DSP throughput: %s", playing_ ? buf : "stopped");
        ImGui::Text("FFT lines: %.1f /s", measuredFftRate_);
        ImGui::Text("ADC peak: %.0f %%%s", double(dsp_.peakLevel() * 100.0f), dsp_.peakLevel() > 0.98f ? "  CLIP" : "");
        ImGui::Text("Noise floor: %.1f dBFS/bin", double(view_.noiseFloorDb()));
        ImGui::Text("Dropped (overrun): %llu bytes", static_cast<unsigned long long>(source_.overruns()));
    }
    ImGui::PopItemWidth();
}

void App::frame() {
    if (styleDirty_) {  // must happen outside Begin/End
        applyStyle();
        styleDirty_ = false;
    }
    updateStats();
    view_.feed(analyzer_);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 0.0f});
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);

    const float em = ImGui::GetFontSize();
    const float toolbarH = std::round(em * 3.1f);
    const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::BeginChild("##toolbar", {0.0f, toolbarH}, ImGuiChildFlags_AlwaysUseWindowPadding, fixed);
    drawToolbar(toolbarH);
    ImGui::EndChild();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 0.0f});
    const ImVec2 body = ImGui::GetContentRegionAvail();
    if (sidebarWidth_ <= 0.0f) sidebarWidth_ = std::clamp(vp->WorkSize.x * 0.25f, em * 18.0f, em * 28.0f);
    if (sidebarOpen_) {
        sidebarWidth_ = std::clamp(sidebarWidth_, em * 12.0f, std::max(em * 12.0f, body.x - em * 20.0f));
        ImGui::PopStyleVar();
        ImGui::BeginChild("##sidebar", {sidebarWidth_, body.y}, ImGuiChildFlags_AlwaysUseWindowPadding);
        drawSidebar();
        ImGui::EndChild();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 0.0f});

        // Draggable sidebar / viewport divider.
        ImGui::SameLine();
        const float gripW = std::max(4.0f, em * 0.3f);
        const ImVec2 gp = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##sidegrip", {gripW, body.y});
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive()) sidebarWidth_ += ImGui::GetIO().MouseDelta.x;
        ImGui::GetWindowDrawList()->AddRectFilled({gp.x + gripW * 0.5f - 1.0f, gp.y}, {gp.x + gripW * 0.5f + 1.0f, gp.y + body.y},
                                                  hot ? palette().accent : palette().grid);
        ImGui::SameLine();
    }
    ImGui::PopStyleVar();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {em * 0.25f, em * 0.25f});
    ImGui::BeginChild("##viewport", {0.0f, body.y}, ImGuiChildFlags_AlwaysUseWindowPadding, fixed);
    SpectrumViewIO io;
    io.centerHz = centerHz_;
    io.sampleRate = source_.sampleRate();
    io.vfo = &vfo_;
    view_.draw(io);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();

    if (io.centerRequested) setCenter(io.newCenterHz);
    if (io.tuneRequested) tune(io.tuneHz);

    // Space toggles play/stop when no widget wants the keyboard.
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false)) setPlaying(!playing_);
}

}  // namespace osx
