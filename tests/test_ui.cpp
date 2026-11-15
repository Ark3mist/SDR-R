// Keyboard-controller tests for the 320x170 UI (no display needed).
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <imgui.h>

#include "cardputer/ui.h"

using namespace osx;

static int failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            ++failures;                                      \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
        }                                                    \
    } while (0)

static KeyEvent K(Key k) { KeyEvent e; e.key = k; if (k == Key::Space) e.text = ' '; return e; }
static KeyEvent C(char c, bool shift = false, bool ctrl = false) {
    KeyEvent e;
    e.key = Key::Char;
    e.ch = c;
    e.shift = shift;
    e.ctrl = ctrl;
    e.text = shiftedChar(c, shift);
    return e;
}
static void type(CardUi& ui, const std::string& s) {
    for (char c : s) {
        const bool upper = c >= 'A' && c <= 'Z';
        ui.onKey(c == ' ' ? K(Key::Space) : C(upper ? char(c - 'A' + 'a') : c, upper));
    }
}

struct Snapshot {
    Demod mode; double bw, vfo; float gain, sq, vol; bool squelch, mute, drawer; int zoom, digit; NrPreset nr;
    bool operator==(const Snapshot&) const = default;
};
static Snapshot snap(Radio& r, CardUi& ui) {
    return {r.rx().mode, r.rx().bandwidthHz, r.vfoHz(), r.source().gain(), r.rx().squelchDb, r.audio.volume.load(),
            r.rx().squelch, r.audio.mute.load(), ui.drawerOpen(), ui.zoom(), ui.tuningDigit(), nrPreset(r.rx())};
}

int main() {
    ImGui::CreateContext();
    const std::string dir = std::filesystem::temp_directory_path().string() + "/osx_ui_test";
    std::filesystem::remove_all(dir);
    const std::string path = dir + "/bookmarks.json";
    BookmarkStore store(path);
    store.load();
    Radio radio;
    CardUi ui(radio, store);

    // ---- 1. K quick-save + rename box: focus lock -----------------------------------
    radio.recall(119.45e6, Demod::AM, 8330);
    const size_t before = store.items().size();
    ui.onKey(C('k'));
    CHECK(ui.modal() == CardUi::Modal::Rename && store.items().size() == before + 1, "K saves and opens the rename box");
    CHECK(ui.modalText() == "119.450 MHz AM", "default name '%s'", ui.modalText().c_str());
    const Bookmark& saved = store.items().back();
    CHECK(saved.frequencyHz == 119450000 && saved.demodMode == "AM" && saved.bandwidthHz == 8330, "saved freq/mode/bw");
    const Snapshot s0 = snap(radio, ui);
    // Every RF hotkey, Tab, Space, digits and shifted symbols, typed into the box.
    for (int i = 0; i < 14; ++i) ui.onKey(K(Key::Backspace));
    type(ui, "KLIA dbgsknrzav");
    ui.onKey(C('=', true));   // '+'
    ui.onKey(C('-'));
    ui.onKey(C('1'));
    ui.onKey(K(Key::Tab));
    ui.onKey(C('d', true));   // 'D'
    CHECK(snap(radio, ui) == s0, "a hotkey fired while typing a bookmark name (focus lock broken)");
    CHECK(ui.modal() == CardUi::Modal::Rename && ui.modalText() == "KLIA dbgsknrzav+-1D", "typed text '%s'", ui.modalText().c_str());
    for (int i = 0; i < 14; ++i) ui.onKey(K(Key::Backspace));
    type(ui, "Approach");
    ui.onKey(K(Key::Enter));
    CHECK(ui.modal() == CardUi::Modal::None && store.items().back().name == "KLIA Approach", "renamed to '%s'", store.items().back().name.c_str());
    CHECK(snap(radio, ui) == s0, "state changed by the rename");

    // Esc keeps the default name.
    ui.onKey(C('s', false, true));  // Ctrl+S
    CHECK(ui.modal() == CardUi::Modal::Rename, "Ctrl+S opens the rename box");
    type(ui, "xyz");
    ui.onKey(K(Key::Esc));
    CHECK(store.items().back().name == "119.450 MHz AM", "Esc keeps the default name");
    store.remove(store.items().size() - 1);

    // ---- 2. frequency entry: focus lock + tuning ---------------------------------------
    ui.onKey(C('1'));
    CHECK(ui.modal() == CardUi::Modal::FreqEntry, "digit opens frequency entry");
    const Snapshot s1 = snap(radio, ui);
    type(ui, "dgbz");
    CHECK(snap(radio, ui) == s1 && ui.modalText() == "1", "letters ignored in frequency entry");
    type(ui, "21.5");
    ui.onKey(K(Key::Enter));
    CHECK(std::fabs(radio.vfoHz() - 121.5e6) < 1 && radio.rx().mode == Demod::AM, "direct entry tuned %.0f", radio.vfoHz());

    // ---- 3. bookmarks tab: Fn-free navigation and recall (freq + mode + bw) ---------------
    radio.recall(100e6, Demod::WFM, 200e3);
    ui.onKey(K(Key::Tab));
    ui.onKey(C('4'));
    CHECK(ui.drawerOpen() && ui.tab() == CardUi::kBookmarks, "Tab, 4 -> bookmarks");
    for (int i = 0; i < 40 && ui.selectedLabel() != "KLIA Approach"; ++i) ui.onKey(C('.'));  // '.' = Down, no Fn
    CHECK(ui.selectedLabel() == "KLIA Approach", "navigated to '%s'", ui.selectedLabel().c_str());
    ui.onKey(K(Key::Enter));
    CHECK(std::fabs(radio.vfoHz() - 119.45e6) < 1 && radio.rx().mode == Demod::AM && radio.rx().bandwidthHz == 8330,
          "recall: %.0f Hz %s %.0f Hz", radio.vfoHz(), demodName(radio.rx().mode), radio.rx().bandwidthHz);
    ui.onKey(C(';'));  // ';' = Up
    const std::string above = ui.selectedLabel();
    ui.onKey(C('.'));
    CHECK(ui.selectedLabel() == "KLIA Approach" && above != "KLIA Approach", "';' / '.' move the selection");

    // E renames the selected bookmark.
    ui.onKey(C('e'));
    CHECK(ui.modal() == CardUi::Modal::Rename && ui.modalText() == "KLIA Approach", "E opens rename");
    type(ui, " 2");
    ui.onKey(K(Key::Enter));
    CHECK(ui.selectedLabel() == "KLIA Approach 2", "rename shows in the list: '%s'", ui.selectedLabel().c_str());

    // Delete with confirmation: any key cancels, Enter deletes.
    const size_t n0 = store.items().size();
    ui.onKey(K(Key::Backspace));
    CHECK(ui.modal() == CardUi::Modal::ConfirmDelete, "Backspace asks for confirmation");
    ui.onKey(C('n'));
    CHECK(ui.modal() == CardUi::Modal::None && store.items().size() == n0, "delete cancelled");
    ui.onKey(K(Key::Backspace));
    ui.onKey(K(Key::Enter));
    CHECK(store.items().size() == n0 - 1, "delete confirmed");

    // Persisted to disk.
    BookmarkStore reread(path);
    reread.load();
    CHECK(reread.items().size() == n0 - 1, "deletion persisted");

    // ---- 4. tab cycling without Fn -------------------------------------------------------
    ui.onKey(C(']'));
    CHECK(ui.tab() == CardUi::kScanner, "] -> next tab");
    ui.onKey(C('['));
    ui.onKey(C('['));
    CHECK(ui.tab() == CardUi::kDsp, "[ -> previous tab");
    ui.onKey(C('1'));
    ui.onKey(C('['));
    CHECK(ui.tab() == CardUi::kDisplay, "[ wraps from the first tab to the last");

    // ---- 5. ',' and '/' adjust the selected setting ---------------------------------------
    ui.onKey(C('2'));
    for (int i = 0; i < 30 && ui.selectedLabel() != "Volume [+/-]"; ++i) ui.onKey(C('.'));
    const float v0 = radio.audio.volume.load();
    ui.onKey(C('/'));
    CHECK(std::fabs(radio.audio.volume.load() - (v0 + 0.05f)) < 1e-4f, "'/' = Right raises the volume");
    ui.onKey(C(','));
    CHECK(std::fabs(radio.audio.volume.load() - v0) < 1e-4f, "',' = Left lowers it again");
    ui.onKey(K(Key::Esc));
    CHECK(!ui.drawerOpen(), "Esc closes the drawer");

    // ---- 6. N cycles the interference presets ---------------------------------------------
    const NrPreset seq[] = {NrPreset::NB, NrPreset::NBNotch, NrPreset::Full, NrPreset::Off};
    for (NrPreset p : seq) {
        ui.onKey(C('n'));
        CHECK(nrPreset(radio.rx()) == p, "N -> %s", nrPresetName(p));
    }

    // ---- 7. master volume keys ---------------------------------------------------------------
    radio.audio.volume = 0.75f;
    ui.onKey(C('='));
    ui.onKey(C('=', true));  // '+'
    CHECK(std::fabs(radio.audio.volume.load() - 0.85f) < 1e-4f, "= and + raise 5 %% each");
    ui.onKey(C('-'));
    ui.onKey(C('-', true));  // '_'
    ui.onKey(C('-'));
    CHECK(std::fabs(radio.audio.volume.load() - 0.70f) < 1e-4f, "- and _ lower 5 %% each");
    ui.onKey(C('v'));
    const double vfoBefore = radio.vfoHz();
    ui.onKey(K(Key::Right));
    CHECK(std::fabs(radio.audio.volume.load() - 0.75f) < 1e-4f && radio.vfoHz() == vfoBefore, "V then Right = volume, not tuning");
    for (int i = 0; i < 30; ++i) ui.onKey(C('='));
    CHECK(radio.audio.volume.load() == 1.0f, "volume clamps at 100 %%");
    ui.onKey(K(Key::Space));
    CHECK(radio.audio.mute.load(), "Space mutes");
    ui.onKey(K(Key::Space));

    // ---- 8. drawer closed: Fn-free tuning ------------------------------------------------------
    radio.recall(145.5e6, Demod::NFM, 12500);
    ui.onKey(K(Key::Down));
    ui.onKey(K(Key::Down));  // digit 3 -> 1 (10 Hz)
    CHECK(ui.tuningDigit() == 1, "step cursor %d", ui.tuningDigit());
    ui.onKey(K(Key::Down));
    CHECK(ui.tuningDigit() == 1, "step cursor stops at 10 Hz");
    for (int i = 0; i < 4; ++i) ui.onKey(K(Key::Up));
    ui.onKey(K(Key::Up));
    CHECK(ui.tuningDigit() == 5, "step cursor stops at 100 kHz");
    ui.onKey(C(';'));
    CHECK(ui.tuningDigit() == 1, "';' wraps the step cursor");
    ui.onKey(C(';'));
    ui.onKey(C(';'));  // 1 kHz
    const double f0 = radio.vfoHz();
    ui.onKey(C('/'));
    ui.onKey(C('/'));
    ui.onKey(C(','));
    CHECK(std::fabs(radio.vfoHz() - (f0 + 1000.0)) < 0.5, "',' '/' tune by the step (%.0f)", radio.vfoHz() - f0);

    // ---- 9. D cycles all modes including DSB ------------------------------------------------------
    radio.setMode(Demod::WFM);
    std::string order;
    for (int i = 0; i < int(Demod::Count); ++i) {
        ui.onKey(C('d'));
        order += std::string(demodName(radio.rx().mode)) + " ";
    }
    CHECK(order == "NFM AM SAM USB LSB DSB CW RAW WFM ", "mode cycle: %s", order.c_str());

    std::filesystem::remove_all(dir);
    ImGui::DestroyContext();
    std::printf(failures ? "%d UI FAILURE(S)\n" : "all UI tests passed\n", failures);
    return failures ? 1 : 0;
}
