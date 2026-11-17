#pragma once

// The launcher's drawing: software rendering into libnx's framebuffer (1280x720, RGBA), text in
// the console's own system font (pl, as hbmenu uses), and JPEG icons (turbojpeg). No GPU: the games
// that start after the launcher use Vulkan, and a second GPU user in one process is too close to
// what crashes a second game launch.
#include <cstdint>
#include <string>
#include <vector>

namespace ui {

constexpr int kWidth = 1280, kHeight = 720;

using Color = uint32_t;  // as it lies in memory: R, G, B, A
constexpr Color Rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24;
}

struct Image {
    int width = 0, height = 0;
    std::vector<uint32_t> pixels;  // RGBA, rows top to bottom
};

bool Init();  // the framebuffer and the font; false when either is missing
void Exit();

void Begin();  // a new frame, cleared to the background
void End();    // shows it

void Rect(int x, int y, int w, int h, Color color);
void RoundRect(int x, int y, int w, int h, int radius, Color color);
void RoundFrame(int x, int y, int w, int h, int radius, int thickness, Color color);
void DrawImage(const Image& image, int x, int y, int w, int h);  // scaled to w x h

// Text: `y` is the top of the line, `size` its pixel height.
void Text(int x, int y, int size, Color color, const std::string& text);
// The same, a pixel heavier: for titles (the system font is light, and a thin stroke such as the
// "i" spread over two pixel columns looks faint at large sizes).
void BoldText(int x, int y, int size, Color color, const std::string& text);
int TextWidth(int size, const std::string& text);
// Text wrapped to `width` (and at '\n'); returns the height used.
int WrapText(int x, int y, int width, int size, Color color, const std::string& text);
// A button hint: the letter in a circle, then the label; returns its width.
int ButtonHint(int x, int y, const char* button, const std::string& label);

bool LoadJpeg(const char* path, Image& image);

}  // namespace ui
