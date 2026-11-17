#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace bh {

struct Options {
    enum class Mode { Interactive, Capture, Overhead, Help };
    Mode mode = Mode::Interactive;

    std::filesystem::path output;  // capture folder or overhead PNG
    int frames = 120;
    int fps = 30;
    int width = 1280;              // render resolution (capture) / window client size (interactive)
    int height = 720;
    std::optional<uint64_t> seed;  // interactive picks a random seed when absent
    int mazeWidth = 12;
    int mazeHeight = 12;
    int cellPx = 64;               // overhead map scale
    bool debugLayer = false;
};

// Returns nullopt and sets `error` for bad input.
std::optional<Options> parseCommandLine(int argc, char** argv, std::string& error);
std::string usageText();

}  // namespace bh
