#include "cardputer/ui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace osx {

namespace {

constexpr float kSmall = 10.0f, kBig = 13.0f;
constexpr int kRowH = 11, kTabH = 12;

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string fmt(const char* f, ...) {
    char buf[128];
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

// "119.450", "118.0125", "7.074" - at least 3 decimals, more only if needed.
std::string mhzText(double hz) {
    std::string s = fmt("%.6f", hz / 1e6);
    while (s.size() > 1 && s.back() == '0' && s.size() - s.find('.') > 4) s.pop_back();
    return s;
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

CardUi::CardUi(Radio& radio, BookmarkStore& bookmarks)
    : radio_(radio), marks_(bookmarks), px_(size_t(kW) * kViewH, 0xFF000000u) {
    const char* home = std::getenv("HOME");
    const std::string h = home && *home ? home : ".";
    std::error_code ec;
    fileDir_ = std::filesystem::is_directory(h + "/iq", ec) ? h + "/iq" : h;
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

void CardUi::setRx(const std::function<void(RxConfig&)>& f) {
    RxConfig c = radio_.rx();
    f(c);
    radio_.setRx(c);
}

void CardUi::scanFiles() {
    files_.clear();
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(fileDir_, ec)) {
        std::string ext = e.path().extension().string();
        for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (e.is_regular_file(ec) && ext == ".wav") files_.push_back(e.path().string());
    }
    std::sort(files_.begin(), files_.end());
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
    auto isHw = [this] { return radio_.sourceKind() == SK::RtlSdr || radio_.sourceKind() == SK::RtlTcp; };
    auto isSim = [this] { return radio_.sourceKind() == SK::Simulated; };
    auto isFile = [this] { return radio_.sourceKind() == SK::File; };
    auto toggle = [](Item& it, std::function<bool()> get, std::function<void(bool)> set) {
        it.value = [get] { return onOff(get()); };
        it.adjust = [get, set](int) { set(!get()); };
        it.activate = [get, set] { set(!get()); };
    };
    auto modeIs = [this](std::initializer_list<Demod> ms) {
        return std::find(ms.begin(), ms.end(), radio_.rx().mode) != ms.end();
    };

    // ---- [1] SOURCE ---------------------------------------------------------
    Page& src = pages_[kSource];
    toggle(add(src, "Run"), [this] { return radio_.playing(); }, [this](bool on) {
        if (!on) radio_.stop();
        else if (!radio_.play()) toast(radio_.lastError(), 3.0);
    });
    {
        Item& it = add(src, "Device");
        it.value = [this] { return std::string(Radio::sourceKindName(radio_.sourceKind())) + (radio_.fallbackActive() ? " (fallback)" : ""); };
        it.adjust = [this](int d) {
            const int n = int(SK::Count);
            SK k = SK((int(radio_.sourceKind()) + d + n) % n);
            if (k == SK::File && !radio_.file.isOpen()) k = SK((int(k) + d + n) % n);  // nothing loaded yet
            const bool was = radio_.playing();
            radio_.selectSource(k);
            if (was && !radio_.playing()) toast(radio_.lastError(), 3.0);
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
        it.visible = [isFile] { return !isFile(); };
        it.value = [this] { return fmt("%.1f dB", double(radio_.source().gain())); };
        it.adjust = [this](int d) { stepGain(d); };
    }
    {
        Item& it = add(src, "RTL device #");
        it.visible = isRtl;
        it.value = [this] { return fmt("%d of %d", radio_.rtl.deviceIndex, RtlSdrSource::deviceCount()); };
        it.adjust = [this](int d) {
            const int n = std::max(1, RtlSdrSource::deviceCount());
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
        Item& it = add(src, "IQ file");
        it.value = [this] {
            const std::string& p = radio_.file.path();
            return p.empty() ? std::string("[Enter path]") : p.substr(p.find_last_of('/') == std::string::npos ? 0 : p.find_last_of('/') + 1);
        };
        it.adjust = [this](int d) {  // cycle the .wav files in the folder
            scanFiles();
            if (files_.empty()) { toast("no .wav files in " + fileDir_, 2.0); return; }
            int i = 0;
            for (size_t k = 0; k < files_.size(); ++k)
                if (files_[k] == radio_.file.path()) i = int(k) + d;
            i = ((i % int(files_.size())) + int(files_.size())) % int(files_.size());
            if (!radio_.openFile(files_[size_t(i)])) toast(radio_.lastError(), 3.0);
            else toast(fmt("FILE %.3f MSPS %d-bit", radio_.sampleRate() / 1e6, radio_.file.info().encoding));
        };
        it.editGet = [this] { return radio_.file.path().empty() ? fileDir_ + "/" : radio_.file.path(); };
        it.editSet = [this](const std::string& s) {
            if (!radio_.openFile(s)) toast(radio_.lastError(), 3.0);
        };
    }
    {
        Item& it = add(src, "File folder");
        it.value = [this] { return fileDir_; };
        it.editGet = [this] { return fileDir_; };
        it.editSet = [this](const std::string& s) { if (!s.empty()) fileDir_ = s; };
    }
    {
        Item& it = add(src, "Loop");
        it.visible = isFile;
        toggle(it, [this] { return radio_.file.loop.load(); }, [this](bool on) { radio_.file.loop = on; });
    }
    {
        Item& it = add(src, "Seek");
        it.visible = isFile;
        it.value = [this] {
            const double d = radio_.file.durationSeconds();
            return fmt("%.1f / %.1f s", radio_.file.position() * d, d);
        };
        it.adjust = [this](int d) { radio_.file.seek(std::clamp(radio_.file.position() + 0.05 * d, 0.0, 0.999)); };
    }
    {
        Item& it = add(src, "File centre");
        it.visible = isFile;
        it.value = [this] { return mhzText(radio_.centerHz()) + (radio_.file.info().centerHz > 0 ? "" : " ?"); };
        it.editGet = [this] { return mhzText(radio_.centerHz()); };
        it.editSet = [this](const std::string& s) {  // label an unnamed recording (MHz), while stopped
            const double hz = std::atof(s.c_str()) * 1e6;
            if (hz > 0) {
                const bool was = radio_.playing();
                radio_.stop();
                radio_.setCenter(hz);
                radio_.tune(hz);
                if (was) radio_.play();
            }
        };
    }
    {
        Item& it = add(src, "Sim noise");
        it.visible = isSim;
        it.value = [this] { return fmt("%.0f dBFS", double(radio_.sim.noiseDb.load())); };
        it.adjust = [this](int d) { radio_.sim.noiseDb = std::clamp(radio_.sim.noiseDb.load() + 2.0f * float(d), -90.0f, -10.0f); };
    }
    {
        Item& it = add(src, "Sim impulses");
        it.visible = isSim;
        it.value = [this] { return fmt("%.0f /s", double(radio_.sim.impulseRate.load())); };
        it.adjust = [this](int d) { radio_.sim.impulseRate = std::clamp(radio_.sim.impulseRate.load() + 50.0f * float(d), 0.0f, 1000.0f); };
    }
    {
        Item& it = add(src, "Sim IQ skew");
        it.visible = isSim;
        it.value = [this] { return fmt("%.1f deg", double(radio_.sim.iqPhaseErrDeg.load())); };
        it.adjust = [this](int d) {
            const float v = std::clamp(radio_.sim.iqPhaseErrDeg.load() + float(d), 0.0f, 10.0f);
            radio_.sim.iqPhaseErrDeg = v;
            radio_.sim.iqGainErrDb = v * 0.25f;
        };
    }
    {
        Item& it = add(src, "Sim emitters");
        it.visible = isSim;
        toggle(it, [this] { return radio_.sim.emittersEnabled.load(); }, [this](bool on) { radio_.sim.emittersEnabled = on; });
    }
    add(src, "ADC peak").value = [this] { return fmt("%.0f %%%s", double(radio_.dsp.peakLevel()) * 100.0, radio_.dsp.peakLevel() > 0.98f ? " CLIP" : ""); };
    add(src, "Overruns").value = [this] { return fmt("%llu B", static_cast<unsigned long long>(radio_.source().overruns())); };

    // ---- [2] RADIO ----------------------------------------------------------
    Page& rad = pages_[kRadio];
    {
        Item& it = add(rad, "Mode [D]");
        it.value = [this] { return std::string(demodName(radio_.rx().mode)); };
        it.adjust = [this](int d) {
            const int n = int(Demod::Count);
            radio_.setMode(Demod((int(radio_.rx().mode) + d + n) % n));
        };
    }
    {
        Item& it = add(rad, "Bandwidth [B]");
        it.value = [this] { return hzText(radio_.rx().bandwidthHz); };
        it.adjust = [this](int d) { stepBandwidth(d); };
    }
    {
        Item& it = add(rad, "Low cut");
        it.visible = [modeIs] { return modeIs({Demod::USB, Demod::LSB}); };
        it.value = [this] { return fmt("%.0f Hz", radio_.rx().lowCutHz); };
        it.adjust = [this](int d) { setRx([d](RxConfig& c) { c.lowCutHz = std::clamp(c.lowCutHz + 50.0 * d, 100.0, 300.0); }); };
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
        Item& it = add(rad, "Volume [+/-]");
        it.value = [this] { return fmt("%.0f %%", double(radio_.audio.volume.load()) * 100.0); };
        it.adjust = [this](int d) { stepVolume(d); };
    }
    toggle(add(rad, "Mute [Space]"), [this] { return radio_.audio.mute.load(); }, [this](bool on) { radio_.audio.mute = on; });
    toggle(add(rad, "Squelch"), [this] { return radio_.rx().squelch; }, [this](bool on) { setRx([on](RxConfig& c) { c.squelch = on; }); });
    {
        Item& it = add(rad, "Squelch lvl [S]");
        it.value = [this] { return fmt("%.0f dBFS", double(radio_.rx().squelchDb)); };
        it.adjust = [this](int d) { setRx([d](RxConfig& c) { c.squelchDb = std::clamp(c.squelchDb + float(d), -120.0f, 0.0f); }); };
    }
    {
        Item& it = add(rad, "Stereo");
        it.visible = [modeIs] { return modeIs({Demod::WFM}); };
        toggle(it, [this] { return radio_.rx().stereo; }, [this](bool on) { setRx([on](RxConfig& c) { c.stereo = on; }); });
    }
    {
        Item& it = add(rad, "RDS decoder");
        it.visible = [modeIs] { return modeIs({Demod::WFM}); };
        toggle(it, [this] { return radio_.rx().rds; }, [this](bool on) { setRx([on](RxConfig& c) { c.rds = on; }); });
    }
    {
        Item& it = add(rad, "De-emphasis");
        it.visible = [modeIs] { return modeIs({Demod::WFM, Demod::NFM}); };
        it.value = [this] {
            static const char* names[] = {"Off", "50 us", "75 us"};
            return std::string(names[std::clamp(radio_.rx().deemphasis, 0, 2)]);
        };
        it.adjust = [this](int d) { setRx([d](RxConfig& c) { c.deemphasis = (c.deemphasis + d + 3) % 3; }); };
    }
    {
        Item& it = add(rad, "CTCSS detect");
        it.visible = [modeIs] { return modeIs({Demod::NFM}); };
        toggle(it, [this] { return radio_.rx().ctcss; }, [this](bool on) { setRx([on](RxConfig& c) { c.ctcss = on; }); });
    }
    {
        Item& it = add(rad, "AGC");
        it.visible = [modeIs] { return modeIs({Demod::AM, Demod::SAM, Demod::USB, Demod::LSB, Demod::DSB, Demod::CW}); };
        it.value = [this] {
            static const char* names[] = {"Fast", "Medium", "Slow", "Off"};
            return std::string(names[std::clamp(radio_.rx().agc, 0, 3)]);
        };
        it.adjust = [this](int d) { setRx([d](RxConfig& c) { c.agc = (c.agc + d + 4) % 4; }); };
    }
    {
        Item& it = add(rad, "AF gain");
        it.visible = [this, modeIs] { return radio_.rx().agc == 3 && modeIs({Demod::AM, Demod::SAM, Demod::USB, Demod::LSB, Demod::DSB, Demod::CW}); };
        it.value = [this] { return fmt("%.0f dB", double(radio_.rx().afGainDb)); };
        it.adjust = [this](int d) { setRx([d](RxConfig& c) { c.afGainDb = std::clamp(c.afGainDb + 3.0f * float(d), 0.0f, 60.0f); }); };
    }
    {
        Item& it = add(rad, "CW pitch");
        it.visible = [modeIs] { return modeIs({Demod::CW}); };
        it.value = [this] { return fmt("%.0f Hz", double(radio_.rx().cwPitchHz)); };
        it.adjust = [this](int d) { setRx([d](RxConfig& c) { c.cwPitchHz = std::clamp(c.cwPitchHz + 50.0f * float(d), 300.0f, 1000.0f); }); };
    }
    add(rad, "Channel level").value = [this] { return fmt("%.1f dBFS", double(radio_.receiver.levelDb.load())); };
    {
        Item& it = add(rad, "Pilot");
        it.visible = [modeIs] { return modeIs({Demod::WFM}); };
        it.value = [this] { return std::string(radio_.receiver.stereoLocked.load() ? "STEREO" : "mono"); };
    }
    {
        Item& it = add(rad, "RDS PI / PTY");
        it.visible = [modeIs] { return modeIs({Demod::WFM}); };
        it.value = [this] {
            const RdsInfo r = radio_.receiver.rdsInfo();
            if (!r.synced && !r.piValid) return std::string("no sync");
            return fmt("%04X %s", r.pi, rds::ptyName(r.pty));
        };
    }
    {
        Item& it = add(rad, "RDS PS");
        it.visible = [modeIs] { return modeIs({Demod::WFM}); };
        it.value = [this] { const RdsInfo r = radio_.receiver.rdsInfo(); return r.ps.empty() ? std::string("-") : r.ps; };
    }
    {
        Item& it = add(rad, "CTCSS tone");
        it.visible = [modeIs] { return modeIs({Demod::NFM}); };
        it.value = [this] { const float t = radio_.receiver.ctcssHz.load(); return t > 0.0f ? fmt("%.1f Hz", double(t)) : std::string("-"); };
    }
    add(rad, "Audio out").value = [this] { return radio_.audio.deviceOpen() ? radio_.audio.status() : std::string("NO DEVICE"); };
    add(rad, "Out peak").value = [this] { return fmt("%.1f dBFS", 20.0 * std::log10(double(radio_.audio.outputPeak()) + 1e-6)); };

    // ---- [3] DSP / NR -------------------------------------------------------
    Page& dsp = pages_[kDsp];
    {
        Item& it = add(dsp, "Preset [N]");
        it.value = [this] { return std::string(nrPresetName(nrPreset(radio_.rx()))); };
        it.adjust = [this](int d) {
            const int cur = int(nrPreset(radio_.rx()));
            const int next = cur == int(NrPreset::Custom) ? (d > 0 ? 0 : 3) : (cur + d + 4) % 4;
            setRx([next](RxConfig& c) { applyNrPreset(c, NrPreset(next)); });
        };
    }
    toggle(add(dsp, "DC removal"), [this] { return radio_.dsp.dcRemoval.load(); }, [this](bool on) { radio_.dsp.dcRemoval = on; });
    toggle(add(dsp, "IQ auto-balance"), [this] { return radio_.dsp.iqCorrection.load(); }, [this](bool on) { radio_.dsp.iqCorrection = on; });
    add(dsp, "IQ error").value = [this] {
        return fmt("%.2f deg %.2f dB", double(radio_.dsp.iqPhaseErrDeg.load()), double(radio_.dsp.iqGainErrDb.load()));
    };
    toggle(add(dsp, "Invert IQ"), [this] { return radio_.dsp.invertIq.load(); }, [this](bool on) { radio_.dsp.invertIq = on; spec_.resetPeak(); });
    toggle(add(dsp, "Noise blanker"), [this] { return radio_.rx().nb; }, [this](bool on) { setRx([on](RxConfig& c) { c.nb = on; }); });
    {
        Item& it = add(dsp, "NB threshold");
        it.value = [this] { return fmt("%.1fx RMS", double(radio_.rx().nbThreshold)); };
        it.adjust = [this](int d) { setRx([d](RxConfig& c) { c.nbThreshold = std::clamp(c.nbThreshold + 0.5f * float(d), 1.5f, 8.0f); }); };
    }
    {
        Item& it = add(dsp, "NB holdoff");
        it.value = [this] { return fmt("%.0f us", double(radio_.rx().nbHoldoffUs)); };
        it.adjust = [this](int d) {
            static const std::vector<float> steps = {1, 2, 5, 10, 20, 50, 100, 200};
            setRx([d](RxConfig& c) { c.nbHoldoffUs = steps[size_t(std::clamp(nearestIndex(steps, c.nbHoldoffUs) + d, 0, int(steps.size()) - 1))]; });
        };
    }
    add(dsp, "NB blanked").value = [this] { return fmt("%.2f %%", double(radio_.receiver.nbBlankedPct.load())); };
    toggle(add(dsp, "Auto-notch"), [this] { return radio_.rx().anf; }, [this](bool on) { setRx([on](RxConfig& c) { c.anf = on; }); });
    toggle(add(dsp, "Noise reduction"), [this] { return radio_.rx().nr; }, [this](bool on) { setRx([on](RxConfig& c) { c.nr = on; }); });
    {
        Item& it = add(dsp, "NR strength");
        it.value = [this] { return fmt("%.0f %%", double(radio_.rx().nrStrength) * 100.0); };
        it.adjust = [this](int d) { setRx([d](RxConfig& c) { c.nrStrength = std::clamp(c.nrStrength + 0.1f * float(d), 0.0f, 1.0f); }); };
    }

    // ---- [4] BOOKMARKS: rebuilt from the store (buildBookmarkPage) -----------

    // ---- [7] DISPLAY --------------------------------------------------------
    Page& dis = pages_[kDisplay];
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
    toggle(add(dis, "Bookmark tags"), [this] { return showMarks_; }, [this](bool on) { showMarks_ = on; });
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
        Item& it = add(dis, "Auto range [A]");
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
        Item& it = add(dis, "Zoom [Z]");
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

void CardUi::buildBookmarkPage() {
    bookmarksDirty_ = false;
    Page& bm = pages_[kBookmarks];
    bm.clear();
    const auto& cats = BookmarkStore::categories();
    {
        Item& it = add(bm, "Category");
        it.value = [this, &cats] { return markFilter_ < 0 ? std::string("All") : cats[size_t(markFilter_)]; };
        it.adjust = [this, &cats](int d) {
            const int n = int(cats.size()) + 1;  // -1 = All
            markFilter_ = ((markFilter_ + 1 + d + n) % n) - 1;
            bookmarksDirty_ = true;
        };
    }
    {
        Item& it = add(bm, "Save current [K]");
        it.value = [this] { return mhzText(radio_.vfoHz()) + " " + demodName(radio_.rx().mode); };
        it.activate = [this] { quickSave(); };
    }
    for (size_t i = 0; i < marks_.items().size(); ++i) {
        const Bookmark& b = marks_.items()[i];
        if (markFilter_ >= 0 && b.category != cats[size_t(markFilter_)]) continue;
        Item& it = add(bm, b.name.c_str());
        it.value = [this, i] {
            if (i >= marks_.items().size()) return std::string();
            const Bookmark& m = marks_.items()[i];
            return mhzText(double(m.frequencyHz)) + " " + m.demodMode;
        };
        it.activate = [this, i] { recallBookmark(i); };
        it.adjust = [this, i, &cats](int d) {  // Left / Right: move to another category
            if (i >= marks_.items().size()) return;
            const int c = int(std::find(cats.begin(), cats.end(), marks_.items()[i].category) - cats.begin());
            const int n = int(cats.size());
            marks_.setCategory(i, cats[size_t(((c + d) % n + n) % n)]);
            bookmarksDirty_ = true;
        };
        it.key = [this, i](const KeyEvent& k) {
            if (i >= marks_.items().size()) return false;
            if (k.key == Key::Char && k.ch == 'e') {
                modal_ = Modal::Rename;
                modalTarget_ = i;
                text_ = marks_.items()[i].name;
                return true;
            }
            if (k.key == Key::Backspace) {
                modal_ = Modal::ConfirmDelete;
                modalTarget_ = i;
                return true;
            }
            return false;
        };
    }
}

std::vector<int> CardUi::visibleItems(int tab) const {
    std::vector<int> out;
    const Page& p = pages_[size_t(tab)];
    for (size_t i = 0; i < p.size(); ++i)
        if (!p[i].visible || p[i].visible()) out.push_back(int(i));
    return out;
}

CardUi::Item* CardUi::selectedItem() {
    if (tab_ == kBookmarks && bookmarksDirty_) buildBookmarkPage();
    const auto vis = visibleItems(tab_);
    if (vis.empty()) return nullptr;
    int& sel = sel_[size_t(tab_)];
    sel = std::clamp(sel, 0, int(vis.size()) - 1);
    return &pages_[size_t(tab_)][size_t(vis[size_t(sel)])];
}

std::string CardUi::selectedLabel() const {
    const auto vis = visibleItems(tab_);
    if (vis.empty()) return {};
    const int sel = std::clamp(sel_[size_t(tab_)], 0, int(vis.size()) - 1);
    return pages_[size_t(tab_)][size_t(vis[size_t(sel)])].label;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void CardUi::tuneStep(int dir) { radio_.tune(radio_.vfoHz() + double(dir) * std::pow(10.0, digit_)); }

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

void CardUi::stepVolume(int dir) {
    const float v = std::clamp(std::round(radio_.audio.volume.load() * 20.0f + float(dir)) / 20.0f, 0.0f, 1.0f);
    radio_.audio.volume = v;
    volOsdUntil_ = nowSeconds() + 1.0;
}

void CardUi::cycleNrPreset() {
    const NrPreset cur = nrPreset(radio_.rx());
    const NrPreset next = cur == NrPreset::Custom ? NrPreset::Off : NrPreset((int(cur) + 1) % 4);
    setRx([next](RxConfig& c) { applyNrPreset(c, next); });
    toast(std::string("INTERFERENCE: ") + nrPresetName(next));
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

void CardUi::quickSave() {
    Bookmark b;
    b.frequencyHz = int64_t(std::llround(radio_.vfoHz()));
    b.demodMode = demodName(radio_.rx().mode);
    b.bandwidthHz = int(std::lround(radio_.rx().bandwidthHz));
    b.category = markFilter_ >= 0 ? BookmarkStore::categories()[size_t(markFilter_)] : "User";
    b.name = mhzText(double(b.frequencyHz)) + " MHz " + b.demodMode;
    modalTarget_ = marks_.add(b);
    bookmarksDirty_ = true;
    if (!marks_.lastError().empty()) toast(marks_.lastError(), 3.0);
    // The bookmark already exists; the box only renames it (Esc keeps this name).
    modal_ = Modal::Rename;
    text_ = b.name;
}

void CardUi::recallBookmark(size_t index) {
    if (index >= marks_.items().size()) return;
    const Bookmark& b = marks_.items()[index];
    Demod m = radio_.rx().mode;
    if (!demodFromName(b.demodMode, m)) toast("unknown mode '" + b.demodMode + "', keeping " + demodName(m), 2.0);
    radio_.recall(double(b.frequencyHz), m, double(b.bandwidthHz));
    toast(b.name + "  " + mhzText(radio_.vfoHz()) + " " + demodName(radio_.rx().mode), 1.5);
}

void CardUi::commitFreqEntry() {
    modal_ = Modal::None;
    if (text_.empty() || text_ == ".") return;
    const double mhz = std::atof(text_.c_str());  // always MHz: "121.5" -> 121 500 000 Hz
    radio_.tune(std::round(mhz * 1e6));
    toast(fmt("TUNED %.6f MHz", radio_.vfoHz() / 1e6));
}

// Returns true when the key was a hotkey.
bool CardUi::hotkey(const KeyEvent& k) {
    if (k.key == Key::Space) {
        radio_.audio.mute = !radio_.audio.mute.load();
        volOsdUntil_ = nowSeconds() + 1.0;
        return true;
    }
    if (k.key != Key::Char) return false;
    const int dir = k.shift ? -1 : 1;
    switch (k.ch) {
        case 'd': cycleMode(); return true;
        case 'k': quickSave(); return true;
        case 'n': cycleNrPreset(); return true;
        case '+': case '=': stepVolume(+1); return true;
        case '-': case '_': stepVolume(-1); return true;
        case 'v':  // V then Left/Right (or , /) adjusts the volume for 2 s
            volArmedUntil_ = nowSeconds() + 2.0;
            volOsdUntil_ = volArmedUntil_;
            return true;
        case 'b':
            stepBandwidth(dir);
            toast("BW " + hzText(radio_.rx().bandwidthHz));
            return true;
        case 'g': {
            bool agc = false;
            if (radio_.sourceKind() == Radio::SourceKind::RtlSdr || radio_.sourceKind() == Radio::SourceKind::RtlTcp)
                hw([&](auto& s) { agc = s.tunerAgc(); });
            if (agc) { toast("TUNER AGC ON"); return true; }
            stepGain(dir);
            toast(fmt("RF GAIN %.1f dB", double(radio_.source().gain())));
            return true;
        }
        case 's': stepSquelch(dir); return true;
        case 'a': autoRange(); return true;
        case 'z':
            zoomIdx_ = (zoomIdx_ + 1) % 3;
            toast(fmt("ZOOM %dx", 1 << zoomIdx_));
            return true;
        case 'p':
            if (radio_.playing()) { radio_.stop(); toast("STOP"); }
            else if (radio_.play()) toast("RUN");
            else toast(radio_.lastError(), 3.0);
            return true;
        case 'r': toast("RECORDER: PHASE 3", 1.5); return true;
        case 'l': toast("SCANNER: PHASE 3", 1.5); return true;
        default: return false;
    }
}

// A modal box owns the keyboard: nothing here can reach the radio hotkeys.
bool CardUi::modalKey(const KeyEvent& k) {
    const bool isEsc = k.key == Key::Esc || (k.key == Key::Char && k.ch == '`' && modal_ == Modal::FreqEntry);
    switch (modal_) {
        case Modal::FreqEntry: {
            const bool digit = k.key == Key::Char && k.ch >= '0' && k.ch <= '9' && !k.shift;
            if (digit && text_.size() < 11) text_ += k.ch;
            else if (k.key == Key::Char && k.ch == '.' && !k.shift && text_.find('.') == std::string::npos) text_ += '.';
            else if (k.key == Key::Backspace) { if (!text_.empty()) text_.pop_back(); }
            else if (k.key == Key::Enter) commitFreqEntry();
            else if (isEsc) modal_ = Modal::None;
            return true;
        }
        case Modal::Rename:
            if (k.key == Key::Enter) {
                std::string name = text_;
                while (!name.empty() && name.back() == ' ') name.pop_back();
                if (!name.empty()) marks_.rename(modalTarget_, name);
                bookmarksDirty_ = true;
                modal_ = Modal::None;
                toast("SAVED: " + (modalTarget_ < marks_.items().size() ? marks_.items()[modalTarget_].name : name), 1.5);
            } else if (isEsc) {
                modal_ = Modal::None;  // keep the name it already has
                toast("SAVED: " + (modalTarget_ < marks_.items().size() ? marks_.items()[modalTarget_].name : std::string()), 1.5);
            } else if (k.key == Key::Backspace) {
                if (!text_.empty()) text_.pop_back();
            } else if (k.text && text_.size() < 32) {
                text_ += k.text;
            }
            return true;
        case Modal::ConfirmDelete:
            if (k.key == Key::Enter || (k.key == Key::Char && k.ch == 'y')) {
                const std::string name = modalTarget_ < marks_.items().size() ? marks_.items()[modalTarget_].name : "";
                marks_.remove(modalTarget_);
                bookmarksDirty_ = true;
                toast("DELETED: " + name, 1.5);
            }
            modal_ = Modal::None;  // any other key cancels
            return true;
        case Modal::FieldEdit:
            if (k.key == Key::Enter) {
                if (fieldSet_) fieldSet_(text_);
                modal_ = Modal::None;
            } else if (isEsc) {
                modal_ = Modal::None;
            } else if (k.key == Key::Backspace) {
                if (!text_.empty()) text_.pop_back();
            } else if (k.text && text_.size() < 120) {
                text_ += k.text;
            }
            return true;
        default: return false;
    }
}

void CardUi::onKey(const KeyEvent& k) {
    if (k.key == Key::None) return;
    // Rebuild a stale bookmark list here, never inside one of its own row callbacks.
    if (bookmarksDirty_) buildBookmarkPage();
    if (modal_ != Modal::None) {
        modalKey(k);
        if (bookmarksDirty_) buildBookmarkPage();
        return;
    }
    const bool isEscape = k.key == Key::Esc || (k.key == Key::Char && k.ch == '`');
    if (k.key == Key::Tab) { drawerOpen_ = !drawerOpen_; return; }
    if (k.ctrl && k.key == Key::Char && k.ch == 's') { quickSave(); return; }
    if (k.ctrl) return;  // no other Ctrl chords

    // Fn-free arrows: on the Cardputer the arrow legends sit on ; . , /
    const bool upKey = k.key == Key::Up || (k.key == Key::Char && k.ch == ';' && !k.shift);
    const bool downKey = k.key == Key::Down || (k.key == Key::Char && k.ch == '.' && !k.shift);
    const bool leftKey = k.key == Key::Left || (k.key == Key::Char && k.ch == ',' && !k.shift);
    const bool rightKey = k.key == Key::Right || (k.key == Key::Char && k.ch == '/' && !k.shift);
    if (!leftKey && !rightKey && !(k.key == Key::Char && k.ch == 'v')) volArmedUntil_ = 0.0;  // V mode ends on any other key

    if (drawerOpen_) {
        if (isEscape) { drawerOpen_ = false; return; }
        if (k.key == Key::Char && (k.ch == '[' || k.ch == ']') && !k.shift) {
            tab_ = (tab_ + (k.ch == ']' ? 1 : kTabs - 1)) % kTabs;
            return;
        }
        if (k.key == Key::Char && !k.shift && k.ch >= '1' && k.ch <= char('0' + kTabs)) {
            tab_ = k.ch - '1';
            return;
        }
        Item* it = selectedItem();
        const int n = int(visibleItems(tab_).size());
        int& sel = sel_[size_t(tab_)];
        if (upKey) { if (n > 0) sel = (sel + n - 1) % n; return; }
        if (downKey) { if (n > 0) sel = (sel + 1) % n; return; }
        if (nowSeconds() < volArmedUntil_ && (leftKey || rightKey)) {
            stepVolume(rightKey ? 1 : -1);
            volArmedUntil_ = volOsdUntil_ = nowSeconds() + 2.0;
            return;
        }
        if (leftKey) { if (it && it->adjust) it->adjust(-1); return; }
        if (rightKey) { if (it && it->adjust) it->adjust(+1); return; }
        if (it && it->key && it->key(k)) return;
        if (k.key == Key::Enter) {
            if (!it) return;
            if (it->editSet) {
                modal_ = Modal::FieldEdit;
                text_ = it->editGet ? it->editGet() : std::string();
                fieldSet_ = it->editSet;
            } else if (it->activate) {
                it->activate();
            } else if (it->adjust) {
                it->adjust(+1);
            }
            return;
        }
        hotkey(k);
        return;
    }

    if (isEscape) return;
    if (nowSeconds() < volArmedUntil_ && (leftKey || rightKey)) {
        stepVolume(rightKey ? 1 : -1);
        volArmedUntil_ = volOsdUntil_ = nowSeconds() + 2.0;
        return;
    }
    if (k.key == Key::Left || leftKey) { tuneStep(-1); return; }
    if (k.key == Key::Right || rightKey) { tuneStep(+1); return; }
    if (k.key == Key::Up) { digit_ = std::min(digit_ + 1, 5); return; }
    if (k.key == Key::Down) { digit_ = std::max(digit_ - 1, 1); return; }
    if (upKey) { digit_ = digit_ >= 5 ? 1 : digit_ + 1; return; }  // ';' cycles the step coarser, wrapping
    const bool digitKey = k.key == Key::Char && !k.shift && k.ch >= '0' && k.ch <= '9';
    if (digitKey || downKey) {  // '.' starts an entry too ("0.5" -> 500 kHz)
        modal_ = Modal::FreqEntry;
        text_ = std::string(1, k.ch);
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
    double pbLo = 0.0, pbHi = 0.0;
    radio_.passband(pbLo, pbHi);
    spec_.measure(radio_.vfo.offsetHz + 0.5 * (pbLo + pbHi), pbHi - pbLo, sr);

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
    const int xl = xOf(radio_.vfo.offsetHz + pbLo), xr = xOf(radio_.vfo.offsetHz + pbHi);
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

    // VFO passband highlight (asymmetric for SSB).
    double pbLo = 0.0, pbHi = 0.0;
    radio_.passband(pbLo, pbHi);
    const int xl = std::clamp(xOf(radio_.vfo.offsetHz + pbLo), 0, kW - 1);
    const int xr = std::clamp(xOf(radio_.vfo.offsetHz + pbHi), 0, kW - 1);
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

    // Squelch threshold (dashed): green while open, amber while closed.
    if (radio_.rx().squelch) {
        const int y = yOf(radio_.rx().squelchDb);
        if (y >= 0 && y < kFftH)
            for (int x = 0; x < kW; ++x)
                if ((x / 3) % 2 == 0) px_[size_t(y) * kW + size_t(x)] = radio_.receiver.squelchOpen.load() ? 0xFF40D060u : 0xFFE0B030u;
    }
    // Bookmark ticks along the top edge (names are drawn by the overlay).
    if (showMarks_) {
        const uint32_t tick = 0xFFFFD040u;
        for (const Bookmark& b : marks_.items()) {
            const int x = xOf(double(b.frequencyHz) - radio_.centerHz());
            if (x < 0 || x >= kW) continue;
            for (int y = 0; y < 5; ++y) px_[size_t(y) * kW + size_t(x)] = tick;
            if (x + 1 < kW) px_[size_t(x + 1)] = tick;
            if (x > 0) px_[size_t(x - 1)] = tick;
        }
    }
    const int xc = xOf(radio_.vfo.offsetHz);
    if (xc >= 0 && xc < kW)
        for (int y = 0; y < kFftH; ++y) px_[size_t(y) * kW + size_t(xc)] = vfoLine;
}

// ---------------------------------------------------------------------------
// Overlay (text, drawer, popups), drawn with ImGui draw lists
// ---------------------------------------------------------------------------

void CardUi::draw(float fps) {
    fps_ = fps;
    if (bookmarksDirty_) buildBookmarkPage();
    const Palette& pal = palette();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImU32 colBg = ImGui::GetColorU32(ImGuiCol_WindowBg), colText = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 colDim = ImGui::GetColorU32(ImGuiCol_TextDisabled), colHdr = ImGui::GetColorU32(ImGuiCol_Header);
    const ImU32 white = IM_COL32(255, 255, 255, 255), red = IM_COL32(255, 80, 70, 255);
    auto small = [&](float x, float y, ImU32 col, const std::string& s) {
        dl->AddText(fontSmall_, kSmall, {std::floor(x), std::floor(y)}, col, s.c_str());
    };
    auto smallW = [&](const std::string& s) { return fontSmall_->CalcTextSizeA(kSmall, FLT_MAX, 0.0f, s.c_str()).x; };
    auto fit = [&](std::string s, float w) {
        while (s.size() > 2 && smallW(s) > w) s.pop_back();
        return s;
    };
    const double now = nowSeconds();
    const bool playing = radio_.playing();
    const Demod mode = radio_.rx().mode;

    // ---- status bar: [mode][flags]   frequency   [snr][gain][volume] -----
    dl->AddRectFilled({0, 0}, {float(kW), float(kBarH)}, colBg);
    // right block first, so the frequency can be placed between the two
    const float cw = fontBig_->CalcTextSizeA(kBig, FLT_MAX, 0.0f, "0").x;
    float rx = float(kW) - 1.0f;
    {
        const bool muted = radio_.audio.mute.load();
        const std::string v = muted ? "MUTE" : fmt("V%.0f", double(radio_.audio.volume.load()) * 100.0);
        rx -= smallW(v);
        small(rx, 3.0f, muted ? red : colText, v);
        rx -= 4.0f;
        const std::string g = radio_.sourceKind() == Radio::SourceKind::File ? std::string("FILE") : fmt("%.1f", double(radio_.source().gain()));
        rx -= smallW(g);
        small(rx, 3.0f, colDim, g);
        rx -= 4.0f;
        const float bx1 = rx, bx0 = rx - 22.0f;
        const float snr = playing ? std::clamp(spec_.snrDb, 0.0f, 60.0f) : 0.0f;
        dl->AddRectFilled({bx0, 5}, {bx1, 11}, ImGui::GetColorU32(ImGuiCol_FrameBg));
        dl->AddRectFilled({bx0, 5}, {bx0 + (bx1 - bx0) * snr / 60.0f, 11}, pal.accent);
        rx = bx0 - 3.0f;
    }
    const float freqW = cw * 13.0f;
    float lx = 1.0f;
    {
        const std::string m = demodName(mode);
        const float w = smallW(m) + 5.0f;
        dl->AddRectFilled({lx, 2}, {lx + w, 14}, pal.accent, 2.0f);
        small(lx + 3.0f, 3.0f, white, m);
        lx += w + 3.0f;
    }
    const float freqX = std::clamp(std::floor((float(kW) - freqW) * 0.5f), lx + 40.0f, rx - freqW);
    auto flag = [&](const std::string& s, ImU32 col) {  // dropped when there is no room left
        if (lx + smallW(s) > freqX - 2.0f) return;
        small(lx, 3.0f, col, s);
        lx += smallW(s) + 3.0f;
    };
    if (!playing) flag("STOP", colDim);
    if (radio_.fallbackActive()) flag("SIM", IM_COL32(230, 180, 50, 255));
    const NrPreset pre = nrPreset(radio_.rx());
    if (pre != NrPreset::Off) flag(pre == NrPreset::NB ? "NB" : pre == NrPreset::NBNotch ? "NB+N" : pre == NrPreset::Full ? "NR" : "NR*",
                                   IM_COL32(120, 200, 255, 255));
    if (playing && radio_.rx().squelch && !radio_.receiver.squelchOpen.load()) flag("SQ", IM_COL32(230, 180, 50, 255));
    if (playing && mode == Demod::WFM && radio_.receiver.stereoLocked.load()) flag("ST", IM_COL32(80, 220, 110, 255));
    {   // 10-digit frequency, leading zeros dimmed, underline on the tuning digit
        const auto hz = int64_t(std::llround(radio_.vfoHz()));
        char digits[11];
        std::snprintf(digits, sizeof(digits), "%010lld", static_cast<long long>(std::clamp<int64_t>(hz, 0, 9'999'999'999ll)));
        float fx = freqX;
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

    // ---- viewport annotations -----------------------------------------------
    const float vy = float(kBarH);
    const float vx0 = drawerOpen_ ? float(kDrawerW) + 1.0f : 0.0f;  // visible viewport left edge
    if (!drawerOpen_) {
        small(1.0f, vy + 5.0f, colDim, fmt("%.0f", double(fftMax_)));
        small(1.0f, vy + float(kFftH) - 9.0f, colDim, fmt("%.0f", double(fftMin_)));
    }
    {
        const std::string s = hzText(radio_.sampleRate() / double(1 << zoomIdx_)) + (zoomIdx_ ? fmt(" x%d", 1 << zoomIdx_) : "");
        small(float(kW) - smallW(s) - 1.0f, vy + 5.0f, colDim, s);
    }
    // Bookmark names next to their ticks (skipped where they would overlap).
    if (showMarks_) {
        const double sr = radio_.sampleRate(), span = sr / double(1 << zoomIdx_);
        const double lim = (sr - span) * 0.5;
        const double lo = std::clamp(radio_.vfo.offsetHz, -lim, lim) - span * 0.5;
        float lastEnd = vx0;
        std::vector<std::pair<float, const Bookmark*>> vis;
        for (const Bookmark& b : marks_.items()) {
            const float x = float((double(b.frequencyHz) - radio_.centerHz() - lo) / span * kW);
            if (x >= vx0 && x < float(kW)) vis.emplace_back(x, &b);
        }
        std::sort(vis.begin(), vis.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [x, b] : vis) {
            const std::string name = fit(b->name, 60.0f);
            const float w = smallW(name);
            const float tx = std::min(x + 2.0f, float(kW) - w - 1.0f);
            if (tx < lastEnd + 2.0f) continue;
            dl->AddRectFilled({tx - 1.0f, vy + 4.0f}, {tx + w + 1.0f, vy + 13.0f}, IM_COL32(0, 0, 0, 150));
            small(tx, vy + 3.0f, IM_COL32(255, 210, 70, 255), name);
            lastEnd = tx + w;
        }
    }

    // RDS / CTCSS ticker: one translucent line at the bottom of the spectrum.
    std::string ticker;
    if (playing && mode == Demod::NFM && radio_.receiver.ctcssHz.load() > 0.0f)
        ticker = fmt("CTCSS %.1f Hz", double(radio_.receiver.ctcssHz.load()));
    if (playing && mode == Demod::WFM && radio_.rx().rds) {
        const RdsInfo r = radio_.receiver.rdsInfo();
        if (!r.ps.empty() || !r.rt.empty()) {
            ticker = r.ps;
            if (!r.rt.empty()) ticker += (ticker.empty() ? "" : "  |  ") + r.rt;
        }
    }
    if (!ticker.empty()) {
        const float ty = vy + float(kFftH) - 10.0f, tw = float(kW) - vx0;
        dl->AddRectFilled({vx0, ty}, {float(kW), vy + float(kFftH)}, IM_COL32(0, 0, 0, 150));
        const float w = smallW(ticker);
        dl->PushClipRect({vx0, ty}, {float(kW), vy + float(kFftH)}, true);
        if (w <= tw - 4.0f) {
            small(vx0 + (tw - w) * 0.5f, ty, IM_COL32(120, 230, 140, 255), ticker);
        } else {  // marquee, 30 px/s
            const float period = w + 40.0f;
            const float off = std::fmod(float(now) * 30.0f, period);
            small(vx0 + 2.0f - off, ty, IM_COL32(120, 230, 140, 255), ticker);
            small(vx0 + 2.0f - off + period, ty, IM_COL32(120, 230, 140, 255), ticker);
        }
        dl->PopClipRect();
    }

    // ---- accordion drawer ---------------------------------------------------------
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
            small(tx + (tabW - smallW(kTabNames[t])) * 0.5f, vy + 1.0f, t == tab_ ? white : (built ? colText : colDim), kTabNames[t]);
        }
        const float ry0 = vy + float(kTabH) + 1.0f;
        const int rowsVisible = (kH - int(ry0)) / kRowH;
        const auto vis = visibleItems(tab_);
        const int n = int(vis.size());
        if (n == 0) {
            small(4.0f, ry0 + 4.0f, colText, kTabTitles[tab_]);
            small(4.0f, ry0 + 20.0f, colDim, "Scheduled for Phase 3/4.");
            small(4.0f, ry0 + 31.0f, colDim, "No controls until the backend");
            small(4.0f, ry0 + 42.0f, colDim, "module exists.");
            small(4.0f, ry0 + 64.0f, colDim, "[ ] or 1-7: other tabs");
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
                if (selected) dl->AddRectFilled({1, y}, {dx1 - 4.0f, y + float(kRowH)}, colHdr);
                std::string v = it.value ? it.value() : std::string();
                if (selected && it.adjust) v = "< " + v + " >";
                v = fit(v, dx1 - 70.0f);
                const float vw = smallW(v);
                small(4.0f, y + 1.0f, adjustable ? colText : colDim, fit(it.label, dx1 - 14.0f - vw));
                small(dx1 - 7.0f - vw, y + 1.0f, selected ? white : (adjustable ? pal.accent : colDim), v);
            }
            if (n > rowsVisible) {  // scroll indicator
                const float trackH = float(kH) - ry0, thumbH = trackH * float(rowsVisible) / float(n);
                const float ty = ry0 + trackH * float(scroll) / float(n);
                dl->AddRectFilled({dx1 - 3.0f, ty}, {dx1 - 1.0f, ty + thumbH}, pal.accent);
            }
            if (tab_ == kBookmarks && sel >= 2) {  // key help in the visible strip right of the drawer
                static const char* help[] = {"Enter tune", "E rename", "Del delete", "</> category"};
                const float hx = float(kDrawerW) + 4.0f, hy = float(kH) - 46.0f;
                dl->AddRectFilled({hx - 2.0f, hy - 1.0f}, {float(kW), float(kH)}, IM_COL32(0, 0, 0, 170));
                for (int i = 0; i < 4; ++i) small(hx, hy + float(i) * 11.0f, IM_COL32(255, 210, 70, 255), help[i]);
            }
        }
    }

    // ---- modal boxes ----------------------------------------------------------------
    auto box = [&](float w, float h, const std::string& title, const std::string& body, const std::string& hint, ImU32 border) {
        const float bx = std::floor((float(kW) - w) * 0.5f), by = std::floor((float(kH) - h) * 0.5f);
        dl->AddRectFilled({bx, by}, {bx + w, by + h}, colBg, 3.0f);
        dl->AddRect({bx, by}, {bx + w, by + h}, border, 3.0f);
        small(bx + 5.0f, by + 3.0f, colDim, title);
        std::string b = body;
        while (b.size() > 1 && fontBig_->CalcTextSizeA(kBig, FLT_MAX, 0.0f, b.c_str()).x > w - 12.0f) b.erase(0, 1);  // keep the cursor end
        dl->AddText(fontBig_, kBig, {bx + 6.0f, by + 14.0f}, colText, b.c_str());
        if (!hint.empty()) small(bx + 5.0f, by + h - 11.0f, colDim, hint);
    };
    switch (modal_) {
        case Modal::FreqEntry: box(150.0f, 30.0f, "FREQUENCY (MHz)  Enter=tune", text_ + "_", "", pal.accent); break;
        case Modal::Rename: box(240.0f, 41.0f, "BOOKMARK NAME", text_ + "_", "Enter=save  Esc=keep default", pal.accent); break;
        case Modal::FieldEdit: box(260.0f, 41.0f, "EDIT", text_ + "_", "Enter=apply  Esc=cancel", pal.accent); break;
        case Modal::ConfirmDelete: {
            const std::string name = modalTarget_ < marks_.items().size() ? marks_.items()[modalTarget_].name : "";
            box(240.0f, 41.0f, "DELETE BOOKMARK?", name, "Enter/Y=delete  any key=cancel", red);
            break;
        }
        default: break;
    }

    // ---- volume OSD -------------------------------------------------------------------
    if (now < volOsdUntil_ && modal_ == Modal::None) {
        const bool muted = radio_.audio.mute.load();
        const float v = radio_.audio.volume.load();
        const float w = 150.0f, h = 22.0f, bx = (float(kW) - w) * 0.5f, by = float(kH) - 30.0f;
        dl->AddRectFilled({bx, by}, {bx + w, by + h}, IM_COL32(0, 0, 0, 220), 3.0f);
        dl->AddRect({bx, by}, {bx + w, by + h}, pal.accent, 3.0f);
        const std::string label = muted ? std::string("MUTE") : fmt("VOL %.0f%%", double(v) * 100.0);
        small(bx + 5.0f, by + 6.0f, muted ? red : white, label);
        const float x0 = bx + 52.0f, x1 = bx + w - 6.0f;
        dl->AddRectFilled({x0, by + 8.0f}, {x1, by + 14.0f}, ImGui::GetColorU32(ImGuiCol_FrameBg));
        dl->AddRectFilled({x0, by + 8.0f}, {x0 + (x1 - x0) * v, by + 14.0f}, muted ? colDim : pal.accent);
        if (now < volArmedUntil_) small(bx + 5.0f, by - 10.0f, colDim, "V: Left/Right adjust");
    } else if (now < toastUntil_ && !toast_.empty()) {
        const std::string t = fit(toast_, float(kW) - 12.0f);
        const float tw = smallW(t) + 10.0f, tx = (float(kW) - tw) * 0.5f, ty = float(kH) - 18.0f;
        dl->AddRectFilled({tx, ty}, {tx + tw, ty + 13.0f}, IM_COL32(0, 0, 0, 215), 2.0f);
        dl->AddRect({tx, ty}, {tx + tw, ty + 13.0f}, pal.accent, 2.0f);
        small(tx + 5.0f, ty + 2.0f, white, t);
    }
}

}  // namespace osx
