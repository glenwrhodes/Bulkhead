#pragma once
// RGBA8 image with a few drawing helpers (used for the overhead map) and PNG I/O.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace bh {

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    bool operator==(const Color&) const = default;
};

// 0xRRGGBB -> Color
constexpr Color rgb(uint32_t hex, uint8_t alpha = 255) {
    return {uint8_t(hex >> 16), uint8_t(hex >> 8), uint8_t(hex), alpha};
}

// Scales the colour channels (keeps alpha).
Color shade(Color c, float factor);

class Image {
public:
    Image() = default;
    Image(int width, int height, Color fill = {0, 0, 0, 255});

    int width() const { return width_; }
    int height() const { return height_; }
    bool empty() const { return width_ == 0 || height_ == 0; }
    uint8_t* data() { return pixels_.data(); }
    const uint8_t* data() const { return pixels_.data(); }
    size_t rowPitch() const { return size_t(width_) * 4; }

    Color get(int x, int y) const;
    void set(int x, int y, Color c);                     // ignores out-of-range
    void blend(int x, int y, Color c, float coverage);   // "over" blend, alpha * coverage

    // Rectangle [x0, x1) x [y0, y1), clipped. Blended when c.a < 255.
    void fillRect(int x0, int y0, int x1, int y1, Color c);
    // Anti-aliased disc and ring.
    void fillCircle(float cx, float cy, float radius, Color c);
    void strokeCircle(float cx, float cy, float radius, float thickness, Color c);

    // PNG. RGB unless keepAlpha. Returns false on failure.
    bool writePng(const std::filesystem::path& path, bool keepAlpha = false) const;
    static std::optional<Image> loadPng(const std::filesystem::path& path);

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<uint8_t> pixels_;
};

}  // namespace bh
