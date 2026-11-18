#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace bh {

struct Options {
    enum class Mode { Interactive, Capture, Shots, Bench, Overhead, Check, Encounters, SoundCheck, Help };
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
    int mazeWidth = 40;            // the ship: 40 x 40 cells of 4 m
    int mazeHeight = 40;
    int cellPx = 64;               // overhead map scale (the --overhead PNG)
    bool rayTracing = true;
    bool hud = true;
    bool flashlight = true;
    int creatures = -1;            // window: creatures loose in the ship (-1: the game's default)
    std::string hunt = "new";      // how creatures find you: "new" (round 11) or "original" (before it)
    int encounterSeeds = 10;       // --encounters: ships x runs on each
    int encounterVariants = 3;
    float minutes = 10.0f;         // --encounters: each run's length
    bool sweep = false;            // --encounters: try a grid of tunings (count, drift, hearing)
    bool overlay = false;          // capture --scene climb: draw the crawl's machinery over it
    bool temporal = false;         // capture: 1 sample a frame with the window's history (its noise)
    int testLights = 0;            // --bench: add this many short-lived lights round the camera (cost test)
    std::string lights = "sampled";  // "sampled" (round 18) or "all" (every light in range, shadow-tested)
    std::optional<bool> lightCaps;   // the old caps on short-lived lights; default: on with "all"
    int lightPicks = 2;              // sampled: lights shadow-tested per pixel (1-4)
    bool lightCapsOn() const { return lightCaps.value_or(lights == "all"); }
    int stepHz = 100;              // weave capture: game steps per second
    std::string scene = "walk";    // capture: "walk" (start -> exit), "fire" (six shots), "crawl" (the creature)
    float haze = 1.0f;             // haze density multiplier
    float exposure = 1.0f;
    bool debugLayer = false;
};

// Returns nullopt and sets `error` for bad input.
std::optional<Options> parseCommandLine(int argc, char** argv, std::string& error);
std::string usageText();

}  // namespace bh
