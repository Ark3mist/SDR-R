#include "gui/waterfall_gl.h"

#include <algorithm>
#include <cstdio>

namespace osx {

namespace {

const char* kVert = R"(#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char* kFrag = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uData;      // R16: rows = time (ring), columns = frequency
uniform sampler2D uMap;       // 256x1 colormap
uniform float uU0, uU1;       // visible window in texture-space
uniform float uMinN, uMaxN;   // contrast window, normalised texture units
uniform int uWriteRow;        // row holding the newest line
uniform int uRows;            // number of valid rows
uniform vec2 uPane;           // target size in pixels
uniform int uTexW;
void main() {
    int texH = textureSize(uData, 0).y;
    int row = int((1.0 - vUV.y) * uPane.y);      // 0 = top = newest
    if (row >= uRows) {
        fragColor = vec4(texture(uMap, vec2(0.0, 0.5)).rgb, 1.0);
        return;
    }
    float v = (float((uWriteRow + row) % texH) + 0.5) / float(texH);
    float u = mix(uU0, uU1, vUV.x);
    // Peak-detect across the pixel footprint so narrow carriers survive
    // when many bins map onto one pixel.
    float foot = (uU1 - uU0) * float(uTexW) / uPane.x;
    int taps = int(clamp(ceil(foot), 1.0, 16.0));
    float val = 0.0;
    for (int i = 0; i < taps; ++i) {
        float du = ((float(i) + 0.5) / float(taps) - 0.5) * foot / float(uTexW);
        val = max(val, texture(uData, vec2(u + du, v)).r);
    }
    float t = clamp((val - uMinN) / max(uMaxN - uMinN, 1e-5), 0.0, 1.0);
    fragColor = vec4(texture(uMap, vec2(t, 0.5)).rgb, 1.0);
}
)";

GLuint compile(GLenum type, const char* src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[waterfall] shader compile error: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

}  // namespace

bool WaterfallGL::init() {
    const GLuint vs = compile(GL_VERTEX_SHADER, kVert);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs) return false;
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[waterfall] program link error: %s\n", log);
        return false;
    }
    locU0_ = glGetUniformLocation(program_, "uU0");
    locU1_ = glGetUniformLocation(program_, "uU1");
    locMin_ = glGetUniformLocation(program_, "uMinN");
    locMax_ = glGetUniformLocation(program_, "uMaxN");
    locWriteRow_ = glGetUniformLocation(program_, "uWriteRow");
    locRows_ = glGetUniformLocation(program_, "uRows");
    locPane_ = glGetUniformLocation(program_, "uPane");
    locTexW_ = glGetUniformLocation(program_, "uTexW");
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "uData"), 0);
    glUniform1i(glGetUniformLocation(program_, "uMap"), 1);
    glUseProgram(0);

    // Fullscreen triangle.
    const float verts[] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexSize_);
    glGenTextures(1, &dataTex_);
    glGenTextures(1, &mapTex_);
    glGenTextures(1, &fboTex_);
    glGenFramebuffers(1, &fbo_);

    glBindTexture(GL_TEXTURE_2D, mapTex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    setColormap(ColormapId::ClassicSdrpp);
    return true;
}

void WaterfallGL::destroy() {
    if (program_) glDeleteProgram(program_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    const GLuint tex[3] = {dataTex_, mapTex_, fboTex_};
    glDeleteTextures(3, tex);
    program_ = vbo_ = vao_ = fbo_ = dataTex_ = mapTex_ = fboTex_ = 0;
}

void WaterfallGL::configure(int fftSize) {
    fftSize_ = fftSize;
    reduce_ = 1;
    const int limit = std::min(maxTexSize_, 16384);
    while (fftSize / reduce_ > limit) reduce_ *= 2;
    texW_ = fftSize / reduce_;
    rowBuf_.assign(size_t(texW_), 0);

    glBindTexture(GL_TEXTURE_2D, dataTex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const std::vector<uint16_t> zeros(size_t(texW_) * kRows, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16, texW_, kRows, 0, GL_RED, GL_UNSIGNED_SHORT, zeros.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    writeRow_ = 0;
    validRows_ = 0;
}

void WaterfallGL::clear() {
    if (fftSize_ > 0) configure(fftSize_);
}

void WaterfallGL::pushLine(const float* db) {
    if (texW_ <= 0) return;
    constexpr float scale = 65535.0f / kDbRange;
    for (int x = 0; x < texW_; ++x) {
        float m = db[x * reduce_];
        for (int k = 1; k < reduce_; ++k) m = std::max(m, db[x * reduce_ + k]);
        const float q = (m - kDbFloor) * scale;
        rowBuf_[size_t(x)] = uint16_t(std::clamp(q, 0.0f, 65535.0f));
    }
    writeRow_ = (writeRow_ + kRows - 1) % kRows;  // newest row moves "up"
    validRows_ = std::min(validRows_ + 1, kRows);
    glBindTexture(GL_TEXTURE_2D, dataTex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, writeRow_, texW_, 1, GL_RED, GL_UNSIGNED_SHORT, rowBuf_.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void WaterfallGL::setColormap(ColormapId id) {
    uint8_t rgb[256 * 3];
    buildColormap(id, rgb);
    glBindTexture(GL_TEXTURE_2D, mapTex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
}

GLuint WaterfallGL::render(int w, int h, float u0, float u1, float minDb, float maxDb) {
    w = std::max(w, 1);
    h = std::max(h, 1);
    if (w != fboW_ || h != fboH_) {
        fboW_ = w;
        fboH_ = h;
        glBindTexture(GL_TEXTURE_2D, fboTex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboTex_, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(program_);
    glUniform1f(locU0_, u0);
    glUniform1f(locU1_, u1);
    glUniform1f(locMin_, (minDb - kDbFloor) / kDbRange);
    glUniform1f(locMax_, (maxDb - kDbFloor) / kDbRange);
    glUniform1i(locWriteRow_, writeRow_);
    glUniform1i(locRows_, texW_ > 0 ? validRows_ : 0);
    glUniform2f(locPane_, float(w), float(h));
    glUniform1i(locTexW_, std::max(texW_, 1));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mapTex_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, dataTex_);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    return fboTex_;
}

}  // namespace osx
