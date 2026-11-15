// Hardware-accelerated scrolling waterfall.
//
// FFT lines are stored as raw dB (16-bit normalised) in a 2D texture used as
// a ring buffer: each new line overwrites one row via glTexSubImage2D and a
// row pointer advances, so nothing is ever memmoved. A fragment shader does
// the scroll (row pointer + modulo), zoom/pan (texture-space window), dB
// contrast mapping and colormap lookup, which means colormap, contrast and
// zoom changes re-colour the whole history instantly.
#pragma once

#include <cstdint>
#include <vector>

#include <GL/glew.h>

#include "gui/colormaps.h"

namespace osx {

class WaterfallGL {
public:
    static constexpr int kRows = 1024;
    static constexpr float kDbFloor = -150.0f;  // texture encodes [kDbFloor, kDbFloor + kDbRange]
    static constexpr float kDbRange = 160.0f;

    bool init();
    void destroy();

    // (Re)allocate for an FFT size. Clears history.
    void configure(int fftSize);
    void clear();
    int fftSize() const { return fftSize_; }

    // Append one fft-shifted dB line of fftSize() bins (newest row on top).
    void pushLine(const float* db);

    void setColormap(ColormapId id);

    // Renders the visible window [u0, u1] (fractions of the full bandwidth)
    // into an offscreen target of w x h pixels; returns its colour texture.
    GLuint render(int w, int h, float u0, float u1, float minDb, float maxDb);

private:
    GLuint program_ = 0, vao_ = 0, vbo_ = 0;
    GLuint dataTex_ = 0, mapTex_ = 0, fbo_ = 0, fboTex_ = 0;
    GLint locU0_ = -1, locU1_ = -1, locMin_ = -1, locMax_ = -1, locWriteRow_ = -1;
    GLint locRows_ = -1, locPane_ = -1, locTexW_ = -1;
    int fftSize_ = 0, texW_ = 0, reduce_ = 1;
    int writeRow_ = 0, validRows_ = 0;
    int fboW_ = 0, fboH_ = 0;
    int maxTexSize_ = 4096;
    std::vector<uint16_t> rowBuf_;
};

}  // namespace osx
