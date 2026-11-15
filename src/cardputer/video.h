// Display back end. Everything is rendered by SDL's software renderer into
// one 320x170 ARGB surface; that surface is then presented either through an
// SDL window (KMSDRM / X11 / Wayland) or by writing straight into a Linux
// framebuffer device (/dev/fb0), which needs no display server and no GPU.
#pragma once

#include <cstdint>
#include <string>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Surface;

namespace osx {

class Video {
public:
    static constexpr int kW = 320, kH = 170;
    enum class Mode { Auto, Sdl, Fbdev, None };

    ~Video();
    bool init(Mode mode, int scale, const std::string& fbPath, int rotateDeg);
    void shutdown();

    SDL_Renderer* renderer() const { return renderer_; }
    void present();
    bool saveBmp(const std::string& path) const;
    bool hasSdlWindow() const { return window_ != nullptr; }
    bool isFbdev() const { return fbMem_ != nullptr; }
    const std::string& description() const { return desc_; }

private:
    bool openFbdev(const std::string& path);
    void presentFbdev();

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Surface* surface_ = nullptr;
    int scale_ = 1, rotate_ = 0;
    std::string desc_;

    int fbFd_ = -1;
    uint8_t* fbMem_ = nullptr;
    size_t fbLen_ = 0;
    int fbW_ = 0, fbH_ = 0, fbBpp_ = 0, fbStride_ = 0;
    int rOff_ = 0, gOff_ = 0, bOff_ = 0, rLen_ = 0, gLen_ = 0, bLen_ = 0;
};

}  // namespace osx
