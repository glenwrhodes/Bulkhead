#include "core/Image.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

#include <stb_image.h>
#include <stb_image_write.h>

namespace bh {

Color shade(Color c, float f) {
    auto s = [f](uint8_t v) { return uint8_t(std::clamp(v * f + 0.5f, 0.0f, 255.0f)); };
    return {s(c.r), s(c.g), s(c.b), c.a};
}

Image::Image(int width, int height, Color fill)
    : width_(width), height_(height), pixels_(size_t(width) * height * 4) {
    for (size_t i = 0; i < pixels_.size(); i += 4) {
        pixels_[i + 0] = fill.r;
        pixels_[i + 1] = fill.g;
        pixels_[i + 2] = fill.b;
        pixels_[i + 3] = fill.a;
    }
}

Color Image::get(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return {};
    const uint8_t* p = &pixels_[(size_t(y) * width_ + x) * 4];
    return {p[0], p[1], p[2], p[3]};
}

void Image::set(int x, int y, Color c) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
    uint8_t* p = &pixels_[(size_t(y) * width_ + x) * 4];
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = c.a;
}

void Image::blend(int x, int y, Color c, float coverage) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
    float a = std::clamp(coverage, 0.0f, 1.0f) * (c.a / 255.0f);
    if (a <= 0.0f) return;
    uint8_t* p = &pixels_[(size_t(y) * width_ + x) * 4];
    auto mix = [a](uint8_t dst, uint8_t src) { return uint8_t(dst + (src - dst) * a + 0.5f); };
    p[0] = mix(p[0], c.r);
    p[1] = mix(p[1], c.g);
    p[2] = mix(p[2], c.b);
    p[3] = uint8_t(std::min(255.0f, p[3] + (255 - p[3]) * a + 0.5f));
}

void Image::fillRect(int x0, int y0, int x1, int y1, Color c) {
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, width_);
    y1 = std::min(y1, height_);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            if (c.a == 255) set(x, y, c);
            else blend(x, y, c, 1.0f);
        }
}

void Image::fillCircle(float cx, float cy, float r, Color c) {
    int x0 = int(std::floor(cx - r - 1)), x1 = int(std::ceil(cx + r + 1));
    int y0 = int(std::floor(cy - r - 1)), y1 = int(std::ceil(cy + r + 1));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
            blend(x, y, c, r - d + 0.5f);
        }
}

void Image::strokeCircle(float cx, float cy, float r, float thickness, Color c) {
    float outer = r + thickness;
    int x0 = int(std::floor(cx - outer - 1)), x1 = int(std::ceil(cx + outer + 1));
    int y0 = int(std::floor(cy - outer - 1)), y1 = int(std::ceil(cy + outer + 1));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
            blend(x, y, c, thickness * 0.5f - std::abs(d - r) + 0.5f);
        }
}

namespace {
void writeToStream(void* context, void* data, int size) {
    static_cast<std::ofstream*>(context)->write(static_cast<const char*>(data), size);
}
}  // namespace

bool Image::writePng(const std::filesystem::path& path, bool keepAlpha) const {
    if (empty()) return false;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    int ok = 0;
    if (keepAlpha) {
        ok = stbi_write_png_to_func(writeToStream, &file, width_, height_, 4, pixels_.data(), width_ * 4);
    } else {
        std::vector<uint8_t> rgbData(size_t(width_) * height_ * 3);
        for (size_t i = 0, j = 0; i < pixels_.size(); i += 4, j += 3) {
            rgbData[j + 0] = pixels_[i + 0];
            rgbData[j + 1] = pixels_[i + 1];
            rgbData[j + 2] = pixels_[i + 2];
        }
        ok = stbi_write_png_to_func(writeToStream, &file, width_, height_, 3, rgbData.data(), width_ * 3);
    }
    return ok != 0 && file.good();
}

std::optional<Image> Image::loadPng(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return std::nullopt;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
    if (!px) return std::nullopt;
    Image img(w, h);
    std::copy(px, px + size_t(w) * h * 4, img.data());
    stbi_image_free(px);
    return img;
}

}  // namespace bh
