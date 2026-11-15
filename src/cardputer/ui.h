// 320x170 keyboard-only front end for the Cardputer Zero.
//
//   y   0..15   status bar: mode badge + flags | 10-digit frequency | SNR, RF gain, volume
//   y  16..79   FFT spectrum (64 px): trace, peak hold, passband, squelch line, bookmark ticks
//   y  80..169  waterfall (90 px)
//   TAB         215 px translucent drawer over the left of the viewport
//
// Every key goes through onKey(). While a text box is open (frequency entry,
// bookmark rename, field edit) or a delete confirmation is pending, the box
// owns the keyboard: no single-key radio hotkey can fire ("focus lock").
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
#include "engine/bookmarks.h"
#include "engine/radio.h"
#include "gui/colormaps.h"
#include "gui/theme.h"

namespace osx {

class CardUi {
public:
    static constexpr int kW = 320, kH = 170, kBarH = 16, kFftH = 64, kWfH = 90, kViewH = kFftH + kWfH;
    static constexpr int kDrawerW = 215, kTabs = 7;
    enum Tab { kSource = 0, kRadio, kDsp, kBookmarks, kScanner, kRecNet, kDisplay };
    enum class Modal { None, FreqEntry, Rename, ConfirmDelete, FieldEdit };

    CardUi(Radio& radio, BookmarkStore& bookmarks);

    bool initFonts(const std::string& fontDir);  // call after ImGui::CreateContext()
    void onKey(const KeyEvent& k);
    void update();                               // drain FFT lines, paint the viewport pixels
    const uint32_t* pixels() const { return px_.data(); }   // kW x kViewH, ARGB8888
    void draw(float fps);                        // ImGui overlay (status bar, drawer, popups)
    void toast(const std::string& text, double seconds = 1.0);

    int fpsCap = 30;

    // Read-only state, for the tests.
    bool drawerOpen() const { return drawerOpen_; }
    int tab() const { return tab_; }
    Modal modal() const { return modal_; }
    const std::string& modalText() const { return text_; }
    int tuningDigit() const { return digit_; }
    int zoom() const { return zoomIdx_; }
    std::string selectedLabel() const;

private:
    struct Item {
        std::string label;
        std::function<std::string()> value;
        std::function<void(int)> adjust;              // Left / Right (also , and /)
        std::function<void()> activate;               // Enter
        std::function<std::string()> editGet;         // text field: Enter opens an edit box
        std::function<void(const std::string&)> editSet;
        std::function<bool()> visible;
        std::function<bool(const KeyEvent&)> key;     // row-specific keys (bookmarks: E, Backspace)
    };
    using Page = std::vector<Item>;

    void buildMenus();
    void buildBookmarkPage();
    Item& add(Page& p, const char* label);
    std::vector<int> visibleItems(int tab) const;
    Item* selectedItem();
    void applyTheme();
    void rebuildLut();
    void paintFft(double loHz, double spanHz);

    // actions
    void tuneStep(int dir);
    void cycleMode();
    void stepBandwidth(int dir);
    void stepGain(int dir);
    void stepSquelch(int dir);
    void stepVolume(int dir);
    void cycleNrPreset();
    void autoRange();
    void quickSave();
    void recallBookmark(size_t index);
    void commitFreqEntry();
    bool hotkey(const KeyEvent& k);
    bool modalKey(const KeyEvent& k);
    void setRx(const std::function<void(RxConfig&)>& f);
    void scanFiles();
    template <typename F> void hw(F&& f) {
        if (radio_.sourceKind() == Radio::SourceKind::RtlTcp) f(radio_.tcp);
        else f(radio_.rtl);
    }

    Radio& radio_;
    BookmarkStore& marks_;
    SpectrumData spec_;
    WaterfallSW wf_{kWfH};
    std::vector<uint32_t> px_;
    std::vector<float> colAvg_, colPeak_;
    std::array<uint32_t, 256> lut_{};

    // display settings
    float fftMin_ = -100.0f, fftMax_ = -10.0f, wfMin_ = -90.0f, wfMax_ = -25.0f;
    bool peakHold_ = true, showMarks_ = true;
    int zoomIdx_ = 0;
    ColormapId colormap_ = ColormapId::ClassicSdrpp;
    ThemeId theme_ = ThemeId::SdrppDark;

    // interaction state
    int digit_ = 3;                 // tuning step = 10^digit_ Hz, 1 (10 Hz) .. 5 (100 kHz)
    bool drawerOpen_ = false;
    int tab_ = kSource;
    std::array<int, kTabs> sel_{}, scroll_{};
    std::array<Page, kTabs> pages_;
    bool bookmarksDirty_ = true;
    int markFilter_ = -1;           // -1 = all categories
    Modal modal_ = Modal::None;
    std::string text_;              // text of the open box
    size_t modalTarget_ = 0;        // bookmark index for Rename / ConfirmDelete
    std::function<void(const std::string&)> fieldSet_;
    double volArmedUntil_ = 0.0, volOsdUntil_ = 0.0;
    std::string toast_;
    double toastUntil_ = 0.0;
    double lastCenter_ = 0.0, lastRate_ = 0.0;
    std::string shownError_;
    float fps_ = 0.0f;
    std::string fileDir_;
    std::vector<std::string> files_;

    ImFont* fontSmall_ = nullptr;
    ImFont* fontBig_ = nullptr;
};

}  // namespace osx
