#include "cardputer/video.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <SDL.h>

#ifdef __linux__
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace osx {

Video::~Video() { shutdown(); }

bool Video::init(Mode mode, int scale, const std::string& fbPath, int rotateDeg) {
    scale_ = std::max(1, scale);
    rotate_ = ((rotateDeg % 360) + 360) % 360;

    bool windowOk = false;
    if (mode == Mode::Auto || mode == Mode::Sdl) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0) {
            const bool kms = SDL_GetCurrentVideoDriver() && std::strcmp(SDL_GetCurrentVideoDriver(), "KMSDRM") == 0;
            window_ = SDL_CreateWindow("OpenSDR-X Cardputer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       kW * scale_, kH * scale_, kms ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            if (window_ && SDL_GetWindowSurface(window_)) {
                windowOk = true;
                SDL_ShowCursor(SDL_DISABLE);
                desc_ = std::string("SDL window (") + SDL_GetCurrentVideoDriver() + ")";
            } else {
                std::fprintf(stderr, "[video] SDL window failed: %s\n", SDL_GetError());
                if (window_) SDL_DestroyWindow(window_);
                window_ = nullptr;
                SDL_Quit();
            }
        } else {
            std::fprintf(stderr, "[video] SDL video init failed: %s\n", SDL_GetError());
        }
        if (!windowOk && mode == Mode::Sdl) return false;
    }
    if (!windowOk) {
        // No SDL display: SDL is only needed for surfaces + the software renderer.
        setenv("SDL_VIDEODRIVER", "dummy", 1);
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
            std::fprintf(stderr, "[video] SDL init failed: %s\n", SDL_GetError());
            return false;
        }
        if (mode == Mode::Auto || mode == Mode::Fbdev) {
            if (!openFbdev(fbPath) && mode == Mode::Fbdev) return false;
            if (!fbMem_ && mode == Mode::Auto) {
                std::fprintf(stderr, "[video] no SDL display and no usable %s\n", fbPath.c_str());
                return false;
            }
        } else {
            desc_ = "offscreen";
        }
    }

    surface_ = SDL_CreateRGBSurfaceWithFormat(0, kW, kH, 32, SDL_PIXELFORMAT_ARGB8888);
    renderer_ = surface_ ? SDL_CreateSoftwareRenderer(surface_) : nullptr;
    if (!renderer_) {
        std::fprintf(stderr, "[video] software renderer failed: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

void Video::shutdown() {
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (surface_) SDL_FreeSurface(surface_);
    if (window_) SDL_DestroyWindow(window_);
    renderer_ = nullptr;
    surface_ = nullptr;
    window_ = nullptr;
#ifdef __linux__
    if (fbMem_) {
        std::memset(fbMem_, 0, fbLen_);
        munmap(fbMem_, fbLen_);
        fbMem_ = nullptr;
    }
    if (fbFd_ >= 0) ::close(fbFd_);
    fbFd_ = -1;
#endif
    if (SDL_WasInit(0)) SDL_Quit();
}

bool Video::saveBmp(const std::string& path) const { return surface_ && SDL_SaveBMP(surface_, path.c_str()) == 0; }

void Video::present() {
    if (window_) {
        SDL_Surface* ws = SDL_GetWindowSurface(window_);
        if (!ws) return;
        const int s = std::max(1, std::min(ws->w / kW, ws->h / kH));
        SDL_Rect dst{(ws->w - kW * s) / 2, (ws->h - kH * s) / 2, kW * s, kH * s};
        if (s == 1) SDL_BlitSurface(surface_, nullptr, ws, &dst);
        else SDL_BlitScaled(surface_, nullptr, ws, &dst);
        SDL_UpdateWindowSurface(window_);
    } else if (fbMem_) {
        presentFbdev();
    }
}

#ifdef __linux__
bool Video::openFbdev(const std::string& path) {
    fbFd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fbFd_ < 0) {
        std::fprintf(stderr, "[video] cannot open %s\n", path.c_str());
        return false;
    }
    fb_var_screeninfo v{};
    fb_fix_screeninfo f{};
    if (ioctl(fbFd_, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(fbFd_, FBIOGET_FSCREENINFO, &f) < 0 ||
        (v.bits_per_pixel != 16 && v.bits_per_pixel != 32)) {
        std::fprintf(stderr, "[video] %s: unsupported framebuffer format\n", path.c_str());
        ::close(fbFd_);
        fbFd_ = -1;
        return false;
    }
    fbW_ = int(v.xres);
    fbH_ = int(v.yres);
    fbBpp_ = int(v.bits_per_pixel);
    fbStride_ = int(f.line_length);
    rOff_ = int(v.red.offset); rLen_ = int(v.red.length);
    gOff_ = int(v.green.offset); gLen_ = int(v.green.length);
    bOff_ = int(v.blue.offset); bLen_ = int(v.blue.length);
    fbLen_ = size_t(fbStride_) * size_t(v.yres_virtual ? v.yres_virtual : v.yres);
    void* mem = mmap(nullptr, fbLen_, PROT_READ | PROT_WRITE, MAP_SHARED, fbFd_, 0);
    if (mem == MAP_FAILED) {
        std::fprintf(stderr, "[video] mmap %s failed\n", path.c_str());
        ::close(fbFd_);
        fbFd_ = -1;
        return false;
    }
    fbMem_ = static_cast<uint8_t*>(mem);
    std::memset(fbMem_, 0, fbLen_);
    // A portrait panel (170x320) needs the image turned unless told otherwise.
    if (rotate_ == 0 && fbW_ < kW && fbH_ >= kW) rotate_ = 90;
    char d[128];
    std::snprintf(d, sizeof(d), "fbdev %s %dx%d %dbpp rot%d", path.c_str(), fbW_, fbH_, fbBpp_, rotate_);
    desc_ = d;
    return true;
}

void Video::presentFbdev() {
    const bool swap = (rotate_ == 90 || rotate_ == 270);
    const int outW = swap ? kH : kW, outH = swap ? kW : kH;
    const int s = std::max(1, std::min(fbW_ / outW, fbH_ / outH));
    const int ox = std::max(0, (fbW_ - outW * s) / 2), oy = std::max(0, (fbH_ - outH * s) / 2);
    const auto* src = static_cast<const uint32_t*>(surface_->pixels);
    const int srcPitch = surface_->pitch / 4;
    const int bytes = fbBpp_ / 8;

    for (int y = 0; y < outH; ++y) {
        for (int x = 0; x < outW; ++x) {
            int sx = x, sy = y;
            switch (rotate_) {
                case 90: sx = y; sy = kH - 1 - x; break;
                case 180: sx = kW - 1 - x; sy = kH - 1 - y; break;
                case 270: sx = kW - 1 - y; sy = x; break;
                default: break;
            }
            const uint32_t p = src[sy * srcPitch + sx];
            const uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            const uint32_t out = ((r >> (8 - rLen_)) << rOff_) | ((g >> (8 - gLen_)) << gOff_) | ((b >> (8 - bLen_)) << bOff_);
            for (int dy = 0; dy < s; ++dy) {
                const int fy = oy + y * s + dy;
                if (fy >= fbH_) break;
                uint8_t* row = fbMem_ + size_t(fy) * size_t(fbStride_);
                for (int dx = 0; dx < s; ++dx) {
                    const int fx = ox + x * s + dx;
                    if (fx >= fbW_) break;
                    if (bytes == 2) { const uint16_t v16 = uint16_t(out); std::memcpy(row + fx * 2, &v16, 2); }
                    else std::memcpy(row + fx * 4, &out, 4);
                }
            }
        }
    }
}
#else
bool Video::openFbdev(const std::string&) { return false; }
void Video::presentFbdev() {}
#endif

}  // namespace osx
