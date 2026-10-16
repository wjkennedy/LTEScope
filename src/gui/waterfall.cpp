#include "gui/waterfall.h"

#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#include <algorithm>
#include <cmath>

// Cap the waterfall texture width: FFT sizes (32768/65536) exceed the GPU's
// GL_MAX_TEXTURE_SIZE (often 16384), which fails the texture allocation and
// breaks the waterfall. A few thousand columns is far more than any display
// needs, so we max-pool the bins down to this width when the FFT is larger.
static constexpr int kMaxTexW = 4096;

const char* const kColorPaletteLabels[kColorPaletteCount] = {
    "Classic",
    "Viridis",
    "Magma",
    "Grayscale",
    "Blue / white",
};

namespace {
void ramp(float t, const float* stops, int stopCount,
          uint8_t& r, uint8_t& g, uint8_t& b)
{
    t = std::clamp(t, 0.0f, 1.0f);
    float x = t * (float)(stopCount - 1);
    int i = (int)x;
    if (i > stopCount - 2)
        i = stopCount - 2;
    float f = x - (float)i;
    const float* a = stops + i * 3;
    const float* z = stops + (i + 1) * 3;
    float rr = a[0] + (z[0] - a[0]) * f;
    float gg = a[1] + (z[1] - a[1]) * f;
    float bb = a[2] + (z[2] - a[2]) * f;
    r = (uint8_t)std::lround(rr * 255.0f);
    g = (uint8_t)std::lround(gg * 255.0f);
    b = (uint8_t)std::lround(bb * 255.0f);
}
}

Waterfall::~Waterfall()
{
    if (tex_)
        glDeleteTextures(1, &tex_);
}

void Waterfall::ensureTexture(int bins)
{
    if (tex_ && bins == texW_)
        return;

    if (tex_)
    {
        glDeleteTextures(1, &tex_);
        tex_ = 0;
    }

    texW_ = bins;
    writePtr_ = 0;
    filled_ = false;
    paletteDirty_ = false;

    std::vector<uint8_t> zeros((size_t)texW_ * texH_ * 4, 0);
    intensityBuf_.assign((size_t)texW_ * texH_, 0);
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texW_, texH_, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, zeros.data());
}

void Waterfall::colorForPalette(ColorPalette palette, float t,
                                uint8_t& r, uint8_t& g, uint8_t& b)
{
    t = std::clamp(t, 0.0f, 1.0f);
    static const float classic[] = {
        0.00f, 0.00f, 0.00f, // black
        0.00f, 0.00f, 0.55f, // dark blue
        0.00f, 0.55f, 0.90f, // cyan
        0.00f, 0.85f, 0.20f, // green
        0.95f, 0.90f, 0.00f, // yellow
        0.95f, 0.20f, 0.00f, // red
        1.00f, 1.00f, 1.00f, // white
    };
    static const float viridis[] = {
        0.267f, 0.005f, 0.329f,
        0.283f, 0.141f, 0.458f,
        0.254f, 0.265f, 0.530f,
        0.207f, 0.372f, 0.553f,
        0.164f, 0.471f, 0.558f,
        0.128f, 0.567f, 0.551f,
        0.135f, 0.659f, 0.518f,
        0.267f, 0.749f, 0.441f,
        0.478f, 0.821f, 0.318f,
        0.741f, 0.873f, 0.150f,
    };
    static const float magma[] = {
        0.001f, 0.000f, 0.014f,
        0.088f, 0.044f, 0.224f,
        0.232f, 0.060f, 0.438f,
        0.404f, 0.057f, 0.557f,
        0.588f, 0.072f, 0.514f,
        0.759f, 0.170f, 0.386f,
        0.910f, 0.360f, 0.200f,
        0.987f, 0.617f, 0.234f,
        0.997f, 0.850f, 0.475f,
        0.987f, 0.991f, 0.750f,
    };
    static const float blueWhite[] = {
        0.000f, 0.000f, 0.000f,
        0.000f, 0.070f, 0.220f,
        0.000f, 0.280f, 0.650f,
        0.100f, 0.650f, 1.000f,
        0.800f, 0.940f, 1.000f,
        1.000f, 1.000f, 1.000f,
    };

    switch (palette)
    {
        case ColorPalette::Viridis:
            ramp(t, &viridis[0], 10, r, g, b);
            return;
        case ColorPalette::Magma:
            ramp(t, &magma[0], 10, r, g, b);
            return;
        case ColorPalette::Grayscale:
        {
            uint8_t v = (uint8_t)std::lround(t * 255.0f);
            r = g = b = v;
            return;
        }
        case ColorPalette::BlueWhite:
            ramp(t, &blueWhite[0], 6, r, g, b);
            return;
        case ColorPalette::Classic:
        default:
            ramp(t, &classic[0], 7, r, g, b);
            return;
    }
}

void Waterfall::setPalette(ColorPalette palette)
{
    if (palette == palette_)
        return;

    palette_ = palette;
    paletteDirty_ = true;
}

void Waterfall::uploadRecoloredTexture()
{
    if (!tex_ || texW_ <= 0 || intensityBuf_.size() != (size_t)texW_ * texH_)
        return;

    std::vector<uint8_t> rgba((size_t)texW_ * texH_ * 4);
    for (size_t i = 0; i < intensityBuf_.size(); ++i)
    {
        uint8_t r, g, b;
        colorForPalette(palette_, intensityBuf_[i] / 255.0f, r, g, b);
        rgba[i * 4 + 0] = r;
        rgba[i * 4 + 1] = g;
        rgba[i * 4 + 2] = b;
        rgba[i * 4 + 3] = 255;
    }

    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texW_, texH_, GL_RGBA,
                    GL_UNSIGNED_BYTE, rgba.data());
    paletteDirty_ = false;
}

void Waterfall::addRow(const float* db, int n, float dbMin, float dbMax)
{
    if (n <= 0)
        return;
    int w = (n > kMaxTexW) ? kMaxTexW : n; // texture width (capped)
    ensureTexture(w);

    rowBuf_.resize((size_t)texW_ * 4);
    float span = (dbMax > dbMin) ? (dbMax - dbMin) : 1.0f;
    for (int i = 0; i < texW_; ++i)
    {
        float v;
        if (w == n)
        {
            v = db[i];
        }
        else
        {
            // Max-pool the FFT bins covering this output column (keeps peaks).
            int lo = (int)((long long)i * n / w);
            int hi = (int)((long long)(i + 1) * n / w);
            if (hi <= lo) hi = lo + 1;
            if (hi > n) hi = n;
            v = db[lo];
            for (int k = lo + 1; k < hi; ++k)
                if (db[k] > v) v = db[k];
        }
        float t = (v - dbMin) / span;
        t = std::clamp(t, 0.0f, 1.0f);
        uint8_t r, g, b;
        colorForPalette(palette_, t, r, g, b);
        rowBuf_[i * 4 + 0] = r;
        rowBuf_[i * 4 + 1] = g;
        rowBuf_[i * 4 + 2] = b;
        rowBuf_[i * 4 + 3] = 255;
    }

    // Newest row goes to the (decremented) write pointer so memory index
    // increasing == older, which maps cleanly to a top-to-bottom display.
    writePtr_ = (writePtr_ - 1 + texH_) % texH_;
    if (intensityBuf_.size() == (size_t)texW_ * texH_)
    {
        uint8_t* out = intensityBuf_.data() + (size_t)writePtr_ * texW_;
        for (int i = 0; i < texW_; ++i)
        {
            float v;
            if (w == n)
            {
                v = db[i];
            }
            else
            {
                int lo = (int)((long long)i * n / w);
                int hi = (int)((long long)(i + 1) * n / w);
                if (hi <= lo) hi = lo + 1;
                if (hi > n) hi = n;
                v = db[lo];
                for (int k = lo + 1; k < hi; ++k)
                    if (db[k] > v) v = db[k];
            }
            float t = std::clamp((v - dbMin) / span, 0.0f, 1.0f);
            out[i] = (uint8_t)std::lround(t * 255.0f);
        }
    }
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, writePtr_, texW_, 1, GL_RGBA,
                    GL_UNSIGNED_BYTE, rowBuf_.data());
    if (writePtr_ == 0)
        filled_ = true;
}

void Waterfall::draw(const ImVec2& size, float uMin, float uMax,
                     float xFracLo, float xFracHi)
{
    if (!tex_ || size.x <= 1.0f || size.y <= 1.0f)
    {
        ImGui::Dummy(size);
        return;
    }

    if (paletteDirty_)
        uploadRecoloredTexture();

    if (uMax <= uMin)
    {
        uMin = 0.0f;
        uMax = 1.0f;
    }

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##waterfall", size);

    // Black background over the whole panel so margins (when the image is
    // narrower than the panel, e.g. zoomed out) stay clean rather than showing
    // stale/stretched texture.
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y),
                      IM_COL32(0, 0, 0, 255));

    xFracLo = std::clamp(xFracLo, 0.0f, 1.0f);
    xFracHi = std::clamp(xFracHi, 0.0f, 1.0f);
    if (xFracHi <= xFracLo)
        return; // nothing visible: leave the black background

    float dx0 = p0.x + size.x * xFracLo;
    float dx1 = p0.x + size.x * xFracHi;

    float H = (float)texH_;
    float drawH = size.y;
    float yOff = p0.y;
    if (size.y > H)
    {
        // Upscaling: snap to integer multiples so each texture row maps to
        // exactly N screen pixels with no sub-pixel blending.
        float rowScale = std::floor(size.y / H);
        drawH = rowScale * H;
        yOff = p0.y + (size.y - drawH) * 0.5f;
    }
    float vSplit = (float)writePtr_ / H;
    float topFrac = 1.0f - vSplit;

    ImTextureID tid = (ImTextureID)(intptr_t)tex_;

    // Top slice: tex rows [writePtr_ .. texH_-1].
    float topH = drawH * topFrac;
    ImVec2 a0 = ImVec2(dx0, yOff);
    ImVec2 a1 = ImVec2(dx1, yOff + topH);
    dl->AddImage(tid, a0, a1, ImVec2(uMin, vSplit), ImVec2(uMax, 1.0f));

    // Bottom slice: tex rows [0 .. writePtr_-1].
    if (vSplit > 0.0f)
    {
        ImVec2 b0 = ImVec2(dx0, yOff + topH);
        ImVec2 b1 = ImVec2(dx1, yOff + drawH);
        dl->AddImage(tid, b0, b1, ImVec2(uMin, 0.0f), ImVec2(uMax, vSplit));
    }
}

void Waterfall::clear()
{
    if (tex_)
    {
        glDeleteTextures(1, &tex_);
        tex_ = 0;
    }
    texW_ = 0;
    writePtr_ = 0;
    filled_ = false;
    paletteDirty_ = false;
    intensityBuf_.clear();
}
