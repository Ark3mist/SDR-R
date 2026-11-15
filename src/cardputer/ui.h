// 320x170 keyboard-only front end.
//
//   y   0..15   status bar (mode badge, flags, 10-digit frequency, SNR, gain)
//   y  16..79   FFT spectrum (64 px)
//   y  80..169  waterfall (90 px)
//   TAB         215 px translucent drawer over the left of the viewport
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include "cardputer/input.h"
#include "cardputer/waterfall_sw.h"
#include "dsp/spectrum_data.h"
#include "engine/radio.h"
#include "gui/colormaps.h"
#include "gui/theme.h"

namespace osx {

class CardUi {
public:
    static constexpr int kW = 320, kH = 170, kBarH = 16, kFftH = 64, kWfH = 90, kViewH = kFftH + kWfH;
    static constexpr int kDrawerW = 215, kTabs = 7;

    explicit CardUi(Radio& radio);

    bool initFonts(const std::string& fontDir);  // call after ImGui::CreateContext()
    void onKey(const KeyEvent& k);
    void update();                               // drain FFT lines, paint the viewport pixels
    const uint32_t* pixels() const { return px_.data(); }   // kW x kViewH, ARGB8888
    void draw(float fps);                        // ImGui overlay (status bar, drawer, popups)
    void toast(const std::string& text, double seconds = 1.0);

    int fpsCap = 30;

private:
    struct Item {
        std::string label;
        std::function<std::string()> value;
        std::function<void(int)> adjust;              // Left / Right
        std::function<void()> activate;               // Enter
        std::function<std::string()> editGet;         // text entry (Enter starts editing)
        std::function<void(const std::string&)> editSet;
        std::function<bool()> visible;
    };
    using Page = std::vector<Item>;

    void buildMenus();
    Item& add(Page& p, const char* label);
    std::vector<int> visibleItems(int tab) const;
    void applyTheme();
    void rebuildLut();
    void paintFft(double loHz, double spanHz);
    void tuneStep(int dir);
    void cycleMode();
    void stepBandwidth(int dir);
    void stepGain(int dir);
    void stepSquelch(int dir);
    void autoRange();
    void commitEntry();
    void hotkey(const KeyEvent& k);
    template <typename F> void hw(F&& f) {
        if (radio_.sourceKind() == Radio::SourceKind::RtlTcp) f(radio_.tcp);
        else f(radio_.rtl);
    }

    Radio& radio_;
    SpectrumData spec_;
    WaterfallSW wf_{kWfH};
    std::vector<uint32_t> px_;
    std::vector<float> colAvg_, colPeak_;
    std::array<uint32_t, 256> lut_{};

    // display settings
    float fftMin_ = -100.0f, fftMax_ = -10.0f, wfMin_ = -90.0f, wfMax_ = -25.0f;
    bool peakHold_ = true;
    int zoomIdx_ = 0;
    ColormapId colormap_ = ColormapId::ClassicSdrpp;
    ThemeId theme_ = ThemeId::SdrppDark;

    // interaction state
    int digit_ = 5;                 // tuning step = 10^digit_ Hz
    bool drawerOpen_ = false;
    int tab_ = 0;
    std::array<int, kTabs> sel_{}, scroll_{};
    std::array<Page, kTabs> pages_;
    bool editing_ = false;
    std::string editBuf_;
    bool entryActive_ = false;
    std::string entry_;
    std::string toast_;
    double toastUntil_ = 0.0;
    double lastCenter_ = 0.0, lastRate_ = 0.0;
    std::string shownError_;
    float fps_ = 0.0f;

    ImFont* fontSmall_ = nullptr;
    ImFont* fontBig_ = nullptr;
};

}  // namespace osx
