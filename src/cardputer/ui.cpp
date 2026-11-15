#include "cardputer/ui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace osx {

namespace {

constexpr float kSmall = 10.0f, kBig = 13.0f;
constexpr int kRowH = 11, kTabH = 12;

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string fmt(const char* f, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

std::string onOff(bool b) { return b ? "ON" : "off"; }

std::string hzText(double hz) {
    if (hz >= 1e6) return fmt("%g MHz", hz / 1e6);
    if (hz >= 1e3) return fmt("%g kHz", hz / 1e3);
    return fmt("%g Hz", hz);
}

// ImU32 (ABGR, as ImGui packs it) -> ARGB8888 for the SDL pixel buffer.
uint32_t argb(ImU32 c) { return (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu); }

inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t a /*0..256*/) {
    const uint32_t rb = (((src & 0xFF00FFu) * a + (dst & 0xFF00FFu) * (256 - a)) >> 8) & 0xFF00FFu;
    const uint32_t g = (((src & 0x00FF00u) * a + (dst & 0x00FF00u) * (256 - a)) >> 8) & 0x00FF00u;
    return 0xFF000000u | rb | g;
}

double niceStep(double raw) {
    const double mag = std::pow(10.0, std::floor(std::log10(std::max(raw, 1e-9))));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (m * mag >= raw) return m * mag;
    return 10.0 * mag;
}

template <typename T, typename V>
int nearestIndex(const std::vector<T>& list, V v) {
    int best = 0;
    for (size_t i = 1; i < list.size(); ++i)
        if (std::fabs(double(list[i]) - double(v)) < std::fabs(double(list[size_t(best)]) - double(v))) best = int(i);
    return best;
}

const char* kTabNames[CardUi::kTabs] = {"1SRC", "2RAD", "3DSP", "4BKM", "5SCN", "6R/N", "7DIS"};
const char* kTabTitles[CardUi::kTabs] = {"SOURCE", "RADIO", "DSP / NR", "BOOKMARKS", "SCANNER", "REC / NET", "DISPLAY"};

}  // namespace

CardUi::CardUi(Radio& radio) : radio_(radio), px_(size_t(kW) * kViewH, 0xFF000000u) {
    buildMenus();
    rebuildLut();
    lastCenter_ = radio_.centerHz();
    lastRate_ = radio_.sampleRate();
}

bool CardUi::initFonts(const std::string& fontDir) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    fontSmall_ = io.Fonts->AddFontFromFileTTF((fontDir + "/ProggyTiny.ttf").c_str(), kSmall, &cfg);
    fontBig_ = io.Fonts->AddFontFromFileTTF((fontDir + "/ProggyClean.ttf").c_str(), kBig, &cfg);
    const bool ok = fontSmall_ && fontBig_;
    if (!fontSmall_) fontSmall_ = io.Fonts->AddFontDefault();
    if (!fontBig_) fontBig_ = fontSmall_;
    applyTheme();
    return ok;
}

void CardUi::applyTheme() {
    osx::applyTheme(theme_, 1.0f);
    ImGuiStyle& st = ImGui::GetStyle();
    st.ItemSpacing = {2.0f, 2.0f};
    st.FramePadding = {3.0f, 1.0f};
    st.FontSizeBase = kSmall;
    st.AntiAliasedLines = false;        // crisp 1-px geometry, and cheaper to rasterise
    st.AntiAliasedLinesUseTex = false;
    st.AntiAliasedFill = false;
}

void CardUi::rebuildLut() {
    uint8_t rgb[256 * 3];
    buildColormap(colormap_, rgb);
    for (int i = 0; i < 256; ++i)
        lut_[size_t(i)] = 0xFF000000u | (uint32_t(rgb[3 * i]) << 16) | (uint32_t(rgb[3 * i + 1]) << 8) | rgb[3 * i + 2];
}

void CardUi::toast(const std::string& text, double seconds) {
    toast_ = text;
    toastUntil_ = nowSeconds() + seconds;
}

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------

CardUi::Item& CardUi::add(Page& p, const char* label) {
    p.emplace_back();
    p.back().label = label;
    return p.back();
}

void CardUi::buildMenus() {
    using SK = Radio::SourceKind;
    auto isRtl = [this] { return radio_.sourceKind() == SK::RtlSdr; };
    auto isTcp = [this] { return radio_.sourceKind() == SK::RtlTcp; };
    auto isHw = [this] { return radio_.sourceKind() != SK::Simulated; };
    auto isSim = [this] { return radio_.sourceKind() == SK::Simulated; };
    auto toggle = [](Item& it, std::function<bool()> get, std::function<void(bool)> set) {
        it.value = [get] { return onOff(get()); };
        it.adjust = [get, set](int) { set(!get()); };
        it.activate = [get, set] { set(!get()); };
    };

    // ---- [1] SOURCE ---------------------------------------------------------
    Page& src = pages_[0];
    toggle(add(src, "Run"), [this] { return radio_.playing(); }, [this](bool on) {
        if (!on) radio_.stop();
        else if (!radio_.play()) toast(radio_.lastError(), 3.0);
    });
    {
        Item& it = add(src, "Device");
        it.value = [this] { return std::string(Radio::sourceKindName(radio_.sourceKind())); };
        it.adjust = [this](int d) {
            const int n = int(SK::Count);
            radio_.selectSource(SK((int(radio_.sourceKind()) + d + n) % n));
            sel_[0] = 1;
            if (!radio_.lastError().empty() && !radio_.playing()) toast(radio_.lastError(), 3.0);
        };
    }
    {
        Item& it = add(src, "Sample rate");
        it.value = [this] { return fmt("%.3f MSPS", radio_.sampleRate() / 1e6); };
        it.adjust = [this](int d) {
            const auto rates = radio_.source().sampleRates();
            const int i = std::clamp(nearestIndex(rates, radio_.sampleRate()) + d, 0, int(rates.size()) - 1);
            radio_.setSampleRate(rates[size_t(i)]);
        };
    }
    {
        Item& it = add(src, "RF gain");
        it.value = [this] { return fmt("%.1f dB", double(radio_.source().gain())); };
        it.adjust = [this](int d) { stepGain(d); };
    }
    {
        Item& it = add(src, "RTL device #");
        it.visible = isRtl;
        it.value = [this] { return fmt("%d of %d", radio_.rtl.deviceIndex, int(RtlSdrSource::listDevices().size())); };
        it.adjust = [this](int d) {
            const int n = std::max(1, int(RtlSdrSource::listDevices().size()));
            const int idx = std::clamp(radio_.rtl.deviceIndex + d, 0, n - 1);
            if (idx == radio_.rtl.deviceIndex) return;
            const bool was = radio_.playing();
            radio_.stop();
            radio_.rtl.deviceIndex = idx;
            if (was && !radio_.play()) toast(radio_.lastError(), 3.0);
        };
    }
    {
        Item& it = add(src, "Host");
        it.visible = isTcp;
        it.value = [this] { return radio_.tcp.host; };
        it.editGet = [this] { return radio_.tcp.host; };
        it.editSet = [this](const std::string& s) { if (!s.empty()) radio_.tcp.host = s; };
    }
    {
        Item& it = add(src, "Port");
        it.visible = isTcp;
        it.value = [this] { return fmt("%d", radio_.tcp.port); };
        it.editGet = [this] { return fmt("%d", radio_.tcp.port); };
        it.editSet = [this](const std::string& s) { radio_.tcp.port = std::clamp(std::atoi(s.c_str()), 1, 65535); };
        it.adjust = [this](int d) { radio_.tcp.port = std::clamp(radio_.tcp.port + d, 1, 65535); };
    }
    {
        Item& it = add(src, "Net buffer");
        it.visible = isTcp;
        it.value = [this] { return fmt("%d kB", radio_.tcp.bufferKb); };
        it.adjust = [this](int d) { radio_.tcp.bufferKb = std::clamp(d > 0 ? radio_.tcp.bufferKb * 2 : radio_.tcp.bufferKb / 2, 4, 256); };
    }
    {
        Item& it = add(src, "Direct sampl.");
        it.visible = isHw;
        it.value = [this] {
            int m = 0;
            hw([&](auto& s) { m = s.directSampling(); });
            return std::string(m == 0 ? "off" : (m == 1 ? "I-ADC" : "Q-ADC"));
        };
        it.adjust = [this](int d) { hw([&](auto& s) { s.setDirectSampling((s.directSampling() + d + 3) % 3); }); };
    }
    auto hwToggle = [&](const char* label, auto get, auto set) {
        Item& it = add(src, label);
        it.visible = isHw;
        toggle(it, [this, get] { bool v = false; hw([&](auto& s) { v = get(s); }); return v; },
               [this, set](bool on) { hw([&](auto& s) { set(s, on); }); });
    };
    hwToggle("Offset tuning", [](auto& s) { return s.offsetTuning(); }, [](auto& s, bool on) { s.setOffsetTuning(on); });
    hwToggle("RTL AGC", [](auto& s) { return s.rtlAgc(); }, [](auto& s, bool on) { s.setRtlAgc(on); });
    hwToggle("Tuner AGC", [](auto& s) { return s.tunerAgc(); }, [](auto& s, bool on) { s.setTunerAgc(on); });
    {
        Item& it = add(src, "PPM corr.");
        it.visible = isHw;
        it.value = [this] { int p = 0; hw([&](auto& s) { p = s.ppm(); }); return fmt("%+d ppm", p); };
        it.adjust = [this](int d) { hw([&](auto& s) { s.setPpm(s.ppm() + d); }); };
    }
    hwToggle("Bias-T", [](auto& s) { return s.biasTee(); }, [](auto& s, bool on) { s.setBiasTee(on); });
    {
        Item& it = add(src, "Sim noise");
        it.visible = isSim;
        it.value = [this] { return fmt("%.0f dBFS", double(radio_.sim.noiseDb.load())); };
        it.adjust = [this](int d) { radio_.sim.noiseDb = std::clamp(radio_.sim.noiseDb.load() + 2.0f * float(d), -90.0f, -10.0f); };
    }
    {
        Item& it = add(src, "Sim DC offset");
        it.visible = isSim;
        it.value = [this] { return fmt("%.1f %%FS", double(radio_.sim.dcOffset.load()) * 100.0); };
        it.adjust = [this](int d) { radio_.sim.dcOffset = std::clamp(radio_.sim.dcOffset.load() + 0.002f * float(d), 0.0f, 0.05f); };
    }
    {
        Item& it = add(src, "Sim emitters");
        it.visible = isSim;
        toggle(it, [this] { return radio_.sim.emittersEnabled.load(); }, [this](bool on) { radio_.sim.emittersEnabled = on; });
    }
    add(src, "ADC peak").value = [this] { return fmt("%.0f %%%s", double(radio_.dsp.peakLevel()) * 100.0, radio_.dsp.peakLevel() > 0.98f ? " CLIP" : ""); };
    add(src, "Overruns").value = [this] { return fmt("%llu B", static_cast<unsigned long long>(radio_.source().overruns())); };

    // ---- [2] RADIO ----------------------------------------------------------
    Page& rad = pages_[1];
    auto modeIs = [this](std::initializer_list<Demod> ms) {
        return std::find(ms.begin(), ms.end(), radio_.rx().mode) != ms.end();
    };
    auto setRx = [this](const std::function<void(RxConfig&)>& f) {
        RxConfig c = radio_.rx();
        f(c);
        radio_.setRx(c);
    };
    {
        Item& it = add(rad, "Mode");
        it.value = [this] { return std::string(demodName(radio_.rx().mode)); };
        it.adjust = [this](int d) {
            const int n = int(Demod::Count);
            radio_.setMode(Demod((int(radio_.rx().mode) + d + n) % n));
        };
    }
    {
        Item& it = add(rad, "Bandwidth");
        it.value = [this] { return hzText(radio_.rx().bandwidthHz); };
        it.adjust = [this](int d) { stepBandwidth(d); };
    }
    {
        static const std::vector<double> snaps = {1, 10, 100, 500, 1e3, 2.5e3, 5e3, 6.25e3, 8.33e3, 9e3, 10e3, 12.5e3, 25e3, 50e3, 100e3, 200e3};
        Item& it = add(rad, "Snap step");
        it.value = [this] { return hzText(radio_.vfo.snapHz); };
        it.adjust = [this](int d) {
            const int i = std::clamp(nearestIndex(snaps, radio_.vfo.snapHz) + d, 0, int(snaps.size()) - 1);
            radio_.vfo.snapHz = snaps[size_t(i)];
        };
    }
    {
        Item& it = add(rad, "Tuning mode");
        it.value = [this] { return std::string(radio_.tuningMode == Radio::TuningMode::Normal ? "Normal" : "Centre"); };
        it.adjust = [this](int) {
            radio_.tuningMode = radio_.tuningMode == Radio::TuningMode::Normal ? Radio::TuningMode::Center : Radio::TuningMode::Normal;
            radio_.tune(radio_.vfoHz());
        };
    }
    {
        Item& it = add(rad, "Volume");
        it.value = [this] { return fmt("%.0f %%", double(radio_.audio.volume.load()) * 100.0); };
        it.adjust = [this](int d) { radio_.audio.volume = std::clamp(radio_.audio.volume.load() + 0.05f * float(d), 0.0f, 1.0f); };
    }
    toggle(add(rad, "Mute"), [this] { return radio_.audio.mute.load(); }, [this](bool on) { radio_.audio.mute = on; });
    toggle(add(rad, "Squelch"), [this] { return radio_.rx().squelch; }, [setRx](bool on) { setRx([on](RxConfig& c) { c.squelch = on; }); });
    {
        Item& it = add(rad, "Squelch level");
        it.value = [this] { return fmt("%.0f dBFS", double(radio_.rx().squelchDb)); };
        it.adjust = [setRx](int d) { setRx([d](RxConfig& c) { c.squelchDb = std::clamp(c.squelchDb + float(d), -120.0f, 0.0f); }); };
    }
    {
        Item& it = add(rad, "Stereo");
        it.visible = [modeIs] { return modeIs({Demod::WFM}); };
        toggle(it, [this] { return radio_.rx().stereo; }, [setRx](bool on) { setRx([on](RxConfig& c) { c.stereo = on; }); });
    }
    {
        Item& it = add(rad, "De-emphasis");
        it.visible = [modeIs] { return modeIs({Demod::WFM, Demod::NFM}); };
        it.value = [this] {
            static const char* names[] = {"none", "50 us", "75 us"};
            return std::string(names[std::clamp(radio_.rx().deemphasis, 0, 2)]);
        };
        it.adjust = [setRx](int d) { setRx([d](RxConfig& c) { c.deemphasis = (c.deemphasis + d + 3) % 3; }); };
    }
    {
        Item& it = add(rad, "CTCSS detect");
        it.visible = [modeIs] { return modeIs({Demod::NFM}); };
        toggle(it, [this] { return radio_.rx().ctcss; }, [setRx](bool on) { setRx([on](RxConfig& c) { c.ctcss = on; }); });
    }
    {
        Item& it = add(rad, "AGC");
        it.visible = [modeIs] { return modeIs({Demod::AM, Demod::SAM, Demod::USB, Demod::LSB, Demod::CW}); };
        it.value = [this] {
            static const char* names[] = {"Fast", "Medium", "Slow"};
            return std::string(names[std::clamp(radio_.rx().agc, 0, 2)]);
        };
        it.adjust = [setRx](int d) { setRx([d](RxConfig& c) { c.agc = (c.agc + d + 3) % 3; }); };
    }
    {
        Item& it = add(rad, "CW pitch");
        it.visible = [modeIs] { return modeIs({Demod::CW}); };
        it.value = [this] { return fmt("%.0f Hz", double(radio_.rx().cwPitchHz)); };
        it.adjust = [setRx](int d) { setRx([d](RxConfig& c) { c.cwPitchHz = std::clamp(c.cwPitchHz + 50.0f * float(d), 200.0f, 1200.0f); }); };
    }
    add(rad, "Channel level").value = [this] { return fmt("%.1f dBFS", double(radio_.receiver.levelDb.load())); };
    {
        Item& it = add(rad, "Pilot lock");
        it.visible = [modeIs] { return modeIs({Demod::WFM}); };
        it.value = [this] { return std::string(radio_.receiver.stereoLocked.load() ? "STEREO" : "mono"); };
    }
    {
        Item& it = add(rad, "CTCSS tone");
        it.visible = [modeIs] { return modeIs({Demod::NFM}); };
        it.value = [this] { const float t = radio_.receiver.ctcssHz.load(); return t > 0.0f ? fmt("%.1f Hz", double(t)) : std::string("-"); };
    }
    add(rad, "Audio out").value = [this] { return radio_.audio.deviceOpen() ? radio_.audio.status() : std::string("NO DEVICE"); };
    add(rad, "Out peak").value = [this] { return fmt("%.1f dBFS", 20.0 * std::log10(double(radio_.audio.outputPeak()) + 1e-6)); };

    // ---- [3] DSP / NR -------------------------------------------------------
    Page& dsp = pages_[2];
    toggle(add(dsp, "DC removal"), [this] { return radio_.dsp.dcRemoval.load(); }, [this](bool on) { radio_.dsp.dcRemoval = on; });
    toggle(add(dsp, "Invert IQ"), [this] { return radio_.dsp.invertIq.load(); }, [this](bool on) { radio_.dsp.invertIq = on; spec_.resetPeak(); });

    // ---- [7] DISPLAY --------------------------------------------------------
    Page& dis = pages_[6];
    {
        Item& it = add(dis, "FFT size");
        it.value = [this] { return fmt("%d", radio_.analyzer.fftSize()); };
        it.adjust = [this](int d) { radio_.analyzer.setFftSize(std::clamp(d > 0 ? radio_.analyzer.fftSize() * 2 : radio_.analyzer.fftSize() / 2, 1024, 65536)); };
    }
    {
        Item& it = add(dis, "FFT rate");
        it.value = [this] { return fmt("%d /s", radio_.analyzer.rate()); };
        it.adjust = [this](int d) { radio_.analyzer.setRate(std::clamp(radio_.analyzer.rate() + 5 * d, 5, 60)); };
    }
    {
        Item& it = add(dis, "UI frame cap");
        it.value = [this] { return fmt("%d FPS", fpsCap); };
        it.adjust = [this](int d) { fpsCap = std::clamp(fpsCap + 5 * d, 10, 60); };
    }
    {
        Item& it = add(dis, "FFT window");
        it.value = [this] { return std::string(fftWindowName(radio_.analyzer.window())); };
        it.adjust = [this](int d) {
            const int n = int(FftWindow::Count);
            radio_.analyzer.setWindow(FftWindow((int(radio_.analyzer.window()) + d + n) % n));
        };
    }
    {
        Item& it = add(dis, "Smoothing");
        it.value = [this] { return fmt("%.2f", double(spec_.smoothing)); };
        it.adjust = [this](int d) { spec_.smoothing = std::clamp(spec_.smoothing + 0.05f * float(d), 0.0f, 0.95f); };
    }
    toggle(add(dis, "Peak hold"), [this] { return peakHold_; }, [this](bool on) { peakHold_ = on; spec_.resetPeak(); });
    {
        Item& it = add(dis, "Reset peak");
        it.value = [] { return std::string("[Enter]"); };
        it.activate = [this] { spec_.resetPeak(); };
    }
    auto level = [&](const char* label, float CardUi::*member, float lo, float hi) {
        Item& it = add(dis, label);
        it.value = [this, member] { return fmt("%.0f dB", double(this->*member)); };
        it.adjust = [this, member, lo, hi](int d) { this->*member = std::clamp(this->*member + 5.0f * float(d), lo, hi); };
    };
    level("FFT max", &CardUi::fftMax_, -100.0f, 20.0f);
    level("FFT min", &CardUi::fftMin_, -160.0f, -20.0f);
    level("Waterfall max", &CardUi::wfMax_, -100.0f, 20.0f);
    level("Waterfall min", &CardUi::wfMin_, -160.0f, -20.0f);
    {
        Item& it = add(dis, "Auto range");
        it.value = [] { return std::string("[Enter]"); };
        it.activate = [this] { autoRange(); };
    }
    {
        Item& it = add(dis, "Colormap");
        it.value = [this] { return std::string(colormapName(colormap_)); };
        it.adjust = [this](int d) {
            const int n = int(ColormapId::Count);
            colormap_ = ColormapId((int(colormap_) + d + n) % n);
            rebuildLut();
        };
    }
    {
        Item& it = add(dis, "Zoom");
        it.value = [this] { return fmt("%dx", 1 << zoomIdx_); };
        it.adjust = [this](int d) { zoomIdx_ = (zoomIdx_ + d + 3) % 3; };
    }
    {
        Item& it = add(dis, "Theme");
        it.value = [this] { return std::string(themeName(theme_)); };
        it.adjust = [this](int d) {
            const int n = int(ThemeId::Count);
            theme_ = ThemeId((int(theme_) + d + n) % n);
            applyTheme();
        };
    }
    add(dis, "UI rate").value = [this] { return fmt("%.1f FPS", double(fps_)); };
    add(dis, "Audio underruns").value = [this] { return fmt("%llu", static_cast<unsigned long long>(radio_.audio.underruns())); };
    add(dis, "Mixer").value = [this] {
        const std::string& r = radio_.mixerReport;
        return r.empty() ? std::string("-") : r.substr(r.find(':') == std::string::npos ? 0 : r.find(':') + 2);
    };
}

std::vector<int> CardUi::visibleItems(int tab) const {
    std::vector<int> out;
    const Page& p = pages_[size_t(tab)];
    for (size_t i = 0; i < p.size(); ++i)
        if (!p[i].visible || p[i].visible()) out.push_back(int(i));
    return out;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void CardUi::tuneStep(int dir) {
    radio_.tune(radio_.vfoHz() + double(dir) * std::pow(10.0, digit_));
}

void CardUi::cycleMode() {
    radio_.setMode(Demod((int(radio_.rx().mode) + 1) % int(Demod::Count)));
    toast(fmt("MODE %s  BW %s", demodName(radio_.rx().mode), hzText(radio_.rx().bandwidthHz).c_str()));
}

void CardUi::stepBandwidth(int dir) {
    const DemodLimits& lim = demodLimits(radio_.rx().mode);
    const double bw = std::round((radio_.rx().bandwidthHz + double(dir) * lim.bwStep) / lim.bwStep) * lim.bwStep;
    radio_.setBandwidth(std::clamp(bw, lim.minBw, lim.maxBw));
}

void CardUi::stepGain(int dir) {
    Source& s = radio_.source();
    const auto steps = s.gainSteps();
    if (steps.empty()) return;
    const int i = std::clamp(nearestIndex(steps, s.gain()) + dir, 0, int(steps.size()) - 1);
    s.setGain(steps[size_t(i)]);
}

void CardUi::stepSquelch(int dir) {
    RxConfig c = radio_.rx();
    if (!c.squelch) {
        c.squelch = true;  // first press arms the squelch just above the current channel level
        c.squelchDb = std::round(radio_.receiver.levelDb.load()) + (dir > 0 ? 3.0f : -3.0f);
    } else {
        c.squelchDb += 2.0f * float(dir);
    }
    c.squelchDb = std::clamp(c.squelchDb, -120.0f, 0.0f);
    radio_.setRx(c);
    toast(fmt("SQUELCH %.0f dBFS", double(c.squelchDb)));
}

void CardUi::autoRange() {
    if (!spec_.hasData()) return;
    fftMin_ = std::floor((spec_.noiseFloorDb - 12.0f) / 5.0f) * 5.0f;
    fftMax_ = std::min(10.0f, std::ceil((spec_.maxDb + 8.0f) / 5.0f) * 5.0f);
    if (fftMax_ - fftMin_ < 30.0f) fftMax_ = fftMin_ + 30.0f;
    wfMin_ = spec_.noiseFloorDb - 3.0f;
    wfMax_ = std::max(wfMin_ + 25.0f, spec_.maxDb + 3.0f);
    toast(fmt("AUTO %.0f..%.0f dB", double(fftMin_), double(fftMax_)));
}

void CardUi::commitEntry() {
    entryActive_ = false;
    if (entry_.empty() || entry_ == ".") return;
    const double mhz = std::atof(entry_.c_str());  // always MHz: "121.5" -> 121 500 000 Hz
    radio_.tune(std::round(mhz * 1e6));
    toast(fmt("TUNED %.6f MHz", radio_.vfoHz() / 1e6));
}

void CardUi::hotkey(const KeyEvent& k) {
    if (k.key == Key::Space) {
        radio_.audio.mute = !radio_.audio.mute.load();
        toast(radio_.audio.mute.load() ? "MUTE" : "UNMUTE");
        return;
    }
    if (k.key != Key::Char) return;
    const int dir = k.shift ? -1 : 1;
    switch (k.ch) {
        case 'd': cycleMode(); break;
        case 'b':
            stepBandwidth(dir);
            toast("BW " + hzText(radio_.rx().bandwidthHz));
            break;
        case 'g': {
            bool agc = false;
            if (radio_.sourceKind() != Radio::SourceKind::Simulated) hw([&](auto& s) { agc = s.tunerAgc(); });
            if (agc) { toast("TUNER AGC ON"); break; }
            stepGain(dir);
            toast(fmt("RF GAIN %.1f dB", double(radio_.source().gain())));
            break;
        }
        case 's': stepSquelch(dir); break;
        case 'a': autoRange(); break;
        case 'z':
            zoomIdx_ = (zoomIdx_ + 1) % 3;
            toast(fmt("ZOOM %dx", 1 << zoomIdx_));
            break;
        case 'p':
            if (radio_.playing()) { radio_.stop(); toast("STOP"); }
            else if (radio_.play()) toast("RUN");
            else toast(radio_.lastError(), 3.0);
            break;
        case 'r': toast("RECORDER NOT BUILT YET", 1.5); break;
        case 'l': toast("SCANNER NOT BUILT YET", 1.5); break;
        default: break;
    }
}

void CardUi::onKey(const KeyEvent& k) {
    if (k.key == Key::None) return;
    const bool isDigit = k.key == Key::Char && k.ch >= '0' && k.ch <= '9';
    const bool isEscape = k.key == Key::Esc || (k.key == Key::Char && k.ch == '`');

    // 1. Direct frequency entry popup.
    if (entryActive_) {
        if (isDigit && entry_.size() < 11) entry_ += k.ch;
        else if (k.key == Key::Char && k.ch == '.' && entry_.find('.') == std::string::npos) entry_ += '.';
        else if (k.key == Key::Backspace) { if (!entry_.empty()) entry_.pop_back(); }
        else if (k.key == Key::Enter) commitEntry();
        else if (isEscape) entryActive_ = false;
        return;
    }
    // 2. Text field being edited inside the drawer.
    if (editing_) {
        const auto vis = visibleItems(tab_);
        if (k.key == Key::Enter) {
            if (!vis.empty()) {
                Item& it = pages_[size_t(tab_)][size_t(vis[size_t(std::min(sel_[size_t(tab_)], int(vis.size()) - 1))])];
                if (it.editSet) it.editSet(editBuf_);
            }
            editing_ = false;
        } else if (k.key == Key::Esc) editing_ = false;
        else if (k.key == Key::Backspace) { if (!editBuf_.empty()) editBuf_.pop_back(); }
        else if (k.key == Key::Char && editBuf_.size() < 40) editBuf_ += k.ch;
        return;
    }
    if (k.key == Key::Tab) { drawerOpen_ = !drawerOpen_; return; }
    if (isEscape) { drawerOpen_ = false; return; }

    if (drawerOpen_) {
        const auto vis = visibleItems(tab_);
        int& sel = sel_[size_t(tab_)];
        const int n = int(vis.size());
        if (n > 0) sel = std::clamp(sel, 0, n - 1);
        Item* it = n > 0 ? &pages_[size_t(tab_)][size_t(vis[size_t(sel)])] : nullptr;
        switch (k.key) {
            case Key::Up: if (n > 0) sel = (sel + n - 1) % n; return;
            case Key::Down: if (n > 0) sel = (sel + 1) % n; return;
            case Key::Left: if (it && it->adjust) it->adjust(-1); return;
            case Key::Right: if (it && it->adjust) it->adjust(+1); return;
            case Key::Enter:
                if (!it) return;
                if (it->editSet) { editing_ = true; editBuf_ = it->editGet ? it->editGet() : std::string(); }
                else if (it->activate) it->activate();
                else if (it->adjust) it->adjust(+1);
                return;
            default: break;
        }
        if (isDigit && k.ch >= '1' && k.ch <= '0' + kTabs) { tab_ = k.ch - '1'; return; }
        hotkey(k);
        return;
    }

    switch (k.key) {
        case Key::Left: tuneStep(-1); return;
        case Key::Right: tuneStep(+1); return;
        case Key::Up: digit_ = std::min(digit_ + 1, 9); return;
        case Key::Down: digit_ = std::max(digit_ - 1, 0); return;
        default: break;
    }
    if (isDigit || (k.key == Key::Char && k.ch == '.')) {
        entryActive_ = true;
        entry_ = std::string(1, k.ch);
        return;
    }
    hotkey(k);
}

// ---------------------------------------------------------------------------
// Viewport pixels (FFT + waterfall), painted straight into the ARGB buffer
// ---------------------------------------------------------------------------

void CardUi::update() {
    radio_.poll();
    if (radio_.lastError() != shownError_) {
        shownError_ = radio_.lastError();
        if (!shownError_.empty()) toast(shownError_, 4.0);
    }
    const double sr = radio_.sampleRate();
    if (sr != lastRate_) {  // history bins no longer line up
        spec_.clear();
        lastRate_ = sr;
    }
    if (radio_.centerHz() != lastCenter_) {
        spec_.resetPeak();
        lastCenter_ = radio_.centerHz();
    }
    spec_.feed(radio_.analyzer, [&](const std::vector<float>& line) {
        wf_.push(line.data(), line.size(), radio_.centerHz(), sr);
    });
    spec_.measure(radio_.vfo.offsetHz, radio_.vfo.bandwidthHz, sr);

    // View window: zoom is centred on the VFO, clamped inside the captured band.
    const double span = sr / double(1 << zoomIdx_);
    const double lim = (sr - span) * 0.5;
    const double lo = std::clamp(radio_.vfo.offsetHz, -lim, lim) - span * 0.5;

    paintFft(lo, span);
    wf_.render(px_.data() + size_t(kFftH) * kW, kW, kW, kWfH, radio_.centerHz() + lo, span, wfMin_, wfMax_, lut_.data());

    // VFO markers on the waterfall.
    const Palette& pal = palette();
    const auto xOf = [&](double offHz) { return int(std::floor((offHz - lo) / span * kW)); };
    const int xc = xOf(radio_.vfo.offsetHz);
    const int xl = xOf(radio_.vfo.offsetHz - radio_.vfo.bandwidthHz * 0.5), xr = xOf(radio_.vfo.offsetHz + radio_.vfo.bandwidthHz * 0.5);
    for (int y = kFftH; y < kViewH; ++y) {
        uint32_t* row = &px_[size_t(y) * kW];
        if (xl >= 0 && xl < kW) row[xl] = blend(row[xl], 0xFFFFFFu, 70);
        if (xr >= 0 && xr < kW && xr != xl) row[xr] = blend(row[xr], 0xFFFFFFu, 70);
        if (xc >= 0 && xc < kW) row[xc] = blend(row[xc], argb(pal.vfoCenter), 150);
    }
}

void CardUi::paintFft(double loHz, double spanHz) {
    const Palette& pal = palette();
    const uint32_t bg = argb(pal.plotBg), grid = argb(pal.grid), trace = argb(pal.trace);
    const uint32_t fill = argb(pal.accent), peakCol = argb(pal.peakHold), vfoLine = argb(pal.vfoCenter);
    const double sr = radio_.sampleRate();
    const float range = std::max(fftMax_ - fftMin_, 1.0f);
    auto yOf = [&](float db) { return int(std::lround(float(kFftH - 1) - (db - fftMin_) / range * float(kFftH - 1))); };
    auto xOf = [&](double offHz) { return int(std::floor((offHz - loHz) / spanHz * kW)); };

    std::fill(px_.begin(), px_.begin() + size_t(kFftH) * kW, bg);

    // Grid: dotted dB lines, solid frequency lines at "nice" absolute frequencies.
    const float dbStep = range > 80.0f ? 20.0f : 10.0f;
    for (float db = std::ceil(fftMin_ / dbStep) * dbStep; db < fftMax_; db += dbStep) {
        const int y = yOf(db);
        if (y <= 0 || y >= kFftH) continue;
        for (int x = 0; x < kW; x += 2) px_[size_t(y) * kW + size_t(x)] = grid;
    }
    const double fStep = niceStep(spanHz / 4.0);
    const double absLo = radio_.centerHz() + loHz;
    for (double f = std::ceil(absLo / fStep) * fStep; f < absLo + spanHz; f += fStep) {
        const int x = xOf(f - radio_.centerHz());
        if (x < 0 || x >= kW) continue;
        for (int y = 0; y < kFftH; ++y) px_[size_t(y) * kW + size_t(x)] = grid;
    }

    // VFO passband highlight box.
    const int xl = std::clamp(xOf(radio_.vfo.offsetHz - radio_.vfo.bandwidthHz * 0.5), 0, kW - 1);
    const int xr = std::clamp(xOf(radio_.vfo.offsetHz + radio_.vfo.bandwidthHz * 0.5), 0, kW - 1);
    const uint32_t vfoCol = argb(radio_.vfo.color) & 0xFFFFFFu;
    for (int y = 0; y < kFftH; ++y)
        for (int x = xl; x <= xr; ++x) {
            uint32_t& p = px_[size_t(y) * kW + size_t(x)];
            p = blend(p, vfoCol, (x == xl || x == xr) ? 110 : 40);
        }

    if (spec_.hasData()) {
        SpectrumData::reduce(spec_.avg(), colAvg_, kW, loHz, spanHz, sr);
        int prevY = -1;
        for (int x = 0; x < kW; ++x) {
            const int y = std::clamp(yOf(colAvg_[size_t(x)]), 0, kFftH - 1);
            for (int yy = y + 1; yy < kFftH; ++yy) {  // translucent fill under the trace
                uint32_t& p = px_[size_t(yy) * kW + size_t(x)];
                p = blend(p, fill, 60);
            }
            const int y0 = prevY < 0 ? y : std::min(prevY, y), y1 = prevY < 0 ? y : std::max(prevY, y);
            for (int yy = y0; yy <= y1; ++yy) px_[size_t(yy) * kW + size_t(x)] = trace;  // connected trace
            prevY = y;
        }
        if (peakHold_ && spec_.peak().size() == spec_.avg().size()) {
            SpectrumData::reduce(spec_.peak(), colPeak_, kW, loHz, spanHz, sr);
            for (int x = 0; x < kW; ++x) {
                const int y = yOf(colPeak_[size_t(x)]);
                if (y >= 0 && y < kFftH) px_[size_t(y) * kW + size_t(x)] = peakCol;
            }
        }
    }

    // Squelch threshold (dashed) across the passband region and beyond.
    if (radio_.rx().squelch) {
        const int y = yOf(radio_.rx().squelchDb);
        if (y >= 0 && y < kFftH)
            for (int x = 0; x < kW; ++x)
                if ((x / 3) % 2 == 0) px_[size_t(y) * kW + size_t(x)] = radio_.receiver.squelchOpen.load() ? 0xFF40D060u : 0xFFE0B030u;
    }
    const int xc = xOf(radio_.vfo.offsetHz);
    if (xc >= 0 && xc < kW)
        for (int y = 0; y < kFftH; ++y) px_[size_t(y) * kW + size_t(xc)] = vfoLine;
}

// ---------------------------------------------------------------------------
// Overlay (text + drawer), drawn with ImGui draw lists
// ---------------------------------------------------------------------------

void CardUi::draw(float fps) {
    fps_ = fps;
    const Palette& pal = palette();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImU32 colBg = ImGui::GetColorU32(ImGuiCol_WindowBg), colText = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 colDim = ImGui::GetColorU32(ImGuiCol_TextDisabled), colHdr = ImGui::GetColorU32(ImGuiCol_Header);
    auto small = [&](float x, float y, ImU32 col, const std::string& s) {
        dl->AddText(fontSmall_, kSmall, {std::floor(x), std::floor(y)}, col, s.c_str());
    };
    auto smallW = [&](const std::string& s) { return fontSmall_->CalcTextSizeA(kSmall, FLT_MAX, 0.0f, s.c_str()).x; };
    const double now = nowSeconds();

    // ---- status bar ---------------------------------------------------------
    dl->AddRectFilled({0, 0}, {float(kW), float(kBarH)}, colBg);
    float x = 1.0f;
    {
        const std::string mode = demodName(radio_.rx().mode);
        const float w = smallW(mode) + 5.0f;
        dl->AddRectFilled({x, 2}, {x + w, 14}, pal.accent, 2.0f);
        small(x + 3.0f, 3.0f, IM_COL32(255, 255, 255, 255), mode);
        x += w + 3.0f;
    }
    auto flag = [&](const char* s, ImU32 col) {
        small(x, 3.0f, col, s);
        x += smallW(s) + 3.0f;
    };
    if (!radio_.playing()) flag("STOP", colDim);
    if (radio_.audio.mute.load()) flag("MUTE", IM_COL32(255, 80, 70, 255));
    if (radio_.playing() && radio_.rx().squelch && !radio_.receiver.squelchOpen.load()) flag("SQ", IM_COL32(230, 180, 50, 255));
    if (radio_.playing() && radio_.rx().mode == Demod::WFM && radio_.receiver.stereoLocked.load()) flag("ST", IM_COL32(80, 220, 110, 255));

    {   // 10-digit frequency, leading zeros dimmed, underline on the tuning digit
        const auto hz = int64_t(std::llround(radio_.vfoHz()));
        char digits[11];
        std::snprintf(digits, sizeof(digits), "%010lld", static_cast<long long>(std::clamp<int64_t>(hz, 0, 9'999'999'999ll)));
        const float cw = fontBig_->CalcTextSizeA(kBig, FLT_MAX, 0.0f, "0").x;
        const float total = cw * 13.0f;
        float fx = std::floor((float(kW) - total) * 0.5f) + 6.0f;
        bool leading = true;
        for (int i = 0; i < 10; ++i) {
            if (digits[i] != '0' || i == 9) leading = false;
            const char s[2] = {digits[i], 0};
            dl->AddText(fontBig_, kBig, {fx, 1.0f}, leading ? pal.digitDim : colText, s);
            if (9 - i == digit_) dl->AddRectFilled({fx, 14.0f}, {fx + cw - 1.0f, 16.0f}, pal.accent);
            fx += cw;
            if (i == 0 || i == 3 || i == 6) {
                dl->AddText(fontBig_, kBig, {fx, 1.0f}, leading ? pal.digitDim : colText, ".");
                fx += cw;
            }
        }
    }
    {   // right: SNR bar + RF gain
        const std::string g = fmt("%.1fdB", double(radio_.source().gain()));
        const float gw = smallW(g);
        small(float(kW) - gw - 1.0f, 3.0f, colText, g);
        const float bx1 = float(kW) - gw - 5.0f, bx0 = bx1 - 34.0f;
        const float snr = radio_.playing() ? std::clamp(spec_.snrDb, 0.0f, 60.0f) : 0.0f;
        dl->AddRectFilled({bx0, 5}, {bx1, 11}, ImGui::GetColorU32(ImGuiCol_FrameBg));
        dl->AddRectFilled({bx0, 5}, {bx0 + (bx1 - bx0) * snr / 60.0f, 11}, pal.accent);
        const std::string s = fmt("%2.0f", double(snr));
        small(bx0 - smallW(s) - 2.0f, 3.0f, colDim, s);
    }

    // ---- viewport annotations -------------------------------------------------
    const float vy = float(kBarH);
    if (!drawerOpen_) {
        small(1.0f, vy, colDim, fmt("%.0f", double(fftMax_)));
        small(1.0f, vy + float(kFftH) - 9.0f, colDim, fmt("%.0f", double(fftMin_)));
    }
    {
        const std::string s = fmt("%s%s", hzText(radio_.sampleRate() / double(1 << zoomIdx_)).c_str(), zoomIdx_ ? fmt(" x%d", 1 << zoomIdx_).c_str() : "");
        small(float(kW) - smallW(s) - 1.0f, vy, colDim, s);
    }

    // RDS / CTCSS ticker: one translucent line at the bottom of the spectrum.
    std::string ticker;
    if (radio_.playing() && radio_.rx().mode == Demod::NFM && radio_.receiver.ctcssHz.load() > 0.0f)
        ticker = fmt("CTCSS %.1f Hz", double(radio_.receiver.ctcssHz.load()));
    if (!ticker.empty()) {
        const float ty = vy + float(kFftH) - 10.0f, tx = drawerOpen_ ? float(kDrawerW) : 0.0f;
        dl->AddRectFilled({tx, ty}, {float(kW), vy + float(kFftH)}, IM_COL32(0, 0, 0, 140));
        small(tx + (float(kW) - tx - smallW(ticker)) * 0.5f, ty, IM_COL32(120, 230, 140, 255), ticker);
    }

    // ---- accordion drawer -------------------------------------------------------
    if (drawerOpen_) {
        const float dx1 = float(kDrawerW);
        dl->AddRectFilled({0, vy}, {dx1, float(kH)}, (colBg & 0x00FFFFFFu) | (ImU32(222) << 24));
        dl->AddLine({dx1, vy}, {dx1, float(kH)}, pal.accent);
        const float tabW = dx1 / float(kTabs);
        for (int t = 0; t < kTabs; ++t) {
            const float tx = std::floor(tabW * float(t));
            const bool built = !pages_[size_t(t)].empty();
            if (t == tab_) dl->AddRectFilled({tx, vy}, {tx + tabW, vy + float(kTabH)}, pal.accent);
            else dl->AddRectFilled({tx, vy}, {tx + tabW - 1.0f, vy + float(kTabH)}, colHdr);
            small(tx + (tabW - smallW(kTabNames[t])) * 0.5f, vy + 1.0f, t == tab_ ? IM_COL32(255, 255, 255, 255) : (built ? colText : colDim), kTabNames[t]);
        }
        const float ry0 = vy + float(kTabH) + 1.0f;
        const int rowsVisible = (kH - int(ry0)) / kRowH;
        const auto vis = visibleItems(tab_);
        const int n = int(vis.size());
        if (n == 0) {
            small(4.0f, ry0 + 4.0f, colText, kTabTitles[tab_]);
            small(4.0f, ry0 + 20.0f, colDim, "Backend module not built yet.");
            small(4.0f, ry0 + 31.0f, colDim, "No controls are shown until it");
            small(4.0f, ry0 + 42.0f, colDim, "exists in the engine.");
        } else {
            int& sel = sel_[size_t(tab_)];
            int& scroll = scroll_[size_t(tab_)];
            sel = std::clamp(sel, 0, n - 1);
            scroll = std::clamp(scroll, std::max(0, sel - rowsVisible + 1), std::min(sel, std::max(0, n - rowsVisible)));
            for (int r = 0; r < rowsVisible && scroll + r < n; ++r) {
                const Item& it = pages_[size_t(tab_)][size_t(vis[size_t(scroll + r)])];
                const float y = ry0 + float(r * kRowH);
                const bool selected = (scroll + r == sel);
                const bool adjustable = bool(it.adjust) || bool(it.activate) || bool(it.editSet);
                if (selected) dl->AddRectFilled({1, y}, {dx1 - 4.0f, y + float(kRowH)}, editing_ ? IM_COL32(150, 90, 20, 255) : colHdr);
                small(4.0f, y + 1.0f, adjustable ? colText : colDim, it.label);
                std::string v = (selected && editing_) ? editBuf_ + "_" : (it.value ? it.value() : std::string());
                while (v.size() > 3 && smallW(v) > dx1 - 96.0f) v.erase(v.size() - 1);
                if (selected && it.adjust && !editing_) v = "< " + v + " >";
                small(dx1 - 7.0f - smallW(v), y + 1.0f, selected ? IM_COL32(255, 255, 255, 255) : (adjustable ? pal.accent : colDim), v);
            }
            if (n > rowsVisible) {  // scroll indicator
                const float trackH = float(kH) - ry0, thumbH = trackH * float(rowsVisible) / float(n);
                const float ty = ry0 + trackH * float(scroll) / float(n);
                dl->AddRectFilled({dx1 - 3.0f, ty}, {dx1 - 1.0f, ty + thumbH}, pal.accent);
            }
        }
    }

    // ---- direct frequency entry -------------------------------------------------
    if (entryActive_) {
        const float bw = 150.0f, bh = 30.0f, bx = (float(kW) - bw) * 0.5f, by = (float(kH) - bh) * 0.5f;
        dl->AddRectFilled({bx, by}, {bx + bw, by + bh}, colBg, 3.0f);
        dl->AddRect({bx, by}, {bx + bw, by + bh}, pal.accent, 3.0f);
        small(bx + 5.0f, by + 3.0f, colDim, "FREQUENCY (MHz)  Enter=tune");
        dl->AddText(fontBig_, kBig, {bx + 6.0f, by + 14.0f}, colText, (entry_ + "_").c_str());
    }

    // ---- toast --------------------------------------------------------------------
    if (now < toastUntil_ && !toast_.empty()) {
        std::string t = toast_;
        while (t.size() > 4 && smallW(t) > float(kW) - 12.0f) t.erase(t.size() - 1);
        const float tw = smallW(t) + 10.0f, tx = (float(kW) - tw) * 0.5f, ty = float(kH) - 18.0f;
        dl->AddRectFilled({tx, ty}, {tx + tw, ty + 13.0f}, IM_COL32(0, 0, 0, 215), 2.0f);
        dl->AddRect({tx, ty}, {tx + tw, ty + 13.0f}, pal.accent, 2.0f);
        small(tx + 5.0f, ty + 2.0f, IM_COL32(255, 255, 255, 255), t);
    }
}

}  // namespace osx
