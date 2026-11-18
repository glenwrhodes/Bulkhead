#include "core/CommandLine.h"

#include <charconv>
#include <format>
#include <string_view>

namespace bh {
namespace {

template <class T>
bool parseNumber(std::string_view s, T& out) {
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

bool parsePair(std::string_view s, int& a, int& b) {
    size_t x = s.find('x');
    if (x == std::string_view::npos) return false;
    return parseNumber(s.substr(0, x), a) && parseNumber(s.substr(x + 1), b);
}

}  // namespace

std::string usageText() {
    return
        "Bulkhead - ray-traced sci-fi ship maze shooter\n"
        "\n"
        "  Bulkhead.exe                          play in a window\n"
        "  Bulkhead.exe --capture <dir>          headless: walk start -> exit, write <dir>/frame_0001.png ...\n"
        "  Bulkhead.exe --shots <dir>            headless: corridor/door/room/junction views, RT on and off\n"
        "  Bulkhead.exe --bench <dir>            headless: play the walk in real time like the window does,\n"
        "                                        report frame times, save sample frames\n"
        "  Bulkhead.exe --overhead <file.png>    write the top-down map of the maze (no GPU)\n"
        "  Bulkhead.exe --check                  load the models and build a ship, then report (no GPU)\n"
        "  Bulkhead.exe --sound-check            open the sound device and play two sounds (~1 s)\n"
        "\n"
        "Options:\n"
        "  --seed N          maze seed (capture/overhead default 1234, window default random)\n"
        "  --size WxH        maze size in cells (default 40x40, 6..128)\n"
        "  --res WxH         render resolution / window size (default 1280x720)\n"
        "  --frames N        frames to capture or bench (default: the whole walk)\n"
        "  --fps N           capture frame rate (default 30)\n"
        "  --start S         start the capture S seconds into the walk\n"
        "  --spp N           capture samples per pixel (default 8)\n"
        "  --rt on|off       ray tracing (default on)\n"
        "  --exposure X      exposure multiplier (default 1)\n"
        "  --flashlight on|off  the flashlight under the gun (default on)\n"
        "  --scene walk|fire|crawl|hunt|fight  capture: the walk start -> exit (default), firing six shots,\n"
        "                    the creature crawling over you, a creature hunting you down, or killing one\n"
        "  --creatures N     creatures loose in the ship (window, bench, encounters; 0..24; default:\n"
        "                    the game's, see --hunt; the bench has none unless given)\n"
        "  --hunt new|original  how creatures find you: new (more of them, they drift toward where\n"
        "                    you roughly are and hear footsteps) or original (before round 11)\n"
        "  --encounters <dir>  no GPU: time how long a simulated player exploring the ship takes to\n"
        "                    first see a creature; writes encounters.log and encounters.csv\n"
        "  --runs SxV        encounters: S ships x V runs on each (default 10x3)\n"
        "  --minutes M       encounters: length of each run (default 10)\n"
        "  --sweep           encounters: try a grid of creature counts, drift timings and hearing\n"
        "  --test-lights N   bench: add N small short-lived lights in a ring round the camera (what\n"
        "                    each one costs per frame)\n"
        "  --scene approach  capture: you stand still; a creature that has spotted you closes in\n"
        "                    and leaps (the real AI; with --hunt original, as before round 13)\n"
        "  --scene window    capture: walk up to a viewport on the ship's edge, the stars outside,\n"
        "                    the creature crawling past behind you in the glass, turn to face it\n"
        "  --scene weave     capture: the window's movement code, forward held, down a straight\n"
        "                    corridor; logs the sideways drift (weave.csv, weave_plot.png)\n"
        "  --step-hz N       weave: game steps per second (default 100, the window's typical rate)\n"
        "  --haze X          haze density multiplier (default 1, 0 = none)\n"
        "  --no-hud          hide crosshair and minimap\n"
        "  --assets DIR      kit folder (default: found next to the executable or in the source tree)\n"
        "  --cell-px N       overhead map pixels per cell (default 64)\n"
        "  --debug           enable the D3D12 debug layer\n"
        "  --help            this text\n"
        "\n"
        "Window: WASD move, mouse look, left click fire, Ctrl (or C) crouch, Shift run, click to grab the\n"
        "        mouse, Esc release (twice: quit), F flashlight, M map, R ray tracing on/off, H HUD,\n"
        "        N new ship, F12 screenshot.\n";
}

std::optional<Options> parseCommandLine(int argc, char** argv, std::string& error) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto value = [&]() -> std::optional<std::string_view> {
            if (i + 1 >= argc) {
                error = std::format("{} needs a value", a);
                return std::nullopt;
            }
            return std::string_view(argv[++i]);
        };
        auto bad = [&](std::string_view v) {
            error = std::format("bad value '{}' for {}", v, a);
            return std::nullopt;
        };

        if (a == "--help" || a == "-h" || a == "/?") {
            o.mode = Options::Mode::Help;
        } else if (a == "--check") {
            o.mode = Options::Mode::Check;
        } else if (a == "--sound-check") {
            o.mode = Options::Mode::SoundCheck;
        } else if (a == "--capture" || a == "--overhead" || a == "--shots" || a == "--bench" || a == "--encounters") {
            auto v = value();
            if (!v) return std::nullopt;
            o.mode = a == "--capture"      ? Options::Mode::Capture
                     : a == "--shots"      ? Options::Mode::Shots
                     : a == "--bench"      ? Options::Mode::Bench
                     : a == "--encounters" ? Options::Mode::Encounters
                                           : Options::Mode::Overhead;
            o.output = std::filesystem::path(std::string(*v));
        } else if (a == "--scene") {
            auto v = value();
            if (!v) return std::nullopt;
            if (*v != "walk" && *v != "fire" && *v != "crawl" && *v != "hunt" && *v != "fight" && *v != "weave" &&
                *v != "window" && *v != "approach")
                return bad(*v);
            o.scene = std::string(*v);
        } else if (a == "--assets") {
            auto v = value();
            if (!v) return std::nullopt;
            o.assets = std::filesystem::path(std::string(*v));
        } else if (a == "--seed") {
            auto v = value();
            uint64_t s = 0;
            if (!v) return std::nullopt;
            if (!parseNumber(*v, s)) return bad(*v);
            o.seed = s;
        } else if (a == "--size") {
            auto v = value();
            if (!v) return std::nullopt;
            if (!parsePair(*v, o.mazeWidth, o.mazeHeight)) return bad(*v);
        } else if (a == "--res") {
            auto v = value();
            if (!v) return std::nullopt;
            if (!parsePair(*v, o.width, o.height) || o.width < 16 || o.height < 16 || o.width > 8192 ||
                o.height > 8192)
                return bad(*v);
        } else if (a == "--step-hz") {
            auto v = value();
            int n = 0;
            if (!v) return std::nullopt;
            if (!parseNumber(*v, n) || n < 10 || n > 1000) return bad(*v);
            o.stepHz = n;
        } else if (a == "--creatures") {
            auto v = value();
            int n = 0;
            if (!v) return std::nullopt;
            if (!parseNumber(*v, n) || n < 0 || n > 24) return bad(*v);
            o.creatures = n;
        } else if (a == "--hunt") {
            auto v = value();
            if (!v) return std::nullopt;
            if (*v != "new" && *v != "original") return bad(*v);
            o.hunt = std::string(*v);
        } else if (a == "--runs") {
            auto v = value();
            if (!v) return std::nullopt;
            if (!parsePair(*v, o.encounterSeeds, o.encounterVariants) || o.encounterSeeds < 1 ||
                o.encounterVariants < 1 || o.encounterSeeds * o.encounterVariants > 400)
                return bad(*v);
        } else if (a == "--minutes") {
            auto v = value();
            float f = 0;
            if (!v) return std::nullopt;
            if (!parseNumber(*v, f) || f <= 0.0f || f > 60.0f) return bad(*v);
            o.minutes = f;
        } else if (a == "--frames" || a == "--fps" || a == "--cell-px" || a == "--spp") {
            auto v = value();
            int n = 0;
            if (!v) return std::nullopt;
            if (!parseNumber(*v, n) || n <= 0) return bad(*v);
            if (a == "--frames") o.frames = n;
            else if (a == "--fps") o.fps = n;
            else if (a == "--spp") o.spp = std::min(n, 64);
            else o.cellPx = n;
        } else if (a == "--start" || a == "--exposure" || a == "--haze") {
            auto v = value();
            float f = 0;
            if (!v) return std::nullopt;
            if (!parseNumber(*v, f) || f < 0) return bad(*v);
            if (a == "--start") o.start = f;
            else if (a == "--haze") o.haze = f;
            else o.exposure = f;
        } else if (a == "--rt" || a == "--flashlight") {
            auto v = value();
            if (!v) return std::nullopt;
            bool on = false;
            if (*v == "on") on = true;
            else if (*v == "off") on = false;
            else return bad(*v);
            if (a == "--rt") o.rayTracing = on;
            else o.flashlight = on;
        } else if (a == "--test-lights") {
            auto v = value();
            int n = 0;
            if (!v) return std::nullopt;
            if (!parseNumber(*v, n) || n < 0 || n > 32) return bad(*v);
            o.testLights = n;
        } else if (a == "--sweep") {
            o.sweep = true;
        } else if (a == "--no-hud") {
            o.hud = false;
        } else if (a == "--debug") {
            o.debugLayer = true;
        } else {
            error = std::format("unknown argument '{}'", a);
            return std::nullopt;
        }
    }
    return o;
}

}  // namespace bh
