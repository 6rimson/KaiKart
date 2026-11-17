// The launcher's drawing (ui.h).
#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>

#include <switch.h>
#include <turbojpeg.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imstb_truetype.h"  // stb_truetype, as Dear ImGui carries it (kaihen/build.sh points at it)

namespace ui {
namespace {

Framebuffer g_framebuffer;
bool g_framebuffer_ready = false;
uint32_t* g_pixels = nullptr;
int g_stride = 0;  // in pixels

stbtt_fontinfo g_font;
bool g_font_ready = false;

struct Glyph {
    int width = 0, height = 0, x_offset = 0, y_offset = 0, advance = 0;
    std::vector<uint8_t> coverage;
};
std::map<std::pair<int, uint32_t>, Glyph> g_glyphs;  // (size, code point)

inline void Blend(uint32_t& pixel, Color color, unsigned alpha) {
    alpha = alpha * (color >> 24) / 255;
    if (alpha == 0) return;
    if (alpha >= 255) {
        pixel = color | 0xFF000000u;
        return;
    }
    const unsigned inverse = 255 - alpha;
    const unsigned r = ((color & 0xFF) * alpha + (pixel & 0xFF) * inverse) / 255;
    const unsigned g = (((color >> 8) & 0xFF) * alpha + ((pixel >> 8) & 0xFF) * inverse) / 255;
    const unsigned b = (((color >> 16) & 0xFF) * alpha + ((pixel >> 16) & 0xFF) * inverse) / 255;
    pixel = r | g << 8 | b << 16 | 0xFF000000u;
}

inline void Span(int x0, int x1, int y, Color color) {
    if (y < 0 || y >= kHeight || !g_pixels) return;
    x0 = std::max(x0, 0);
    x1 = std::min(x1, kWidth);
    uint32_t* row = g_pixels + y * g_stride;
    if ((color >> 24) == 255) {
        for (int x = x0; x < x1; ++x) row[x] = color;
    } else {
        for (int x = x0; x < x1; ++x) Blend(row[x], color, 255);
    }
}

// UTF-8 to code points; anything malformed comes out as '?'.
std::vector<uint32_t> Decode(const std::string& text) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (extra < 0 || i + extra >= text.size() + (extra == 0 ? 1 : 0)) {
            out.push_back('?');
            ++i;
            continue;
        }
        uint32_t cp = extra == 0 ? c : c & (0x3F >> extra);
        for (int k = 1; k <= extra; ++k) cp = cp << 6 | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        out.push_back(cp);
        i += size_t(extra) + 1;
    }
    return out;
}

const Glyph& GetGlyph(int size, uint32_t cp) {
    auto key = std::make_pair(size, cp);
    auto it = g_glyphs.find(key);
    if (it != g_glyphs.end()) return it->second;
    Glyph glyph;
    if (g_font_ready) {
        const float scale = stbtt_ScaleForPixelHeight(&g_font, float(size));
        int advance, bearing;
        stbtt_GetCodepointHMetrics(&g_font, int(cp), &advance, &bearing);
        glyph.advance = int(std::lround(advance * scale));
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&g_font, int(cp), scale, scale, &x0, &y0, &x1, &y1);
        glyph.width = std::max(0, x1 - x0);
        glyph.height = std::max(0, y1 - y0);
        glyph.x_offset = x0;
        glyph.y_offset = y0;
        glyph.coverage.resize(size_t(glyph.width) * glyph.height);
        if (!glyph.coverage.empty()) {
            stbtt_MakeCodepointBitmap(&g_font, glyph.coverage.data(), glyph.width, glyph.height, glyph.width, scale,
                                      scale, int(cp));
        }
    }
    return g_glyphs.emplace(key, std::move(glyph)).first->second;
}

int Ascent(int size) {
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &gap);
    return int(std::lround(ascent * stbtt_ScaleForPixelHeight(&g_font, float(size))));
}

// Rows of a rounded rectangle: how far each row's ends are pulled in by the corners.
int Inset(int row, int h, int radius) {
    const int from_edge = std::min(row, h - 1 - row);
    if (from_edge >= radius) return 0;
    const double dy = radius - from_edge - 0.5;
    return radius - int(std::sqrt(std::max(0.0, double(radius) * radius - dy * dy)));
}

}  // namespace

bool Init() {
    if (R_SUCCEEDED(framebufferCreate(&g_framebuffer, nwindowGetDefault(), kWidth, kHeight, PIXEL_FORMAT_RGBA_8888, 2)) &&
        R_SUCCEEDED(framebufferMakeLinear(&g_framebuffer))) {
        g_framebuffer_ready = true;
    }
    PlFontData font;
    if (R_SUCCEEDED(plInitialize(PlServiceType_User)) &&
        R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_Standard)) &&
        stbtt_InitFont(&g_font, static_cast<const unsigned char*>(font.address), 0)) {
        g_font_ready = true;
    }
    return g_framebuffer_ready && g_font_ready;
}

void Exit() {
    if (g_framebuffer_ready) framebufferClose(&g_framebuffer);
    g_framebuffer_ready = false;
    plExit();
    g_glyphs.clear();
}

void Begin() {
    if (!g_framebuffer_ready) return;
    u32 stride = 0;
    g_pixels = static_cast<uint32_t*>(framebufferBegin(&g_framebuffer, &stride));
    g_stride = int(stride / 4);
    if (!g_pixels) return;
    // The background: charcoal, a shade darker towards the bottom.
    for (int y = 0; y < kHeight; ++y) {
        const int t = y * 255 / kHeight;
        const uint8_t grey = uint8_t(36 - t * 8 / 255);
        Span(0, kWidth, y, Rgb(grey, uint8_t(grey + 1), uint8_t(grey + 3)));
    }
}

void End() {
    if (g_framebuffer_ready && g_pixels) framebufferEnd(&g_framebuffer);
    g_pixels = nullptr;
}

void Rect(int x, int y, int w, int h, Color color) {
    for (int row = 0; row < h; ++row) Span(x, x + w, y + row, color);
}

void RoundRect(int x, int y, int w, int h, int radius, Color color) {
    radius = std::min({radius, w / 2, h / 2});
    for (int row = 0; row < h; ++row) {
        const int inset = Inset(row, h, radius);
        Span(x + inset, x + w - inset, y + row, color);
    }
}

void RoundFrame(int x, int y, int w, int h, int radius, int thickness, Color color) {
    radius = std::min({radius, w / 2, h / 2});
    const int inner_radius = std::max(0, radius - thickness);
    for (int row = 0; row < h; ++row) {
        const int outer = Inset(row, h, radius);
        const int inner_row = row - thickness, inner_h = h - 2 * thickness;
        if (inner_row < 0 || inner_row >= inner_h) {
            Span(x + outer, x + w - outer, y + row, color);
            continue;
        }
        const int inner = thickness + Inset(inner_row, inner_h, inner_radius);
        Span(x + outer, x + inner, y + row, color);
        Span(x + w - inner, x + w - outer, y + row, color);
    }
}

void DrawImage(const Image& image, int x, int y, int w, int h) {
    if (!g_pixels || image.width == 0 || image.height == 0) return;
    for (int row = 0; row < h; ++row) {
        const int py = y + row;
        if (py < 0 || py >= kHeight) continue;
        const int sy = row * image.height / h;
        uint32_t* out = g_pixels + py * g_stride;
        const uint32_t* in = image.pixels.data() + size_t(sy) * image.width;
        for (int col = 0; col < w; ++col) {
            const int px = x + col;
            if (px < 0 || px >= kWidth) continue;
            const uint32_t pixel = in[col * image.width / w];
            Blend(out[px], pixel | 0xFF000000u, pixel >> 24);
        }
    }
}

void Text(int x, int y, int size, Color color, const std::string& text) {
    if (!g_pixels || !g_font_ready) return;
    const int baseline = y + Ascent(size);
    int pen = x;
    for (const uint32_t cp : Decode(text)) {
        const Glyph& glyph = GetGlyph(size, cp);
        for (int row = 0; row < glyph.height; ++row) {
            const int py = baseline + glyph.y_offset + row;
            if (py < 0 || py >= kHeight) continue;
            uint32_t* out = g_pixels + py * g_stride;
            for (int col = 0; col < glyph.width; ++col) {
                const int px = pen + glyph.x_offset + col;
                if (px < 0 || px >= kWidth) continue;
                const uint8_t coverage = glyph.coverage[size_t(row) * glyph.width + col];
                if (coverage) Blend(out[px], color, coverage);
            }
        }
        pen += glyph.advance;
    }
}

int TextWidth(int size, const std::string& text) {
    if (!g_font_ready) return 0;
    int width = 0;
    for (const uint32_t cp : Decode(text)) width += GetGlyph(size, cp).advance;
    return width;
}

int WrapText(int x, int y, int width, int size, Color color, const std::string& text) {
    const int line_height = size * 5 / 4;
    int top = y;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string paragraph = text.substr(start, end - start);
        std::string line;
        size_t at = 0;
        while (at <= paragraph.size()) {
            size_t space = paragraph.find(' ', at);
            if (space == std::string::npos) space = paragraph.size();
            const std::string word = paragraph.substr(at, space - at);
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && TextWidth(size, candidate) > width) {
                Text(x, top, size, color, line);
                top += line_height;
                line = word;
            } else {
                line = candidate;
            }
            at = space + 1;
        }
        Text(x, top, size, color, line);
        top += line_height;
        start = end + 1;
    }
    return top - y;
}

int ButtonHint(int x, int y, const char* button, const std::string& label) {
    constexpr int kSize = 22, kDiameter = 30;
    const Color circle = Rgb(226, 227, 230);
    RoundRect(x, y, kDiameter, kDiameter, kDiameter / 2, circle);
    const int letter = TextWidth(kSize, button);
    Text(x + (kDiameter - letter) / 2, y + 3, kSize, Rgb(32, 33, 36), button);
    Text(x + kDiameter + 10, y + 3, kSize, Rgb(210, 211, 214), label);
    return kDiameter + 10 + TextWidth(kSize, label);
}

bool LoadJpeg(const char* path, Image& image) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    std::vector<unsigned char> data;
    unsigned char buffer[16384];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) data.insert(data.end(), buffer, buffer + n);
    std::fclose(file);
    tjhandle decoder = tjInitDecompress();
    if (!decoder) return false;
    int width, height, subsampling, colorspace;
    bool ok = tjDecompressHeader3(decoder, data.data(), data.size(), &width, &height, &subsampling, &colorspace) == 0;
    if (ok) {
        image.width = width;
        image.height = height;
        image.pixels.resize(size_t(width) * height);
        ok = tjDecompress2(decoder, data.data(), data.size(), reinterpret_cast<unsigned char*>(image.pixels.data()),
                           width, 0, height, TJPF_RGBA, 0) == 0;
    }
    tjDestroy(decoder);
    return ok;
}

}  // namespace ui
