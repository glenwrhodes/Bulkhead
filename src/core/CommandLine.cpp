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
        "Bulkhead - sci-fi ship maze shooter (round 1: engine skeleton + maze generator)\n"
        "\n"
        "  Bulkhead.exe                          play in a window\n"
        "  Bulkhead.exe --capture <dir>          render headless and write <dir>/frame_0001.png ...\n"
        "  Bulkhead.exe --overhead <file.png>    write the top-down map of the maze (no GPU)\n"
        "\n"
        "Options:\n"
        "  --seed N          maze seed (capture/overhead default 1234, window default random)\n"
        "  --size WxH        maze size in cells (default 12x12, 6..128)\n"
        "  --frames N        frames to capture (default 120)\n"
        "  --fps N           capture time step (default 30)\n"
        "  --res WxH         render resolution / window size (default 1280x720)\n"
        "  --cell-px N       overhead map pixels per cell (default 64)\n"
        "  --debug           enable the D3D12 debug layer\n"
        "  --help            this text\n"
        "\n"
        "Window keys: Esc quit, M toggle map, N next seed, F12 screenshot to captures/.\n";
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
        } else if (a == "--capture" || a == "--overhead") {
            auto v = value();
            if (!v) return std::nullopt;
            o.mode = a == "--capture" ? Options::Mode::Capture : Options::Mode::Overhead;
            o.output = std::filesystem::path(std::string(*v));
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
        } else if (a == "--frames" || a == "--fps" || a == "--cell-px") {
            auto v = value();
            int n = 0;
            if (!v) return std::nullopt;
            if (!parseNumber(*v, n) || n <= 0) return bad(*v);
            if (a == "--frames") o.frames = n;
            else if (a == "--fps") o.fps = n;
            else o.cellPx = n;
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
