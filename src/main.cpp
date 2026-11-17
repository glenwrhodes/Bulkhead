// Bulkhead entry point.
//   (no args)            interactive window
//   --capture <dir>      headless: render N frames to <dir>/frame_0001.png ... and exit
//   --overhead <file>    write the maze's top-down map PNG and exit (CPU only)
// See usageText() in core/CommandLine.cpp.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>

#include "core/CommandLine.h"
#include "core/Image.h"
#include "core/Log.h"
#include "core/Window.h"
#include "render/Gpu.h"
#include "render/Renderer.h"
#include "world/Maze.h"
#include "world/OverheadMap.h"

namespace fs = std::filesystem;
using namespace bh;

namespace {

constexpr uint64_t kDefaultSeed = 1234;

float smoothstep(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The generated level for this round: the maze plus its overhead map.
struct Level {
    Maze maze;
    OverheadMap map;
};

Level buildLevel(const Options& o, uint64_t seed) {
    MazeParams mp;
    mp.width = o.mazeWidth;
    mp.height = o.mazeHeight;
    mp.seed = seed;
    Level level{Maze::generate(mp), {}};
    OverheadStyle style;
    style.cellPx = o.cellPx;
    style.marginPx = o.cellPx / 2;
    level.map = renderOverheadMap(level.maze, style);
    log::info("Maze {}", level.maze.describe());
    return level;
}

// Places the route marker `distance` pixels along the route.
void placeMarker(const OverheadMap& map, float distance, FrameParams& p) {
    const Float2 pos = map.routeTrack.at(distance);
    p.markerU = pos.x / float(map.image.width());
    p.markerV = pos.y / float(map.image.height());
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') out += '\\';
        out += ch;
    }
    return out;
}

// ---------------------------------------------------------------------------

int runOverhead(const Options& o) {
    const uint64_t seed = o.seed.value_or(kDefaultSeed);
    const Level level = buildLevel(o, seed);
    log::info("\n{}", level.maze.toAscii());
    if (o.output.has_parent_path()) fs::create_directories(o.output.parent_path());
    if (!level.map.image.writePng(o.output)) {
        log::error("Could not write {}", o.output.string());
        return 1;
    }
    log::info("Wrote overhead map {} ({}x{})", o.output.string(), level.map.image.width(), level.map.image.height());
    return 0;
}

int runCapture(const Options& o) {
    const auto started = std::chrono::steady_clock::now();
    const fs::path dir = o.output;
    fs::create_directories(dir);
    // Remove frames from an earlier run so the folder only holds this capture.
    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && name.starts_with("frame_") && entry.path().extension() == ".png")
            fs::remove(entry.path());
    }
    log::setFile(dir / "capture.log");

    const uint64_t seed = o.seed.value_or(kDefaultSeed);
    log::info("Capture: {} frames at {}x{}, {} fps, seed {}, maze {}x{} -> {}", o.frames, o.width, o.height, o.fps,
              seed, o.mazeWidth, o.mazeHeight, dir.string());
    const Level level = buildLevel(o, seed);
    level.map.image.writePng(dir / "overhead.png");

    Gpu gpu(o.debugLayer);
    Renderer renderer(gpu, nullptr, uint32_t(o.width), uint32_t(o.height));
    renderer.setMapImage(level.map.image);

    // Scripted timeline: map fades in, marker appears, walks start -> exit, holds.
    const float duration = float(o.frames) / float(o.fps);
    const float travelStart = std::min(0.8f, duration * 0.12f);
    const float travelEnd = std::max(travelStart + 0.1f, duration * 0.9f);
    const float routeLength = level.map.routeTrack.length();

    for (int i = 0; i < o.frames; ++i) {
        const float t = float(i) / float(o.fps);
        FrameParams p;
        p.time = t;
        p.mapOpacity = smoothstep(0.0f, 0.6f, t);
        p.markerVisible = smoothstep(travelStart - 0.3f, travelStart, t);
        placeMarker(level.map, routeLength * smoothstep(travelStart, travelEnd, t), p);

        const Image frame = renderer.renderToImage(p);
        const fs::path file = dir / std::format("frame_{:04}.png", i + 1);
        if (!frame.writePng(file)) {
            log::error("Could not write {}", file.string());
            return 1;
        }
        if ((i + 1) % 30 == 0 || i + 1 == o.frames) log::info("  frame {}/{}", i + 1, o.frames);
    }

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::ofstream manifest(dir / "capture.json");
    manifest << std::format(
        "{{\n  \"seed\": {},\n  \"maze\": \"{}x{}\",\n  \"frames\": {},\n  \"fps\": {},\n  \"width\": {},\n"
        "  \"height\": {},\n  \"gpu\": \"{}\",\n  \"raytracingTier\": \"{}\",\n  \"rooms\": {},\n"
        "  \"routeCells\": {},\n  \"seconds\": {:.2f}\n}}\n",
        seed, o.mazeWidth, o.mazeHeight, o.frames, o.fps, o.width, o.height, jsonEscape(gpu.info().adapterName),
        gpu.info().raytracingTierName(), level.maze.rooms().size(), level.map.route.size(), seconds);
    log::info("Capture done in {:.1f} s", seconds);
    log::setFile({});
    return 0;
}

int runInteractive(const Options& o) {
    // Started from Explorer: drop the console window that a console-subsystem exe gets.
    DWORD pids[2];
    if (GetConsoleProcessList(pids, 2) == 1) FreeConsole();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    uint64_t seed = o.seed.value_or(GetTickCount64() % 100000);
    Window window("Bulkhead", o.width, o.height);
    Gpu gpu(o.debugLayer);
    uint32_t w = 0, h = 0;
    window.clientSize(w, h);
    Renderer renderer(gpu, window.hwnd(), w, h);

    std::unique_ptr<Level> level;
    auto regenerate = [&] {
        level = std::make_unique<Level>(buildLevel(o, seed));
        renderer.setMapImage(level->map.image);
        window.setTitle(std::format("Bulkhead  |  seed {}  |  {}x{} ship, {} rooms  |  {}  |  RT tier {}  |  "
                                    "M map  N next seed  F12 screenshot",
                                    seed, o.mazeWidth, o.mazeHeight, level->maze.rooms().size(),
                                    gpu.info().adapterName, gpu.info().raytracingTierName()));
    };
    regenerate();

    bool showMap = true, nextSeed = false, screenshot = false;
    window.onKeyDown = [&](int key) {
        if (key == VK_ESCAPE) window.close();
        else if (key == 'M') showMap = !showMap;
        else if (key == 'N') nextSeed = true;
        else if (key == VK_F12) screenshot = true;
    };

    float time = 0.0f, mapOpacity = 0.0f, routeClock = 0.0f;
    int screenshotCount = 0;
    auto last = std::chrono::steady_clock::now();
    while (window.pump()) {
        window.clientSize(w, h);
        if (w == 0 || h == 0) {  // minimised
            Sleep(16);
            continue;
        }
        renderer.resize(w, h);
        if (nextSeed) {
            nextSeed = false;
            ++seed;
            regenerate();
            routeClock = 0.0f;
        }

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
        last = now;
        time += dt;
        routeClock += dt;
        mapOpacity += ((showMap ? 1.0f : 0.0f) - mapOpacity) * std::min(1.0f, dt * 8.0f);

        // Marker walks the route at 3 cells per second, waits at the exit, repeats.
        const float speed = 3.0f * float(level->map.cellPx);
        const float travel = level->map.routeTrack.length() / speed;
        if (routeClock > travel + 1.5f) routeClock = 0.0f;

        FrameParams p;
        p.time = time;
        p.mapOpacity = mapOpacity;
        p.markerVisible = 1.0f;
        placeMarker(level->map, routeClock * speed, p);
        renderer.renderAndPresent(p);

        if (screenshot) {
            screenshot = false;
            fs::create_directories("captures");
            const fs::path file = fs::path("captures") / std::format("screenshot_seed{}_{:03}.png", seed, ++screenshotCount);
            if (renderer.renderToImage(p).writePng(file)) log::info("Saved {}", fs::absolute(file).string());
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string error;
    const auto options = parseCommandLine(argc, argv, error);
    if (!options) {
        std::fprintf(stderr, "%s\n\n%s", error.c_str(), usageText().c_str());
        return 2;
    }
    try {
        switch (options->mode) {
            case Options::Mode::Help: std::fputs(usageText().c_str(), stdout); return 0;
            case Options::Mode::Overhead: return runOverhead(*options);
            case Options::Mode::Capture: return runCapture(*options);
            case Options::Mode::Interactive: return runInteractive(*options);
        }
    } catch (const std::exception& e) {
        log::error("{}", e.what());
        if (options->mode == Options::Mode::Interactive)
            MessageBoxW(nullptr, widen(e.what()).c_str(), L"Bulkhead error", MB_OK | MB_ICONERROR);
        return 1;
    }
    return 0;
}
