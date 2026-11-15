// OpenSDR-X entry point: Thread 4 (UI + OpenGL) lives here.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "gui/app.h"

namespace {

struct Options {
    bool autostart = false;
    bool vsync = true;
    long frames = -1;          // exit after N frames (diagnostics / CI)
    std::string screenshot;    // write a PPM of the last frame on exit
    int width = 1440, height = 860;
};

void usage() {
    std::puts("OpenSDR-X " OPENSDRX_VERSION "\n"
              "  --autostart          start the source immediately\n"
              "  --no-vsync           disable vsync (uses the 60 FPS limiter only)\n"
              "  --size WxH           initial window size\n"
              "  --frames N           run N frames, print frame timing, exit\n"
              "  --screenshot F.ppm   save the final frame (with --frames)");
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--autostart") o.autostart = true;
        else if (a == "--no-vsync") o.vsync = false;
        else if (a == "--frames") { const char* v = next(); if (!v) return false; o.frames = std::atol(v); }
        else if (a == "--screenshot") { const char* v = next(); if (!v) return false; o.screenshot = v; }
        else if (a == "--size") {
            const char* v = next();
            if (!v || std::sscanf(v, "%dx%d", &o.width, &o.height) != 2) return false;
        } else return false;
    }
    return true;
}

std::string findFont(const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> candidates;
    const fs::path exe = fs::weakly_canonical(fs::path(argv0), ec);
    if (!ec) candidates.push_back(exe.parent_path() / "res/fonts/Roboto-Medium.ttf");
    candidates.push_back(fs::current_path(ec) / "res/fonts/Roboto-Medium.ttf");
    candidates.push_back(fs::path(OPENSDRX_FONT_SRC_DIR) / "Roboto-Medium.ttf");
    for (const auto& p : candidates)
        if (fs::exists(p, ec)) return p.string();
    return {};
}

void saveScreenshot(GLFWwindow* window, const std::string& path) {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    std::vector<unsigned char> px(size_t(w) * size_t(h) * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    for (int y = h - 1; y >= 0; --y)
        f.write(reinterpret_cast<const char*>(&px[size_t(y) * size_t(w) * 3]), std::streamsize(w) * 3);
    std::printf("screenshot: %s (%dx%d)\n", path.c_str(), w, h);
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) {
        usage();
        return 2;
    }

    glfwSetErrorCallback([](int code, const char* msg) { std::fprintf(stderr, "[glfw] %d: %s\n", code, msg); });
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    GLFWwindow* window = glfwCreateWindow(opt.width, opt.height, "OpenSDR-X", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create an OpenGL 3.3 core window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(opt.vsync ? 1 : 0);

    glewExperimental = GL_TRUE;
    const GLenum glewErr = glewInit();
    // GLEW built for GLX reports NO_GLX_DISPLAY on Wayland/EGL although the
    // function pointers resolve fine; anything else is fatal.
    if (glewErr != GLEW_OK && glewErr != 4 /* GLEW_ERROR_NO_GLX_DISPLAY */) {
        std::fprintf(stderr, "glewInit failed: %s\n", reinterpret_cast<const char*>(glewGetErrorString(glewErr)));
        return 1;
    }
    while (glGetError() != GL_NO_ERROR) {}
    std::printf("OpenGL %s | %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    const std::string font = findFont(argv[0]);
    if (font.empty() || !io.Fonts->AddFontFromFileTTF(font.c_str(), 16.0f)) {
        std::fprintf(stderr, "UI font not found, using the built-in bitmap font\n");
        io.Fonts->AddFontDefault();
    }

    float dpi = 1.0f;
#ifndef __APPLE__  // macOS scales the framebuffer, not the coordinate space
    float ys = 1.0f;
    glfwGetWindowContentScale(window, &dpi, &ys);
    if (dpi <= 0.0f) dpi = 1.0f;
#endif

    int rc = 0;
    {
        osx::App app;
        if (!app.init(dpi)) {
            std::fprintf(stderr, "Renderer initialisation failed\n");
            rc = 1;
        } else {
            if (opt.autostart) app.setPlaying(true);

            using clock = std::chrono::steady_clock;
            const auto minFrame = std::chrono::microseconds(16000);
            auto last = clock::now();
            const auto t0 = last;
            long frame = 0;
            double worstMs = 0.0, cpuMs = 0.0;

            while (!glfwWindowShouldClose(window)) {
                glfwPollEvents();
                const auto work0 = clock::now();
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();
                app.frame();
                ImGui::Render();

                int fbw = 0, fbh = 0;
                glfwGetFramebufferSize(window, &fbw, &fbh);
                glViewport(0, 0, fbw, fbh);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                if (opt.frames > 0) glFinish();  // make the timing below honest
                const double work = std::chrono::duration<double, std::milli>(clock::now() - work0).count();
                if (frame > 30) {  // skip start-up (font atlas, FFTW planning)
                    cpuMs += work;
                    worstMs = std::max(worstMs, work);
                }

                ++frame;
                if (opt.frames > 0 && frame >= opt.frames) {
                    if (!opt.screenshot.empty()) saveScreenshot(window, opt.screenshot);
                    break;
                }
                glfwSwapBuffers(window);

                // 60 FPS limiter for when vsync is unavailable or disabled.
                const auto now = clock::now();
                if (now - last < minFrame) std::this_thread::sleep_until(last + minFrame);
                last = clock::now();
            }

            if (opt.frames > 0 && frame > 31) {
                const double wall = std::chrono::duration<double>(clock::now() - t0).count();
                std::printf("frames=%ld wall=%.2fs avg_fps=%.1f | build+render per frame: avg=%.2f ms worst=%.2f ms\n",
                            frame, wall, double(frame) / wall, cpuMs / double(frame - 31), worstMs);
            }
            app.shutdown();
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return rc;
}
