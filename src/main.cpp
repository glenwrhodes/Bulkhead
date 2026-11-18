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
#include <mutex>
#include <string>

#include "assets/Kit.h"
#include "core/CommandLine.h"
#include "core/Image.h"
#include "core/Log.h"
#include "core/Window.h"
#include "audio/AudioDevice.h"
#include "audio/Mixer.h"
#include "game/AudioRecorder.h"
#include "game/Encounters.h"
#include "game/Game.h"
#include "render/Gpu.h"
#include "render/Renderer.h"
#include "world/Maze.h"
#include "world/OverheadMap.h"

namespace fs = std::filesystem;
using namespace bh;

namespace {

constexpr uint64_t kDefaultSeed = 1234;
constexpr int kGameMapCellPx = 32;   // the in-game map texture (minimap and M map)
constexpr float kShotTime = 4.0f;    // light flicker clock for the still shots

MazeParams mazeParams(const Options& o, uint64_t seed) {
    MazeParams mp;
    mp.width = o.mazeWidth;
    mp.height = o.mazeHeight;
    mp.seed = seed;
    return mp;
}

OverheadMap overheadFor(const Maze& maze, const Sections& sections, int cellPx) {
    OverheadStyle style;
    style.cellPx = cellPx;
    style.marginPx = cellPx / 2;
    style.sections = &sections;
    return renderOverheadMap(maze, style);
}

OverheadMap gameMap(const Game& game) { return overheadFor(game.maze(), game.level().sections, kGameMapCellPx); }

// How the creatures find the player: --hunt picks the set, --creatures overrides the count.
HuntTuning huntTuning(const Options& o) {
    HuntTuning t = o.hunt == "original" ? HuntTuning::original() : HuntTuning::round11();
    if (o.creatures >= 0) t.creatures = o.creatures;
    return t;
}

void uploadMap(Renderer& renderer, const OverheadMap& map) {
    renderer.setMapImage(map.image, map.cellPx, map.marginPx);
}

// Per-frame state the renderer takes from the game: light flicker, the flashlight, the
// cells seen so far (for the map).
void applyGame(FrameSettings& s, const Game& game, const Options& o) {
    s.lightRadiance = &game.lightRadiance();
    const Flashlight f = game.flashlight();
    s.flashlight = f.on;
    s.flashPos = f.position;
    s.flashDir = f.direction;
    s.flashIntensity = f.intensity;
    s.flashCosInner = f.cosInner;
    s.flashCosOuter = f.cosOuter;
    s.explored = &game.explored();
    s.haze = o.haze;
    s.dynamicLights = &game.dynamicLights();
    s.steam = &game.steamPlumes();
}

// The scripted shooting scene (--scene fire): standing in a corridor, six shots at the
// walls and floor ahead, the aim easing from one target to the next.
struct FireScene {
    Camera base;
    static constexpr float kShots[6] = {1.0f, 1.75f, 2.35f, 2.75f, 3.6f, 4.4f};
    static constexpr float kYaw[6] = {0.0f, 0.22f, -0.2f, -0.26f, 0.3f, 0.06f};
    static constexpr float kPitch[6] = {-0.03f, -0.05f, 0.05f, -0.12f, 0.08f, -0.16f};
    static constexpr float kDuration = 6.0f;

    Camera at(float t) const {
        // aim: eases toward the next target in the 0.3 s before each shot, plus recoil
        int next = 0;
        while (next < 6 && t > kShots[next]) ++next;
        float yaw = 0.0f, pitch = 0.0f;
        const int prev = next - 1;
        const float py = prev >= 0 ? kYaw[prev] : 0.0f, pp = prev >= 0 ? kPitch[prev] : 0.0f;
        if (next < 6) {
            float w = std::clamp((t - (kShots[next] - 0.3f)) / 0.3f, 0.0f, 1.0f);
            w = w * w * (3.0f - 2.0f * w);
            yaw = py + (kYaw[next] - py) * w;
            pitch = pp + (kPitch[next] - pp) * w;
        } else {
            yaw = py;
            pitch = pp;
        }
        if (prev >= 0) pitch += 0.025f * std::exp(-(t - kShots[prev]) / 0.12f);
        Camera c = base;
        c.yaw += yaw;
        c.pitch += pitch;
        return c;
    }
};

Camera sceneCamera(const Game& game, const char* preferred) {
    const auto shots = game.findShots();
    for (const char* name : {preferred, "corridor_derelict", "corridor"})
        for (const Shot& s : shots)
            if (s.name == name) return s.camera;
    return game.camera();
}

Vec2 mapUV(const OverheadMap& map, Vec3 p) {
    float px = map.marginPx + (p.x / 4.0f + 0.5f) * map.cellPx;
    float py = map.marginPx + (p.z / 4.0f + 0.5f) * map.cellPx;
    return {px / float(map.image.width()), py / float(map.image.height())};
}

// The sound effects in <assets>/sounds. Returns how many loaded (the rest stay silent).
int loadSounds(SoundBank& bank, const Options& o, bool verbose) {
    std::string report;
    const int n = bank.load(findAssetsDirectory(o.assets) / "sounds", report);
    if (verbose) log::info("Sounds: {} of {} loaded\n{}", n, kSoundCount, report);
    return n;
}

void loadKit(Kit& kit, const Options& o) {
    fs::path dir = findAssetsDirectory(o.assets);
    std::string error;
    if (!kit.loadDirectory(dir, error)) throw std::runtime_error("Loading the kit failed: " + error);
    log::info("Kit: {} models, {} parts, {} materials from {}", kit.models().size(), kit.parts().size(),
              kit.materials().size(), fs::absolute(dir).string());
}

void describeSections(const Maze& maze, const Sections& sec) {
    int sections[kMoodCount]{}, cells[kMoodCount]{};
    for (Mood m : sec.mood) ++sections[int(m)];
    for (int y = 0; y < maze.height(); ++y)
        for (int x = 0; x < maze.width(); ++x)
            if (maze.kind(x, y) != CellKind::Void) ++cells[int(sec.moodAt(x, y))];
    std::string parts;
    for (int i = 0; i < kMoodCount; ++i)
        parts += std::format("{}{} {} ({} cells)", i ? ", " : "", sections[i], toString(Mood(i)), cells[i]);
    log::info("Sections: {}: {}", sec.count(), parts);
}

void describe(const Game& game) {
    const Level& l = game.level();
    log::info("Maze {}", game.maze().describe());
    log::info("Level: {} placed models, {} doors, {} lights, {} props, walk {:.1f} m ({:.1f} s)", l.placed.size(),
              l.doors.size(), l.lights.size(), l.propCount, game.walk().length(), game.walkDuration());
    describeSections(game.maze(), l.sections);
    // which moods the scripted walk passes through, and when
    std::string timeline;
    const WalkPath& w = game.walk();
    int current = -1;
    for (float s = 0.0f; s <= w.length(); s += 0.5f) {
        Vec2 p = w.at(s);
        Mood m = l.sections.moodAt(int(std::floor(p.x / kCell + 0.5f)), int(std::floor(p.y / kCell + 0.5f)));
        if (int(m) != current) {
            current = int(m);
            timeline += std::format("{}{:.1f}s {}", timeline.empty() ? "" : ", ", Game::kStartPause + s / Game::kWalkSpeed,
                                    toString(m));
        }
    }
    log::info("Walk moods: {}", timeline);
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
    const Sections sections = buildSections(maze);
    describeSections(maze, sections);
    const OverheadMap map = overheadFor(maze, sections, o.cellPx);
    if (o.output.has_parent_path()) fs::create_directories(o.output.parent_path());
    if (!map.image.writePng(o.output)) {
        log::error("Could not write {}", o.output.string());
        return 1;
    }
    log::info("Wrote overhead map {} ({}x{})", o.output.string(), map.image.width(), map.image.height());
    return 0;
}

// CPU only: finds and loads the models, builds the ship, and says where the models came
// from. The package check runs this on machines without a ray-tracing GPU.
int runCheck(const Options& o) {
    Kit kit;
    loadKit(kit, o);
    Game game(kit, mazeParams(o, o.seed.value_or(kDefaultSeed)));
    describe(game);
    if (!game.level().missingModels.empty()) {
        log::error("The ship needs models the kit does not have");
        return 1;
    }
    SoundBank bank;
    const int sounds = loadSounds(bank, o, false);
    if (sounds < kSoundCount) log::error("Only {} of {} sounds loaded", sounds, kSoundCount);
    log::info("Sounds: {} of {} loaded", sounds, kSoundCount);
    log::info("Check OK: assets {}", fs::absolute(findAssetsDirectory(o.assets)).string());
    return sounds == kSoundCount ? 0 : 1;
}

// --sound-check: the window's sound path (XAudio2) without the window: open the device,
// play the scanner ping and a footstep, close. Exit 0 if it played, 2 if there is no device.
int runSoundCheck(const Options& o) {
    SoundBank bank;
    const int n = loadSounds(bank, o, false);
    Mixer mixer(bank);
    std::mutex lock;
    AudioDevice dev;
    std::string err;
    if (!dev.open(mixer, lock, err)) {
        log::warn("Sound check: {} ({} sounds loaded)", err, n);
        return 2;
    }
    {
        std::lock_guard<std::mutex> g(lock);
        SoundEvent e;
        e.sound = Snd::ScannerPing;
        e.positional = false;
        mixer.play(e);
    }
    Sleep(500);
    {
        std::lock_guard<std::mutex> g(lock);
        SoundEvent e;
        e.sound = Snd::Footstep1;
        e.positional = false;
        mixer.play(e);
    }
    Sleep(700);
    uint64_t frames = 0;
    {
        std::lock_guard<std::mutex> g(lock);
        frames = mixer.framesRendered();
    }
    dev.close();
    log::info("Sound check: XAudio2 played {} frames ({:.2f} s) of {} loaded sounds, peak {:.2f}", frames,
              double(frames) / Mixer::kRate, n, mixer.peak());
    return frames > 0 ? 0 : 1;
}

// --scene weave: the window's own movement code (Game::update) with the forward key held
// and no mouse, down the longest straight corridor, stepped at the window's frame rate
// (--step-hz, default 100). Every step's sideways distance from the corridor's centre
// line goes into weave.csv and the log; frames are written at --fps; weave_plot.png is
// the path seen from above, with the sideways axis stretched so centimetres show.
int runWeave(const Options& o, const fs::path& dir) {
    const uint64_t seed = o.seed.value_or(kDefaultSeed);
    Kit kit;
    loadKit(kit, o);
    Game game(kit, mazeParams(o, seed));
    game.setFlashlight(o.flashlight);
    describe(game);
    const OverheadMap map = gameMap(game);
    Camera start = sceneCamera(game, "corridor");
    start.pitch = -0.03f;
    game.setCamera(start);
    // the corridor: from the start cell straight on along the view
    const Vec3 fwd = normalize(Vec3{start.forward().x, 0, start.forward().z});
    const Vec3 right{-fwd.z, 0, fwd.x};
    const Maze& m = game.maze();
    Int2 c{int(std::floor(start.position.x / kCell + 0.5f)), int(std::floor(start.position.z / kCell + 0.5f))};
    int dirIdx = 0;
    for (int d = 0; d < 4; ++d)
        if (dot(dirVector(Dir(d)), fwd) > 0.9f) dirIdx = d;
    int cells = 1;
    while (m.passable(c.x, c.y, Dir(dirIdx)) && m.kind(c.x + kDx[dirIdx], c.y + kDy[dirIdx]) == CellKind::Corridor) {
        c = {c.x + kDx[dirIdx], c.y + kDy[dirIdx]};
        ++cells;
    }
    const float runLength = float(cells) * kCell - 0.5f;
    const float walkTime = std::min(8.0f, std::max(2.0f, (runLength - 2.5f) / 3.4f));
    const float stepDt = 1.0f / float(o.stepHz);
    const int frames = o.frames > 0 ? o.frames : int(std::ceil(walkTime * float(o.fps)));
    log::info("Scene 'weave': straight corridor of {} cells ({:.1f} m) from ({:.1f}, {:.1f}); forward held, no mouse; "
              "{} Hz steps, {} frames at {} fps",
              cells, runLength, start.position.x, start.position.z, o.stepHz, frames, o.fps);

    Gpu gpu(o.debugLayer);
    Renderer renderer(gpu, nullptr, uint32_t(o.width), uint32_t(o.height));
    uploadMap(renderer, map);
    renderer.setKit(kit);
    renderer.setLevel(game.level());

    std::ofstream csv(dir / "weave.csv");
    csv << "step,time,along_m,sideways_m,eye_height_m\n";
    struct Sample {
        float along, side;
    };
    std::vector<Sample> path;
    InputState in;
    in.forward = true;
    float simTime = 0.0f;
    int step = 0;
    float maxSide = 0.0f, minSide = 0.0f;
    std::vector<SceneInstance> instances;
    std::vector<std::future<bool>> pending;
    bool writeFailed = false;
    for (int i = 0; i < frames; ++i) {
        const float t = float(i) / float(o.fps);
        while (simTime < t - 1e-6f) {
            game.update(stepDt, in);
            simTime += stepDt;
            ++step;
            const Vec3 rel = game.camera().position - start.position;
            const float along = dot(rel, fwd), side = dot(rel, right);
            path.push_back({along, side});
            maxSide = std::max(maxSide, side);
            minSide = std::min(minSide, side);
            csv << std::format("{},{:.4f},{:.4f},{:.4f},{:.4f}\n", step, simTime, along, side, game.camera().position.y);
            if (step % 10 == 0)
                log::info("  step {:4}  t={:.2f}s  along {:6.2f} m  sideways {:+.3f} m", step, simTime, along, side);
        }
        game.buildInstances(instances);
        FrameSettings s;
        s.camera = game.camera();
        s.rayTracing = o.rayTracing;
        s.hud = o.hud;
        s.exposure = o.exposure;
        s.samples = std::max(2, o.spp);
        s.time = t;
        s.playerMapUV = mapUV(map, game.camera().position);
        applyGame(s, game, o);
        const fs::path file = dir / std::format("frame_{:04}.png", i + 1);
        pending.push_back(std::async(std::launch::async,
                                     [img = renderer.renderToImage(instances, s), file] { return img.writePng(file); }));
        while (pending.size() > 8) {
            writeFailed |= !pending.front().get();
            pending.erase(pending.begin());
        }
    }
    for (auto& f : pending) writeFailed |= !f.get();
    log::info("Sideways from the centre line over {} steps: {:+.3f} .. {:+.3f} m (swing {:.3f} m)", step, minSide,
              maxSide, maxSide - minSide);

    // the plot: corridor seen from above, along = x (true scale), sideways stretched
    const int W = 1800, H = 600, margin = 40;
    Image plot(W, H, rgb(0x0b0f14));
    const float along0 = -0.5f, along1 = std::max(runLength, path.empty() ? 1.0f : path.back().along + 0.5f);
    const float sx = float(W - 2 * margin) / (along1 - along0);
    const float sy = float(H - 2 * margin) / 2.6f;  // +-1.3 m across the image height
    auto X = [&](float a) { return float(margin) + (a - along0) * sx; };
    auto Y = [&](float s) { return float(H) * 0.5f - s * sy; };
    for (float g : {-1.0f, -0.75f, -0.5f, -0.25f, 0.25f, 0.5f, 0.75f, 1.0f})  // 25 cm grid
        plot.fillRect(margin, int(Y(g)), W - margin, int(Y(g)) + 1, std::abs(g) == 0.5f || std::abs(g) == 1.0f
                                                                        ? rgb(0x2a3542)
                                                                        : rgb(0x1a222c));
    for (float a = 0.0f; a < along1; a += 4.0f)  // cell boundaries every 4 m
        plot.fillRect(int(X(a)), margin, int(X(a)) + 1, H - margin, rgb(0x1a222c));
    for (int x = margin; x < W - margin; x += 14) plot.fillRect(x, int(Y(0)), x + 7, int(Y(0)) + 2, rgb(0x6c7a88));
    for (float w : {-1.2f, 1.2f}) plot.fillRect(margin, int(Y(w)) - 2, W - margin, int(Y(w)) + 2, rgb(0xc6d0db));
    for (const Sample& p : path) plot.fillCircle(X(p.along), Y(p.side), 2.2f, rgb(0xff9a3c));
    plot.fillCircle(X(0), Y(0), 6.0f, kStartColor);
    plot.writePng(dir / "weave_plot.png");
    log::info("Wrote weave_plot.png: along the corridor left to right at true scale ({:.0f} px/m); sideways "
              "stretched to {:.0f} px/m; walls (white) at +-1.2 m, grid every 25 cm, centre line dashed",
              sx, sy);
    log::setFile({});
    return writeFailed ? 1 : 0;
}

int runCapture(const Options& o) {
    if (o.scene == "weave") {
        clearFrames(o.output);
        log::setFile(o.output / "capture.log");
        return runWeave(o, o.output);
    }
    const auto started = std::chrono::steady_clock::now();
    const fs::path dir = o.output;
    clearFrames(dir);
    log::setFile(dir / "capture.log");
    const uint64_t seed = o.seed.value_or(kDefaultSeed);

    Kit kit;
    loadKit(kit, o);
    Game game(kit, mazeParams(o, seed));
    game.setFlashlight(o.flashlight);
    describe(game);
    overheadFor(game.maze(), game.level().sections, o.cellPx).image.writePng(dir / "overhead.png");
    const OverheadMap map = gameMap(game);

    const float dt = 1.0f / float(o.fps);
    const bool fireScene = o.scene == "fire";
    const bool crawlScene = o.scene == "crawl";
    FireScene fire;
    if (fireScene) {
        fire.base = sceneCamera(game, "corridor_dark");
        log::info("Scene 'fire': standing at ({:.1f}, {:.1f}), six shots", fire.base.position.x, fire.base.position.z);
    }
    if (crawlScene) {
        if (!game.setupCrawlScene()) {
            log::error("Scene 'crawl': no straight corridor long enough (or the kit has no creature)");
            return 1;
        }
        game.setCrawlTime(0.0f);
        log::info("Scene 'crawl': {:.1f} s, creature path {:.1f} m, camera at s = {:.1f} in a {} section",
                  game.crawlDuration(), game.crawlPath().length(), game.crawlPath().project(game.camera().position),
                  toString(game.moodAtCamera()));
    }
    const bool windowScene = o.scene == "window";
    if (windowScene) {
        if (!game.setupWindowScene()) {
            log::error("Scene 'window': no viewport with two corridor cells beside it (or no creature)");
            return 1;
        }
        log::info("Scene 'window': {:.1f} s, {} viewports in the ship, filmed in a {} section", game.windowSceneLength(),
                  game.level().viewports.size(), toString(game.moodAtCamera()));
    }
    const bool huntScene = o.scene == "hunt" || o.scene == "fight";
    if (huntScene) {
        if (!game.setupHuntScene(o.scene == "fight")) {
            log::error("Scene '{}': no corridor for it (or the kit has no creature)", o.scene);
            return 1;
        }
        log::info("Scene '{}': up to {:.1f} s, in a {} section", o.scene, game.huntSceneLength(),
                  toString(game.moodAtCamera()));
    }
    const float sceneLength = fireScene    ? FireScene::kDuration
                              : crawlScene ? game.crawlDuration()
                              : huntScene  ? game.huntSceneLength()
                              : windowScene ? game.windowSceneLength()
                                           : game.walkDuration();
    const int frames = o.frames > 0 ? o.frames : int(std::ceil((sceneLength - o.start) * float(o.fps)));
    log::info("Capture: {} frames at {}x{}, {} fps, {} spp, RT {}, seed {} -> {}", frames, o.width, o.height, o.fps,
              o.spp, o.rayTracing ? "on" : "off", seed, dir.string());

    Gpu gpu(o.debugLayer);
    Renderer renderer(gpu, nullptr, uint32_t(o.width), uint32_t(o.height));
    uploadMap(renderer, map);
    renderer.setKit(kit);
    renderer.setLevel(game.level());

    // the scene at time t: the walk, or standing and shooting
    int shotsDone = 0;
    auto stepScene = [&](float t) {
        if (fireScene) {
            game.setCamera(fire.at(t));
            game.setTime(t);
            while (shotsDone < 6 && t >= FireScene::kShots[shotsDone]) {
                game.fire();
                ++shotsDone;
            }
        } else if (crawlScene) {
            game.setCrawlTime(t);
        } else if (windowScene) {
            game.setWindowTime(t);
        } else if (huntScene) {
            game.stepHuntScene(t, dt);  // the world steps itself (doors included)
            return;
        } else {
            game.setWalkTime(t);
        }
        game.updateDoors(dt);
    };
    // let the doors (and the explored map) catch up with a late start
    for (float t = 0.0f; t < o.start; t += dt) stepScene(t);
    // the sound the game would have played, beside the frames (audio.wav, audio_events.csv,
    // audio_wave.png)
    SoundBank bank;
    std::unique_ptr<AudioRecorder> audio;
    if (loadSounds(bank, o, true) > 0) {
        audio = std::make_unique<AudioRecorder>(bank, o.fps);
        audio->prime(game);
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
    int written = 0;
    std::string lastHuntLog;
    for (int i = 0; i < frames; ++i) {
        const float t = o.start + float(i) * dt;
        if (huntScene && game.huntSceneDone(t)) break;
        stepScene(t);
        if (audio) audio->frame(game, i);
        ++written;
        if (huntScene && !game.hunters().empty()) {
            const Hunter& h = game.hunters()[0];
            std::string line = std::format("{} hp {}{}", toString(h.state()), h.health(),
                                           game.playerDead() ? ", player dead" : "");
            if (line != lastHuntLog) {
                log::info("  t={:.2f}s frame {}: creature {} at {:.1f} m", t, i + 1, line,
                          length(h.headPosition() - game.camera().position));
                lastHuntLog = line;
            }
        }
        game.buildInstances(instances, !game.playerDead() || game.deathTime() < 0.25f);
        if (!fireScene && !crawlScene && !windowScene && game.reachedExit()) exitFlash = std::min(1.0f, exitFlash + dt / 1.2f);
        FrameSettings s;
        s.camera = game.camera();
        s.rayTracing = o.rayTracing;
        s.hud = o.hud;
        s.exposure = o.exposure;
        s.samples = std::max(2, o.spp);
        s.time = t;
        s.playerMapUV = mapUV(map, game.camera().position);
        s.exitFlash = exitFlash * exitFlash;
        applyGame(s, game, o);
        s.redout = game.redout();
        s.blackout = game.blackout();
        if (game.playerDead()) s.hud = false;
        const fs::path file = dir / std::format("frame_{:04}.png", i + 1);
        pending.push_back(std::async(std::launch::async,
                                     [img = renderer.renderToImage(instances, s), file] { return img.writePng(file); }));
        drain(8);
        if (writeFailed) {
            log::error("Could not write frames to {}", dir.string());
            return 1;
        }
        if ((i + 1) % 30 == 0 || i + 1 == frames)
            log::info("  frame {}/{}  t={:.1f}s  {} triangles, {} section, crouch {:.2f}, {} lights + {} short, {} steam, "
                      "{} spark bursts, {} shots",
                      i + 1, frames, t, renderer.trianglesInScene(), toString(game.moodAtCamera()), game.crouch(),
                      game.level().lights.size(), game.dynamicLights().size(), game.steamPlumes().size(),
                      game.sparksVisible(), game.shotsFired());
    }
    drain(0);
    if (writeFailed) {
        log::error("Could not write frames to {}", dir.string());
        return 1;
    }
    if (audio) {
        std::vector<std::string> lines;
        if (!audio->finish(dir, lines)) {
            log::error("Could not write audio.wav to {}", dir.string());
            return 1;
        }
        for (const std::string& l : lines) log::info("{}", l);
    }

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::ofstream manifest(dir / "capture.json");
    manifest << std::format(
        "{{\n  \"seed\": {},\n  \"maze\": \"{}x{}\",\n  \"frames\": {},\n  \"fps\": {},\n  \"width\": {},\n"
        "  \"height\": {},\n  \"spp\": {},\n  \"rayTracing\": {},\n  \"gpu\": \"{}\",\n  \"rooms\": {},\n"
        "  \"doors\": {},\n  \"lights\": {},\n  \"walkMetres\": {:.1f},\n  \"seconds\": {:.2f}\n}}\n",
        seed, o.mazeWidth, o.mazeHeight, written, o.fps, o.width, o.height, o.spp, o.rayTracing ? "true" : "false",
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
    game.setFlashlight(o.flashlight);
    describe(game);
    overheadFor(game.maze(), game.level().sections, o.cellPx).image.writePng(dir / "overhead.png");
    const OverheadMap map = gameMap(game);

    Gpu gpu(o.debugLayer);
    Renderer renderer(gpu, nullptr, uint32_t(o.width), uint32_t(o.height));
    uploadMap(renderer, map);
    renderer.setKit(kit);
    renderer.setLevel(game.level());

    std::vector<SceneInstance> instances;
    for (const Shot& shot : game.findShots()) {
        game.setCamera(shot.camera);
        game.setTime(kShotTime);
        game.openDoorsNear(shot.camera.position, shot.name == "room" ? 3.0f : 0.0f);
        game.buildInstances(instances);
        const Int2 cell = game.cameraCell();
        log::info("Shot {}: cell ({}, {}), {} section, lights {}", shot.name, cell.x, cell.y,
                  toString(game.moodAtCamera()),
                  toString(game.level().sections.power[size_t(cell.y * game.maze().width() + cell.x)].mode));
        for (bool rt : {true, false}) {
            FrameSettings s;
            s.camera = shot.camera;
            s.rayTracing = rt;
            s.hud = o.hud;
            s.minimap = false;
            s.exposure = o.exposure;
            s.samples = std::max(2, o.spp);
            s.time = kShotTime;
            applyGame(s, game, o);
            s.explored = nullptr;
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
    game.setFlashlight(o.flashlight);
    describe(game);
    const OverheadMap map = gameMap(game);
    Gpu gpu(o.debugLayer);
    Renderer renderer(gpu, nullptr, uint32_t(o.width), uint32_t(o.height));
    uploadMap(renderer, map);
    renderer.setKit(kit);
    renderer.setLevel(game.level());

    const float dt = 1.0f / 60.0f;
    // default: the whole walk from the start time to the exit
    const int frames = o.frames > 0 ? o.frames : std::max(1, int(std::ceil((game.walkDuration() - o.start) / dt)));
    const int saveEvery = frames > 1200 ? 300 : 120;
    // --creatures N: they hunt the walk (and can't grab it: a leap that reaches the camera
    // lands and comes again, so they stay close: a harder case than real play)
    const int creatures = std::max(0, o.creatures);
    if (creatures > 0) {
        game.setHuntTuning(huntTuning(o));
        game.setInvulnerable(true);
        game.spawnHunters(creatures);
        log::info("Bench with {} creatures loose ({} hunt), hunting the walk", game.hunters().size(), o.hunt);
    }
    auto creaturesInView = [&](const Camera& cam) {
        int n = 0;
        const float tanV = std::tan(cam.fovY * 0.5f), tanH = tanV * float(o.width) / float(o.height);
        for (const Hunter& h : game.hunters()) {
            if (h.state() == HunterState::Dead) continue;
            const Vec3 d = h.bodyCenter() - cam.position;
            const float dist = length(d), z = dot(d, cam.forward());
            if (dist > 30.0f || z < 0.1f) continue;
            if (std::abs(dot(d, cam.right())) > tanH * z + 1.2f || std::abs(dot(d, cam.up())) > tanV * z + 1.2f) continue;
            Vec3 nrm;
            if (raycastLevel(game.maze(), game.level(), cam.position, d * (1.0f / dist), dist, game.doorOpen(), nrm) >=
                dist - 0.5f)
                ++n;
        }
        return n;
    };
    std::vector<SceneInstance> instances;
    std::vector<double> ms, msWithCreature, aiMs;
    std::vector<double> byMood[kMoodCount];
    int maxInView = 0;
    std::vector<DynamicLight> extraLights;
    std::vector<int> fixtureLoop, fixtureLive;
    for (int i = 0; i < frames; ++i) {
        const float t = o.start + float(i) * dt;
        game.setWalkTime(t);
        if (creatures > 0) {
            const auto a = std::chrono::steady_clock::now();
            game.stepCreatures(dt);
            aiMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
        }
        game.updateDoors(dt);
        game.buildInstances(instances);
        const int inView = creatures > 0 ? creaturesInView(game.camera()) : 0;
        maxInView = std::max(maxInView, inView);
        FrameSettings s;
        s.camera = game.camera();
        s.rayTracing = o.rayTracing;
        s.hud = o.hud;
        s.exposure = o.exposure;
        s.time = t;
        s.playerMapUV = mapUV(map, game.camera().position);
        s.resetHistory = i == 0;
        applyGame(s, game, o);
        // --test-lights: N more short-lived lights in a ring 2 m round the camera, near the
        // ceiling, like a creature's glow close by (most of the screen within their 5.5 m)
        if (o.testLights > 0) {
            extraLights = game.dynamicLights();
            for (int k = 0; k < o.testLights; ++k) {
                const float a = 2.0f * kPi * float(k) / float(o.testLights);
                const Vec3 p = game.camera().position + Vec3{2.0f * std::cos(a), 0.45f, 2.0f * std::sin(a)};
                extraLights.push_back({p, Vec3{1.0f, 0.42f, 0.08f} * 0.6f, 0.15f, 5.5f});
            }
            s.dynamicLights = &extraLights;
        }
        // fixture lights the shader loops over at the camera: every light in the 3x3 cells
        {
            const Int2 cc = game.cameraCell();
            int n = 0, live = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int x = cc.x + dx, y = cc.y + dy;
                    if (x < 0 || y < 0 || x >= game.level().gridW || y >= game.level().gridH) continue;
                    const size_t c = size_t(y * game.level().gridW + x);
                    for (uint32_t k = 0; k < game.level().cellCount[c]; ++k) {
                        ++n;
                        const Vec3 r = game.lightRadiance()[game.level().cellLights[game.level().cellStart[c] + k]];
                        live += (r.x + r.y + r.z) > 1e-4f ? 1 : 0;
                    }
                }
            fixtureLoop.push_back(n);
            fixtureLive.push_back(live);
        }
        // as the window does: short history for flicker, and for creatures moving nearby
        bool hunterNear = false;
        for (const Hunter& hh : game.hunters())
            hunterNear = hunterNear || (hh.alive() && length(hh.headPosition() - game.camera().position) < 14.0f);
        s.maxHistory = game.lightingChange() > 0.15f ? 3 : hunterNear ? 6 : 16;
        const auto a = std::chrono::steady_clock::now();
        const bool last = i + 1 == frames;
        if ((i + 1) % saveEvery == 0 || last) {
            const Image img = renderer.renderToImage(instances, s);
            img.writePng(dir / std::format("frame_{:04}.png", i + 1));
            if (last) img.writePng(dir / "frame_final.png");
        } else {
            renderer.render(instances, s);
            renderer.gpuIdle();
        }
        const double frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        ms.push_back(frameMs);
        if (inView > 0) msWithCreature.push_back(frameMs);
        byMood[int(game.moodAtCamera())].push_back(frameMs);
    }
    auto report = [&](const std::string& label, std::vector<double> v) {
        if (v.empty()) return;
        std::sort(v.begin(), v.end());
        double avg = 0;
        for (double x : v) avg += x;
        avg /= double(v.size());
        log::info("{}: {} frames, average {:.2f} ms ({:.0f} fps), median {:.2f} ms, 95th {:.2f} ms, worst {:.2f} ms",
                  label, v.size(), avg, 1000.0 / avg, v[v.size() / 2], v[v.size() * 95 / 100], v.back());
    };
    report(std::format("Bench {}x{} RT {} flashlight {}", o.width, o.height, o.rayTracing ? "on" : "off",
                       o.flashlight ? "on" : "off"),
           ms);
    for (int m = 0; m < kMoodCount; ++m) report(std::format("  in {} sections", toString(Mood(m))), byMood[m]);
    if (!fixtureLoop.empty()) {
        double n = 0, live = 0;
        int most = 0;
        for (size_t i = 0; i < fixtureLoop.size(); ++i) {
            n += fixtureLoop[i];
            live += fixtureLive[i];
            most = std::max(most, fixtureLive[i]);
        }
        log::info("  fixture lights in the 3x3 cells round the camera (what each pixel loops over): {:.1f} on average, "
                  "{:.1f} lit (each a shadow ray where in range), at most {} lit",
                  n / double(fixtureLoop.size()), live / double(fixtureLoop.size()), most);
    }
    if (o.testLights > 0) log::info("  plus {} test lights in a ring 2 m round the camera", o.testLights);
    if (creatures > 0) {
        report("  with a creature in view", msWithCreature);
        log::info("  creatures: {} loose, at most {} in view at once; creature AI and their lights (CPU) on the "
                  "same frames:",
                  creatures, maxInView);
        report("  creature step (CPU)", aiMs);
    }
    log::info("(every {}th frame and the last also read back and save a PNG, which shows in 'worst')", saveEvery);
    log::setFile({});
    return 0;
}

// --encounters: how long a simulated player exploring the ship takes to first see a
// creature (src/game/Encounters.h). No GPU.
int runEncounterTimer(const Options& o) {
    const fs::path dir = o.output;
    fs::create_directories(dir);
    log::setFile(dir / "encounters.log");
    Kit kit;
    loadKit(kit, o);
    EncounterConfig cfg;
    cfg.tuning = huntTuning(o);
    cfg.minutes = o.minutes;
    cfg.mazeWidth = o.mazeWidth;
    cfg.mazeHeight = o.mazeHeight;
    std::vector<uint64_t> seeds;
    if (o.seed) {
        seeds.push_back(*o.seed);
    } else {
        const uint64_t named[] = {1234, 7, 42, 2026};
        for (int i = 0; i < o.encounterSeeds; ++i) seeds.push_back(i < 4 ? named[i] : uint64_t(i - 3));
    }
    log::info("Encounter timer: {} ships x {} runs, {:.0f} min each, hunt '{}'", seeds.size(), o.encounterVariants,
              o.minutes, o.hunt);
    const auto started = std::chrono::steady_clock::now();
    if (o.sweep) {
        for (int count : {8, 10, 12})
            for (float dmin : {25.0f, 15.0f})
                for (float hear : {16.0f}) {
                    EncounterConfig c = cfg;
                    c.tuning = HuntTuning::round11();
                    c.tuning.creatures = count;
                    c.tuning.driftMin = dmin;
                    c.tuning.driftMax = dmin * 2.0f;
                    c.tuning.hearWalk = hear;
                    c.tuning.hearRun = hear * 2.2f;
                    for (const std::string& line : summarizeEncounters(runEncounters(kit, seeds, o.encounterVariants, c), c))
                        log::info("{}", line);
                }
        log::info("Sweep done in {:.1f} s", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        log::setFile({});
        return 0;
    }
    const std::vector<EncounterRun> runs = runEncounters(kit, seeds, o.encounterVariants, cfg);
    const char* surfaces[] = {"floor", "wall", "ceiling", "air"};
    std::ofstream csv(dir / "encounters.csv");
    csv << "seed,variant,encounter_s,first_seen_s,first_death_s,first_within_25m_s,seen_on,seen_state,seen_m,"
           "sightings,deaths,view_floor_s,view_wall_s,view_ceiling_s,view_air_s,max_in_view,cells_shown,ms_per_step\n";
    for (const EncounterRun& r : runs) {
        const char* on = r.seenSurface >= 0 ? surfaces[r.seenSurface] : "-";
        const char* st = r.seenState >= 0 ? toString(HunterState(r.seenState)) : "-";
        csv << std::format("{},{},{:.2f},{:.2f},{:.2f},{:.2f},{},{},{:.1f},{},{},{:.1f},{:.1f},{:.1f},{:.1f},{:.0f},{},{:.4f}\n",
                           r.seed, r.variant, r.encounter, r.firstSeen, r.firstGrab, r.firstNear, on, st,
                           r.seenDistance, r.sightings, r.deaths, r.viewSeconds[0], r.viewSeconds[1], r.viewSeconds[2],
                           r.viewSeconds[3], r.maxInView, r.explored, r.msPerStep);
        log::info("  seed {:>5} run {}: encounter {:7.1f} s (seen {:7.1f}, killed {:7.1f}, within 25 m {:6.1f}); "
                  "first seen on the {} while {} at {:.1f} m; {} sightings, {} deaths",
                  r.seed, r.variant, r.encounter, r.firstSeen, r.firstGrab, r.firstNear, on, st, r.seenDistance,
                  r.sightings, r.deaths);
    }
    log::info("(-1 = not in the run)");
    for (const std::string& line : summarizeEncounters(runs, cfg)) log::info("{}", line);
    log::info("Encounter timer done in {:.1f} s",
              std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
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

    // sound: the mixer plays through XAudio2; without a device the game is silent
    SoundBank bank;
    const int sounds = loadSounds(bank, o, true);
    Mixer mixer(bank);
    std::mutex audioLock;
    AudioDevice audioOut;  // (declared after the mixer: closes first)
    if (sounds > 0) {
        std::string err;
        if (audioOut.open(mixer, audioLock, err)) log::info("Sound: playing through XAudio2");
        else log::warn("Sound off: {}", err);
    }
    std::vector<SoundEvent> soundEvents;
    std::vector<SoundLoop> soundLoops;

    std::unique_ptr<Game> game;
    OverheadMap map;
    bool flashlight = o.flashlight;
    auto newShip = [&] {
        game = std::make_unique<Game>(kit, mazeParams(o, seed));
        game->setFlashlight(flashlight);
        game->setHuntTuning(huntTuning(o));
        game->spawnHunters(game->huntTuning().creatures);
        describe(*game);
        map = gameMap(*game);
        uploadMap(renderer, map);
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
        } else if (key == 'F') {
            flashlight = !flashlight;
            game->setFlashlight(flashlight);
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
        in.crouch = window.keyDown(VK_CONTROL) || window.keyDown('C');
        // the click that grabs the mouse does not fire
        in.fire = window.mouseCaptured() && window.keyDown(VK_LBUTTON);
        window.takeMouseDelta(in.mouseDx, in.mouseDy);
        const bool wasDead = game->playerDead();
        game->update(dt, in);
        if (wasDead && !game->playerDead()) reset = true;  // restarted at the spawn
        if (audioOut.running()) {
            game->collectSounds(soundEvents, soundLoops);
            std::lock_guard<std::mutex> g(audioLock);
            mixer.setListener(game->listener());
            for (const SoundEvent& e : soundEvents) mixer.play(e);
            mixer.setLoops(soundLoops);
        }
        game->buildInstances(instances, !game->playerDead() || game->deathTime() < 0.25f);

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
        applyGame(s, *game, o);
        s.redout = game->redout();
        s.blackout = game->blackout();
        if (game->playerDead()) s.hud = false;
        // flickering lights: keep only a few frames of history so the flicker stays sharp;
        // and moving creatures nearby smear with a long history
        bool hunterNear = false;
        for (const Hunter& hh : game->hunters())
            hunterNear = hunterNear || (hh.alive() && length(hh.headPosition() - game->camera().position) < 14.0f);
        s.maxHistory = game->lightingChange() > 0.15f ? 3 : hunterNear ? 6 : 16;
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
            std::string status = escaped                 ? "  |  ESCAPED! N = new ship"
                                 : game->playerDead()    ? "  |  YOU WERE KILLED"
                                                         : "";
            status += std::format("  |  kills {}", game->kills());
            window.setTitle(std::format(
                "Bulkhead  |  seed {}  |  RT {}  |  {:.0f} fps  |  {}  |  WASD move, mouse look, click fire, Ctrl crouch, "
                "Shift run, F flashlight, M map, R ray tracing, H HUD, N new ship, Esc mouse{}",
                seed, rt ? "ON" : "OFF", fps, toString(game->moodAtCamera()), status));
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
            case Options::Mode::Check: return runCheck(*options);
            case Options::Mode::Capture: return runCapture(*options);
            case Options::Mode::Shots: return runShots(*options);
            case Options::Mode::Bench: return runBench(*options);
            case Options::Mode::Encounters: return runEncounterTimer(*options);
            case Options::Mode::SoundCheck: return runSoundCheck(*options);
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
