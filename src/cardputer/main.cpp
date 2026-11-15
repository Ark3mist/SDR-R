// OpenSDR-X for the M5Stack Cardputer Zero: 320x170, keyboard only.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdlrenderer2.h>

#include "cardputer/input.h"
#include "cardputer/ui.h"
#include "cardputer/video.h"
#include "engine/radio.h"

namespace {

volatile std::sig_atomic_t g_quit = 0;
void onSignal(int) { g_quit = 1; }

struct Options {
    osx::Video::Mode video = osx::Video::Mode::Auto;
    std::string fbdev = "/dev/fb0";
    int scale = 1, rotate = 0;
    std::string audio = "plughw:0,0";
    bool mixerClamp = true;
    std::string source = "rtlsdr";
    double freq = 100.0e6;
    bool autostart = true;
    long frames = -1;
    std::string screenshot, keys;
    int keyInterval = 4;
};

void usage() {
    std::puts(
        "cardsdr " OPENSDRX_VERSION " - OpenSDR-X for the Cardputer Zero (320x170)\n"
        "  --video auto|sdl|fbdev|none  display back end (auto: SDL window, else /dev/fb0)\n"
        "  --fbdev PATH                 framebuffer device (default /dev/fb0)\n"
        "  --rotate 0|90|180|270        framebuffer rotation (fbdev mode)\n"
        "  --scale N                    window scale for desktop testing\n"
        "  --audio DEVICE               ALSA playback device (default plughw:0,0)\n"
        "  --no-mixer-clamp             do not clamp the card's playback volume to -6 dB\n"
        "  --source rtlsdr|rtltcp|sim   boot source (falls back to sim if it cannot start)\n"
        "  --freq HZ                    boot frequency\n"
        "  --no-autostart               boot stopped\n"
        "  --frames N --screenshot F.bmp --keys \"TAB,2,DOWN,...\" --key-interval N   (diagnostics)");
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--video") {
            const std::string v = next();
            if (v == "auto") o.video = osx::Video::Mode::Auto;
            else if (v == "sdl") o.video = osx::Video::Mode::Sdl;
            else if (v == "fbdev") o.video = osx::Video::Mode::Fbdev;
            else if (v == "none") o.video = osx::Video::Mode::None;
            else return false;
        } else if (a == "--fbdev") o.fbdev = next();
        else if (a == "--rotate") o.rotate = std::atoi(next().c_str());
        else if (a == "--scale") o.scale = std::atoi(next().c_str());
        else if (a == "--audio") o.audio = next();
        else if (a == "--no-mixer-clamp") o.mixerClamp = false;
        else if (a == "--source") o.source = next();
        else if (a == "--freq") o.freq = std::atof(next().c_str());
        else if (a == "--no-autostart") o.autostart = false;
        else if (a == "--frames") o.frames = std::atol(next().c_str());
        else if (a == "--screenshot") o.screenshot = next();
        else if (a == "--keys") o.keys = next();
        else if (a == "--key-interval") o.keyInterval = std::max(1, std::atoi(next().c_str()));
        else return false;
    }
    return true;
}

std::string findFontDir(const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> candidates;
    const fs::path exe = fs::weakly_canonical(fs::path(argv0), ec);
    if (!ec) candidates.push_back(exe.parent_path() / "res/fonts");
    candidates.push_back(fs::current_path(ec) / "res/fonts");
    candidates.push_back("/usr/local/share/opensdrx/fonts");
    candidates.push_back(OPENSDRX_FONT_SRC_DIR);
    for (const auto& p : candidates)
        if (fs::exists(p / "ProggyTiny.ttf", ec)) return p.string();
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) {
        usage();
        return 2;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    osx::Video video;
    if (!video.init(opt.video, opt.scale, opt.fbdev, opt.rotate)) {
        std::fprintf(stderr, "No usable display back end. Try --video fbdev or SDL_VIDEODRIVER=kmsdrm.\n");
        return 1;
    }
    std::printf("video: %s\n", video.description().c_str());

    // Keyboard: SDL delivers keys when it owns a window; otherwise read evdev directly.
    osx::EvdevKeyboard evdev;
    if (!video.hasSdlWindow() && opt.video != osx::Video::Mode::None) {
        const int n = evdev.open();
        std::printf("keyboard: %s\n", n > 0 ? evdev.description().c_str() : "NO evdev keyboard found (check /dev/input permissions)");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {float(osx::Video::kW), float(osx::Video::kH)};
    io.MouseDrawCursor = false;
    ImGui_ImplSDLRenderer2_Init(video.renderer());

    int rc = 0;
    {
        osx::Radio radio;
        osx::CardUi ui(radio);
        if (!ui.initFonts(findFontDir(argv[0]))) std::fprintf(stderr, "pixel fonts not found, using the built-in font\n");

        radio.init(opt.audio, opt.mixerClamp);
        if (!radio.mixerReport.empty()) std::printf("%s\n", radio.mixerReport.c_str());
        std::printf("audio: %s\n", radio.audio.status().c_str());

        using SK = osx::Radio::SourceKind;
        radio.selectSource(opt.source == "sim" ? SK::Simulated : (opt.source == "rtltcp" ? SK::RtlTcp : SK::RtlSdr));
        radio.tune(opt.freq);
        if (opt.autostart && !radio.play()) {
            const std::string why = radio.lastError();
            std::printf("source: %s -> falling back to the simulator\n", why.c_str());
            radio.selectSource(SK::Simulated);
            radio.play();
            ui.toast(why + " - SIMULATOR", 4.0);
        }

        SDL_Texture* viewTex = SDL_CreateTexture(video.renderer(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                                 osx::CardUi::kW, osx::CardUi::kViewH);
        const SDL_Rect viewRect{0, osx::CardUi::kBarH, osx::CardUi::kW, osx::CardUi::kViewH};
        std::vector<osx::KeyEvent> scripted = osx::parseKeyScript(opt.keys), pending;
        size_t scriptPos = 0;

        using clock = std::chrono::steady_clock;
        auto last = clock::now();
        const auto t0 = last;
        long frame = 0;
        double workSum = 0.0, workMax = 0.0;
        float fps = 0.0f;

        while (!g_quit) {
            const auto frameStart = clock::now();

            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) g_quit = 1;
                else if (e.type == SDL_KEYDOWN) ui.onKey(osx::keyFromSdl(e));
            }
            pending.clear();
            evdev.poll(pending);
            for (const auto& k : pending) ui.onKey(k);
            if (scriptPos < scripted.size() && frame > 20 && frame % opt.keyInterval == 0) ui.onKey(scripted[scriptPos++]);

            ui.update();
            SDL_UpdateTexture(viewTex, nullptr, ui.pixels(), osx::CardUi::kW * 4);
            SDL_SetRenderDrawColor(video.renderer(), 0, 0, 0, 255);
            SDL_RenderClear(video.renderer());
            SDL_RenderCopy(video.renderer(), viewTex, nullptr, &viewRect);

            const auto now = clock::now();
            const float dt = std::chrono::duration<float>(now - last).count();
            last = now;
            io.DeltaTime = dt > 0.0f ? dt : 1.0f / 30.0f;
            if (dt > 0.0f) fps += 0.1f * (1.0f / dt - fps);
            ImGui_ImplSDLRenderer2_NewFrame();
            ImGui::NewFrame();
            ui.draw(fps);
            ImGui::Render();
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), video.renderer());
            SDL_RenderPresent(video.renderer());
            video.present();

            const double work = std::chrono::duration<double, std::milli>(clock::now() - frameStart).count();
            if (frame > 20) { workSum += work; workMax = std::max(workMax, work); }
            ++frame;
            if (opt.frames > 0 && frame >= opt.frames) break;

            std::this_thread::sleep_until(frameStart + std::chrono::microseconds(1000000 / std::max(ui.fpsCap, 1)));
        }

        if (opt.frames > 0 && frame > 21) {
            const double wall = std::chrono::duration<double>(clock::now() - t0).count();
            std::printf("frames=%ld wall=%.2fs avg_fps=%.1f | frame work: avg=%.2f ms worst=%.2f ms | audio underruns=%llu drops=%llu overruns=%llu\n",
                        frame, wall, double(frame) / wall, workSum / double(frame - 21), workMax,
                        static_cast<unsigned long long>(radio.audio.underruns()),
                        static_cast<unsigned long long>(radio.receiver.audioDrops.load()),
                        static_cast<unsigned long long>(radio.source().overruns()));
        }
        if (!opt.screenshot.empty()) std::printf("screenshot: %s %s\n", opt.screenshot.c_str(), video.saveBmp(opt.screenshot) ? "ok" : "FAILED");

        SDL_DestroyTexture(viewTex);
        radio.shutdown();
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui::DestroyContext();
    evdev.close();
    video.shutdown();
    return rc;
}
