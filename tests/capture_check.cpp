// Verifies PNGs written by a capture run:
//   capture_check --file F.png --width W --height H
//   capture_check --dir D --count N --width W --height H [--expect-motion]
// Each image must load, have the expected size, and not be a flat colour (a black or
// cleared frame means rendering failed). --expect-motion requires the first and last
// frames to differ, proving time advances through the capture.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "core/Image.h"

namespace fs = std::filesystem;
using bh::Image;

namespace {

double lumaStdDev(const Image& img) {
    double sum = 0, sumSq = 0;
    const size_t n = size_t(img.width()) * img.height();
    const uint8_t* p = img.data();
    for (size_t i = 0; i < n; ++i, p += 4) {
        double l = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
        sum += l;
        sumSq += l * l;
    }
    const double mean = sum / double(n);
    return std::sqrt(std::max(0.0, sumSq / double(n) - mean * mean));
}

double meanAbsDiff(const Image& a, const Image& b) {
    double total = 0;
    const size_t n = size_t(a.width()) * a.height() * 4;
    for (size_t i = 0; i < n; ++i) total += std::abs(int(a.data()[i]) - int(b.data()[i]));
    return total / double(n);
}

bool checkImage(const fs::path& path, int w, int h, Image* out) {
    auto img = Image::loadPng(path);
    if (!img) {
        std::printf("FAIL: cannot load %s\n", path.string().c_str());
        return false;
    }
    if (img->width() != w || img->height() != h) {
        std::printf("FAIL: %s is %dx%d, expected %dx%d\n", path.string().c_str(), img->width(), img->height(), w, h);
        return false;
    }
    const double sd = lumaStdDev(*img);
    if (sd < 4.0) {
        std::printf("FAIL: %s looks flat (luma std dev %.2f)\n", path.string().c_str(), sd);
        return false;
    }
    if (out) *out = std::move(*img);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    fs::path file, dir;
    int count = 0, width = 0, height = 0, atLeast = 0;
    bool expectMotion = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--file") file = next();
        else if (a == "--dir") dir = next();
        else if (a == "--count") count = std::atoi(next().c_str());
        else if (a == "--at-least") atLeast = std::atoi(next().c_str());
        else if (a == "--width") width = std::atoi(next().c_str());
        else if (a == "--height") height = std::atoi(next().c_str());
        else if (a == "--expect-motion") expectMotion = true;
        else {
            std::printf("unknown argument %s\n", a.c_str());
            return 2;
        }
    }

    if (!file.empty()) {
        if (!checkImage(file, width, height, nullptr)) return 1;
        std::printf("OK: %s\n", file.string().c_str());
        return 0;
    }

    if (!dir.empty() && atLeast > 0) {
        // count whatever frames the run produced (e.g. the whole walk)
        count = 0;
        while (fs::exists(dir / std::format("frame_{:04}.png", count + 1))) ++count;
        if (count < atLeast) {
            std::printf("FAIL: only %d frames in %s, expected at least %d\n", count, dir.string().c_str(), atLeast);
            return 1;
        }
    }
    if (dir.empty() || count <= 0) {
        std::printf("usage: capture_check --file F --width W --height H | --dir D --count N --width W --height H\n");
        return 2;
    }
    Image first, last;
    for (int i = 1; i <= count; ++i) {
        fs::path p = dir / std::format("frame_{:04}.png", i);
        Image img;
        if (!checkImage(p, width, height, &img)) return 1;
        if (i == 1) first = std::move(img);
        else if (i == count) last = std::move(img);
    }
    if (fs::exists(dir / std::format("frame_{:04}.png", count + 1))) {
        std::printf("FAIL: more than %d frames in %s\n", count, dir.string().c_str());
        return 1;
    }
    if (expectMotion && count > 1) {
        const double diff = meanAbsDiff(first, last);
        if (diff < 1.0) {
            std::printf("FAIL: first and last frames are nearly identical (mean diff %.3f)\n", diff);
            return 1;
        }
        std::printf("first->last mean abs diff %.2f\n", diff);
    }
    std::printf("OK: %d frames %dx%d in %s\n", count, width, height, dir.string().c_str());
    return 0;
}
