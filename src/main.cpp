// Bulkhead entry point.
//   (no args)            play in a window
//   --capture <dir>      headless: walk start -> exit, write <dir>/frame_0001.png ... and exit
//   --shots <dir>        headless: fixed views (corridor, door, room, junction), RT on and off
//   --overhead <file>    write the maze's top-down map PNG and exit (CPU only)

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <memory>
#include <string>

#include "assets/Kit.h"
#include "core/CommandLine.h"
#include "core/Image.h"
#include "core/Log.h"
#include "core/Window.h"
#include "game/Game.h"
#include "render/Gpu.h"
#include "render/Renderer.h"
#include "world/Maze.h"
#include "world/OverheadMap.h"

namespace fs = std::filesystem;
using namespace bh;

namespace {

constexpr uint64_t kDefaultSeed = 1234;

MazeParams mazeParams(const Options& o, uint64_t seed) {
    MazeParams mp;
    mp.width = o.mazeWidth;
    mp.height = o.mazeHeight;
    mp.seed = seed;
    return mp;
}

OverheadMap overheadFor(const Maze& maze, const Options& o) {
    OverheadStyle style;
    style.cellPx = o.cellPx;
    style.marginPx = o.cellPx / 2;
    return renderOverheadMap(maze, style);
}

Vec2 mapUV(const OverheadMap& map, Vec3 p) {
    float px = map.marginPx + (p.x / 4.0f + 0.5f) * map.cellPx;
    float py = map.marginPx + (p.z / 4.0f + 0.5f) * map.cellPx;
    return {px / float(map.image.width()), py / float(map.image.height())};
}

void loadKit(Kit& kit, const Options& o) {
    fs::path dir = findAssetsDirectory(o.assets);
    std::string error;
    if (!kit.loadDirectory(dir, error)) throw std::runtime_error("Loading the kit failed: " + error);
    log::info("Kit: {} models, {} parts, {} materials from {}", kit.models().size(), kit.parts().size(),
              kit.materials().size(), fs::absolute(dir).string());
}

void describe(const Game& game) {
    const Level& l = game.level();
    log::info("Maze {}", game.maze().describe());
    log::info("Level: {} placed models, {} doors, {} lights, {} props, walk {:.1f} m ({:.1f} s)", l.placed.size(),
              l.doors.size(), l.lights.size(), l.propCount, game.walk().length(), game.walkDuration());
    for (const std::string& m : l.missingModels) log::warn("kit has no model '{}'", m);
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') out += '\\';
        out += ch;
    }
    return out;
}

void clearFrames(const fs::path& dir) {
    fs::create_directories(dir);
    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && entry.path().extension() == ".png" &&
            (name.starts_with("frame_") || name.starts_with("shot_")))
            fs::remove(entry.path());
    }
}

// ---------------------------------------------------------------------------

int runOverhead(const Options& o) {
    const Maze maze = Maze::generate(mazeParams(o, o.seed.value_or(kDefaultSeed)));
    log::info("Maze {}", maze.describe());
    log::info("\n{}", maze.toAscii());
    const OverheadMap map = overheadFor(maze, o);
    if (o.output.has_parent_path()) fs::create_directories(o.output.parent_path());
    if (!map.image.writePng(o.output)) {
        log::error("Could not write {}", o.output.string());
        return 1;
    }
    log::info("Wrote overhead map {} ({}x{})", o.output.string(), map.image.width(), map.image.height());
    return 0;
}

int runCapture(const Options& o) {
    const auto started = std::chrono::steady_clock::now();
    const fs::path dir = o.output;
    clearFrames(dir);
    log::setFile(dir / "capture.log");
    const uint64_t seed = o.seed.value_or(kDefaultSeed);

    Kit kit;
    loadKit(kit, o);
    Game game(kit, mazeParams(o, seed));
    describe(game);
    const OverheadMap map = overheadFor(game.maze(), o);
    map.image.writePng(dir / "overhead.png");

    const float dt = 1.0f / float(o.fps);
    const int frames = o.frames > 0 ? o.frames : int(std::ceil((game.walkDuration() - o.start) * float(o.fps)));
    log::info("Capture: {} frames at {}x{}, {} fps, {} spp, RT {}, seed {} -> {}", frames, o.width, o.height, o.fps,
              o.spp, o.rayTracing ? "on" : "off", seed, dir.string());

    Gpu gpu(o.debugLayer);
    Renderer renderer(gpu, nullptr, uint32_t(o.width), uint32_t(o.height));
    renderer.setMapImage(map.image);
    renderer.setKit(kit);
    renderer.setLevel(game.level());

    // let the doors catch up with a late start
    for (float t = 0.0f; t < o.start; t += dt) {
        game.setWalkTime(t);
        game.updateDoors(dt);
    }
    // PNG encoding is slower than rendering: encode on worker threads while the GPU
    // renders the next frames.
    std::vector<std::future<bool>> pending;
    bool writeFailed = false;
    auto drain = [&](size_t keep) {
        while (pending.size() > keep) {
            writeFailed |= !pending.front().get();
            pending.erase(pending.begin());
        }
    };
    std::vector<SceneInstance> instances;
    float exitFlash = 0.0f;
    for (int i = 0; i < frames; ++i) {
        const float t = o.start + float(i) * dt;
        game.setWalkTime(t);
        game.updateDoors(dt);
        game.buildInstances(instances);
        if (game.reachedExit()) exitFlash = std::min(1.0f, exitFlash + dt / 1.2f);
        FrameSettings s;
        s.camera = game.camera();
        s.rayTracing = o.rayTracing;
        s.hud = o.hud;
        s.exposure = o.exposure;
        s.samples = std::max(2, o.spp);
        s.time = t;
        s.playerMapUV = mapUV(map, game.camera().position);
        s.exitFlash = exitFlash * exitFlash;
        const fs::path file = dir / std::format("frame_{:04}.png", i + 1);
        pending.push_back(std::async(std::launch::async,
                                     [img = renderer.renderToImage(instances, s), file] { return img.writePng(file); }));
        drain(8);
        if (writeFailed) {
            log::error("Could not write frames to {}", dir.string());
            return 1;
        }
        if ((i + 1) % 30 == 0 || i + 1 == frames)
            log::info("  frame {}/{}  t={:.1f}s  {} triangles in scene", i + 1, frames, t,
                      renderer.trianglesInScene());
    }
    drain(0);
    if (writeFailed) {
        log::error("Could not write frames to {}", dir.string());
        return 1;
    }

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::ofstream manifest(dir / "capture.json");
    manifest << std::format(
        "{{\n  \"seed\": {},\n  \"maze\": \"{}x{}\",\n  \"frames\": {},\n  \"fps\": {},\n  \"width\": {},\n"
        "  \"height\": {},\n  \"spp\": {},\n  \"rayTracing\": {},\n  \"gpu\": \"{}\",\n  \"rooms\": {},\n"
        "  \"doors\": {},\n  \"lights\": {},\n  \"walkMetres\": {:.1f},\n  \"seconds\": {:.2f}\n}}\n",
        seed, o.mazeWidth, o.mazeHeight, frames, o.fps, o.width, o.height, o.spp, o.rayTracing ? "true" : "false",
        jsonEscape(gpu.info().adapterName), game.maze().rooms().size(), game.level().doors.size(),
        game.level().lights.size(), game.walk().length(), seconds);
    log::info("Capture done in {:.1f} s", seconds);
    log::setFile({});
    return 0;
}

int runShots(const Options& o) {
    const fs::path dir = o.output;
    clearFrames(dir);
    log::setFile(dir / "shots.log");
    const uint64_t seed = o.seed.value_or(kDefaultSeed);
    Kit kit;
    loadKit(kit, o);
    Game game(kit, mazeParams(o, seed));
    describe(game);
    const OverheadMap map = overheadFor(game.maze(), o);
    map.image.writePng(dir / "overhead.png");

    Gpu gpu(o.debugLayer);
    Renderer renderer(gpu, nullptr, uint32_t(o.width), uint32_t(o.height));
    renderer.setMapImage(map.image);
    renderer.setKit(kit);
    renderer.setLevel(game.level());

    std::vector<SceneInstance> instances;
    for (const Shot& shot : game.findShots()) {
        game.setCamera(shot.camera);
        game.openDoorsNear(shot.camera.position, shot.name == "room" ? 3.0f : 0.0f);
        game.buildInstances(instances);
        for (bool rt : {true, false}) {
            FrameSettings s;
            s.camera = shot.camera;
            s.rayTracing = rt;
            s.hud = o.hud;
            s.minimap = false;
            s.exposure = o.exposure;
            s.samples = std::max(2, o.spp);
            const Image img = renderer.renderToImage(instances, s);
            const fs::path file = dir / std::format("shot_{}_{}.png", shot.name, rt ? "rt" : "nort");
            if (!img.writePng(file)) {
                log::error("Could not write {}", file.string());
                return 1;
            }
            log::info("Wrote {}", file.string());
        }
    }
    log::setFile({});
    return 0;
}

// Plays the scripted walk the way the window renders it (1 sample per pixel, temporal
// accumulation, 60 updates per second), timing every frame and saving a few of them.
int runBench(const Options& o) {
    const fs::path dir = o.output;
    clearFrames(dir);
    log::setFile(dir / "bench.log");
    Kit kit;
    loadKit(kit, o);
    Game game(kit, mazeParams(o, o.seed.value_or(kDefaultSeed)));
    describe(game);
    const OverheadMap map = overheadFor(game.maze(), o);
    Gpu gpu(o.debugLayer);
    Renderer renderer(gpu, nullptr, uint32_t(o.width), uint32_t(o.height));
    renderer.setMapImage(map.image);
    renderer.setKit(kit);
    renderer.setLevel(game.level());

    const float dt = 1.0f / 60.0f;
    const int frames = o.frames > 0 ? o.frames : 600;
    std::vector<SceneInstance> instances;
    std::vector<double> ms;
    for (int i = 0; i < frames; ++i) {
        const float t = o.start + float(i) * dt;
        game.setWalkTime(t);
        game.updateDoors(dt);
        game.buildInstances(instances);
        FrameSettings s;
        s.camera = game.camera();
        s.rayTracing = o.rayTracing;
        s.hud = o.hud;
        s.exposure = o.exposure;
        s.playerMapUV = mapUV(map, game.camera().position);
        s.resetHistory = i == 0;
        const auto a = std::chrono::steady_clock::now();
        if ((i + 1) % 120 == 0) {
            renderer.renderToImage(instances, s).writePng(dir / std::format("frame_{:04}.png", i + 1));
        } else {
            renderer.render(instances, s);
            renderer.gpuIdle();
        }
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
    }
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    double avg = 0;
    for (double v : ms) avg += v;
    avg /= double(ms.size());
    log::info("Bench {}x{} RT {}: {} frames, average {:.2f} ms ({:.0f} fps), median {:.2f} ms, 95th {:.2f} ms, "
              "worst {:.2f} ms (every 120th frame also reads back and saves a PNG)",
              o.width, o.height, o.rayTracing ? "on" : "off", ms.size(), avg, 1000.0 / avg,
              sorted[sorted.size() / 2], sorted[sorted.size() * 95 / 100], sorted.back());
    log::setFile({});
    return 0;
}

int runInteractive(const Options& o) {
    // Started from Explorer: drop the console window that a console-subsystem exe gets.
    DWORD pids[2];
    if (GetConsoleProcessList(pids, 2) == 1) FreeConsole();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    Kit kit;
    loadKit(kit, o);
    uint64_t seed = o.seed.value_or(GetTickCount64() % 100000);
    Window window("Bulkhead", o.width, o.height);
    Gpu gpu(o.debugLayer);
    uint32_t w = 0, h = 0;
    window.clientSize(w, h);
    Renderer renderer(gpu, window.hwnd(), w, h);
    renderer.setKit(kit);

    std::unique_ptr<Game> game;
    OverheadMap map;
    auto newShip = [&] {
        game = std::make_unique<Game>(kit, mazeParams(o, seed));
        describe(*game);
        map = overheadFor(game->maze(), o);
        renderer.setMapImage(map.image);
        renderer.setLevel(game->level());
    };
    newShip();
    window.setMouseCaptured(true);

    bool rt = o.rayTracing, hud = o.hud, showMap = false, nextSeed = false, screenshot = false, reset = true;
    window.onKeyDown = [&](int key) {
        if (key == VK_ESCAPE) {
            if (window.mouseCaptured()) window.setMouseCaptured(false);
            else window.close();
        } else if (key == 'M' || key == VK_TAB) {
            showMap = !showMap;
        } else if (key == 'R') {
            rt = !rt;
            reset = true;
        } else if (key == 'H') {
            hud = !hud;
        } else if (key == 'N') {
            nextSeed = true;
        } else if (key == VK_F12) {
            screenshot = true;
        }
    };

    std::vector<SceneInstance> instances;
    float time = 0.0f, mapOpacity = 0.0f, fpsTimer = 0.0f, exitFlash = 0.0f;
    int fpsFrames = 0, screenshotCount = 0;
    double fps = 0.0;
    auto last = std::chrono::steady_clock::now();
    while (window.pump()) {
        window.clientSize(w, h);
        if (w == 0 || h == 0) {
            Sleep(16);
            continue;
        }
        if (w != renderer.width() || h != renderer.height()) {
            renderer.resize(w, h);
            reset = true;
        }
        if (nextSeed) {
            nextSeed = false;
            ++seed;
            newShip();
            reset = true;
        }
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(0.05f, std::chrono::duration<float>(now - last).count());
        last = now;
        time += dt;

        InputState in;
        in.forward = window.keyDown('W') || window.keyDown(VK_UP);
        in.back = window.keyDown('S') || window.keyDown(VK_DOWN);
        in.left = window.keyDown('A') || window.keyDown(VK_LEFT);
        in.right = window.keyDown('D') || window.keyDown(VK_RIGHT);
        in.run = window.keyDown(VK_SHIFT);
        window.takeMouseDelta(in.mouseDx, in.mouseDy);
        game->update(dt, in);
        game->buildInstances(instances);

        mapOpacity += ((showMap ? 1.0f : 0.0f) - mapOpacity) * std::min(1.0f, dt * 10.0f);
        const bool escaped = game->reachedExit();
        exitFlash += ((escaped ? 1.0f : 0.0f) - exitFlash) * std::min(1.0f, dt * 3.0f);

        FrameSettings s;
        s.camera = game->camera();
        s.rayTracing = rt;
        s.hud = hud;
        s.bigMap = mapOpacity;
        s.exposure = o.exposure;
        s.time = time;
        s.exitFlash = exitFlash;
        s.playerMapUV = mapUV(map, game->camera().position);
        s.resetHistory = reset;
        reset = false;
        renderer.renderAndPresent(instances, s);

        if (screenshot) {
            screenshot = false;
            fs::create_directories("captures");
            FrameSettings still = s;
            still.samples = 16;
            const fs::path file = fs::path("captures") / std::format("screenshot_seed{}_{:03}.png", seed, ++screenshotCount);
            if (renderer.renderToImage(instances, still).writePng(file)) log::info("Saved {}", fs::absolute(file).string());
            reset = true;
        }

        ++fpsFrames;
        fpsTimer += dt;
        if (fpsTimer >= 0.5f) {
            fps = fpsFrames / fpsTimer;
            fpsFrames = 0;
            fpsTimer = 0.0f;
            std::string status = escaped ? "  |  ESCAPED! N = new ship" : "";
            window.setTitle(std::format(
                "Bulkhead  |  seed {}  |  RT {}  |  {:.0f} fps  |  WASD move, mouse look, Shift run, M map, R ray tracing, "
                "H HUD, N new ship, Esc mouse{}",
                seed, rt ? "ON" : "OFF", fps, status));
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
            case Options::Mode::Shots: return runShots(*options);
            case Options::Mode::Bench: return runBench(*options);
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
