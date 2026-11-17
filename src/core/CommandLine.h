#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace bh {

struct Options {
    enum class Mode { Interactive, Capture, Shots, Bench, Overhead, Help };
    Mode mode = Mode::Interactive;

    std::filesystem::path output;  // capture/shots folder or overhead PNG
    std::filesystem::path assets;  // empty: find automatically
    int frames = 0;                // capture: 0 = the whole walk from start to exit
    int fps = 30;
    float start = 0.0f;            // capture: seconds into the walk to begin at
    int spp = 8;                   // capture: samples per pixel per frame
    int width = 1280;              // render resolution (capture) / window client size (interactive)
    int height = 720;
    std::optional<uint64_t> seed;  // interactive picks a random seed when absent
    int mazeWidth = 12;
    int mazeHeight = 12;
    int cellPx = 64;               // overhead map scale
    bool rayTracing = true;
    bool hud = true;
    float exposure = 1.0f;
    bool debugLayer = false;
};

// Returns nullopt and sets `error` for bad input.
std::optional<Options> parseCommandLine(int argc, char** argv, std::string& error);
std::string usageText();

}  // namespace bh
