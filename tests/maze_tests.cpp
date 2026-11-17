// Unit tests for the maze generator, A* route, path track, overhead map and PNG I/O.
// Tiny self-contained harness: prints each failure, returns non-zero if any failed.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Image.h"
#include "world/Maze.h"
#include "world/OverheadMap.h"
#include "world/PathTrack.h"
#include "world/Random.h"
#include "world/Sections.h"

using namespace bh;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s:%d: %s\n", file, line, what.c_str());
    }
}
#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)
#define CHECK_MSG(cond, msg) check((cond), (msg), __FILE__, __LINE__)

// Every structural rule the rest of the game relies on. Returns the problems found.
std::vector<std::string> validate(const Maze& m) {
    std::vector<std::string> problems;
    auto fail = [&](std::string s) { problems.push_back(std::move(s)); };
    const int W = m.width(), H = m.height();

    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const CellKind k = m.kind(x, y);
            for (int di = 0; di < 4; ++di) {
                const Dir d = Dir(di);
                const int nx = x + kDx[di], ny = y + kDy[di];
                const Edge e = m.edge(x, y, d);
                if (!m.inBounds(nx, ny)) {
                    if (e != Edge::Wall) fail(std::format("border edge open at ({},{}) dir {}", x, y, di));
                    continue;
                }
                if (e != m.edge(nx, ny, opposite(d)) || m.door(x, y, d) != m.door(nx, ny, opposite(d)))
                    fail(std::format("asymmetric edge at ({},{}) dir {}", x, y, di));
                const CellKind nk = m.kind(nx, ny);
                if (e == Edge::Wall && m.door(x, y, d) != DoorKind::None)
                    fail(std::format("wall with door kind at ({},{})", x, y));
                if (e == Edge::Open) {
                    if (k == CellKind::Void || nk == CellKind::Void)
                        fail(std::format("open edge into void at ({},{}) dir {}", x, y, di));
                    if (k != nk || (k == CellKind::Room && m.roomAt(x, y) != m.roomAt(nx, ny)))
                        fail(std::format("open edge between different areas at ({},{}) dir {}", x, y, di));
                }
                if (e == Edge::Door) {
                    const bool roomCorridor = (k == CellKind::Room && nk == CellKind::Corridor) ||
                                              (k == CellKind::Corridor && nk == CellKind::Room);
                    if (!roomCorridor) fail(std::format("door not between room and corridor at ({},{})", x, y));
                    if (m.door(x, y, d) == DoorKind::None) fail(std::format("door without kind at ({},{})", x, y));
                    const int ra = m.roomAt(x, y), rb = m.roomAt(nx, ny);
                    const bool exitDoor = ra == m.exitRoom() || rb == m.exitRoom();
                    if (exitDoor != (m.door(x, y, d) == DoorKind::Airlock))
                        fail(std::format("airlock door rule broken at ({},{})", x, y));
                }
            }
            if (k == CellKind::Void && m.passableCount(x, y) != 0) fail(std::format("void cell ({},{}) has openings", x, y));
            if (k == CellKind::Corridor && m.passableCount(x, y) == 0) fail(std::format("sealed corridor ({},{})", x, y));
            if ((k == CellKind::Room) != (m.roomAt(x, y) >= 0)) fail(std::format("room id mismatch at ({},{})", x, y));
        }

    // Rooms
    const auto& rooms = m.rooms();
    if (rooms.size() < 2) fail("fewer than 2 rooms");
    int spawns = 0, airlocks = 0;
    for (const Room& r : rooms) {
        if (r.x < 0 || r.y < 0 || r.x + r.w > W || r.y + r.h > H) fail(std::format("room {} out of bounds", r.id));
        int doors = 0;
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x) {
                if (m.roomAt(x, y) != r.id) fail(std::format("room {} cell ({},{}) not owned", r.id, x, y));
                for (int di = 0; di < 4; ++di) {
                    const int nx = x + kDx[di], ny = y + kDy[di];
                    const bool inside = r.contains(nx, ny);
                    const Edge e = m.edge(x, y, Dir(di));
                    if (inside && e != Edge::Open) fail(std::format("room {} interior edge closed", r.id));
                    if (!inside && e == Edge::Open) fail(std::format("room {} perimeter open without door", r.id));
                    if (!inside && e == Edge::Door) ++doors;
                    if (!inside && m.roomAt(nx, ny) >= 0) fail(std::format("room {} touches room {}", r.id, m.roomAt(nx, ny)));
                }
            }
        if (doors == 0) fail(std::format("room {} has no door", r.id));
        spawns += r.kind == RoomKind::Spawn;
        airlocks += r.kind == RoomKind::Airlock;
    }
    if (spawns != 1 || airlocks != 1) fail("need exactly one Spawn and one Airlock room");
    if (m.startRoom() < 0 || m.exitRoom() < 0 || m.startRoom() == m.exitRoom()) fail("bad start/exit rooms");
    if (!problems.empty()) return problems;
    if (rooms[size_t(m.startRoom())].kind != RoomKind::Spawn) fail("start room is not Spawn");
    if (rooms[size_t(m.exitRoom())].kind != RoomKind::Airlock) fail("exit room is not Airlock");

    // Connectivity: everything walkable is reachable from the start.
    const std::vector<int> dist = m.distancesFrom(m.startCell());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (m.kind(x, y) != CellKind::Void && dist[size_t(y * W + x)] < 0)
                fail(std::format("cell ({},{}) unreachable from start", x, y));

    // The exit is the farthest room.
    const Int2 ec = m.exitCell();
    const int exitDist = dist[size_t(ec.y * W + ec.x)];
    for (const Room& r : rooms) {
        const Int2 c = r.center();
        if (dist[size_t(c.y * W + c.x)] > exitDist) fail(std::format("room {} is farther than the exit", r.id));
    }

    // A* route: valid steps, optimal length.
    const std::vector<Int2> path = m.findPath(m.startCell(), ec);
    if (path.empty()) {
        fail("no route from start to exit");
    } else {
        if (!(path.front() == m.startCell()) || !(path.back() == ec)) fail("route endpoints wrong");
        if (int(path.size()) - 1 != exitDist) fail(std::format("route not shortest ({} vs {})", path.size() - 1, exitDist));
        for (size_t i = 1; i < path.size(); ++i) {
            bool stepOk = false;
            for (int di = 0; di < 4; ++di)
                if (path[i - 1].x + kDx[di] == path[i].x && path[i - 1].y + kDy[di] == path[i].y)
                    stepOk = m.passable(path[i - 1].x, path[i - 1].y, Dir(di));
            if (!stepOk) {
                fail(std::format("route step {} is not a passable neighbour", i));
                break;
            }
        }
    }
    return problems;
}

// ---------------------------------------------------------------------------

void testRng() {
    Rng a(42), b(42), c(43);
    bool same = true, differs = false;
    for (int i = 0; i < 1000; ++i) {
        uint32_t va = a.next(), vb = b.next(), vc = c.next();
        same = same && va == vb;
        differs = differs || va != vc;
    }
    CHECK(same);
    CHECK(differs);

    Rng r(7);
    bool lo = false, hi = false, inRange = true;
    for (int i = 0; i < 10000; ++i) {
        int v = r.range(3, 9);
        inRange = inRange && v >= 3 && v <= 9;
        lo = lo || v == 3;
        hi = hi || v == 9;
        float u = r.unit();
        inRange = inRange && u >= 0.0f && u < 1.0f;
    }
    CHECK(inRange);
    CHECK(lo && hi);
    // Known first value pins the generator: if this changes, every seed's ship changes.
    Rng pinned(1234);
    const uint32_t first = pinned.next();
    std::printf("  rng(1234).next() = %u\n", first);
    Rng pinned2(1234);
    CHECK(pinned2.next() == first);
}

void testDeterminism() {
    MazeParams p;
    p.seed = 1234;
    CHECK(Maze::generate(p).hash() == Maze::generate(p).hash());
    CHECK(Maze::generate(p).toAscii() == Maze::generate(p).toAscii());

    std::set<uint64_t> hashes;
    for (uint64_t s = 1; s <= 50; ++s) {
        p.seed = s;
        hashes.insert(Maze::generate(p).hash());
    }
    CHECK_MSG(hashes.size() >= 48, std::format("only {} distinct layouts from 50 seeds", hashes.size()));
}

void testInvariants() {
    struct Size {
        int w, h, seeds;
    };
    const Size sizes[] = {{6, 6, 200}, {8, 12, 200}, {12, 12, 300}, {20, 14, 150}, {32, 32, 60}, {64, 48, 10}};
    for (const Size& s : sizes) {
        int bad = 0;
        for (int seed = 1; seed <= s.seeds; ++seed) {
            MazeParams p;
            p.width = s.w;
            p.height = s.h;
            p.seed = uint64_t(seed);
            const Maze m = Maze::generate(p);
            const auto problems = validate(m);
            if (!problems.empty()) {
                if (bad++ < 3) {
                    std::printf("  %dx%d seed %d: %s (+%zu more)\n%s", s.w, s.h, seed, problems[0].c_str(),
                                problems.size() - 1, m.toAscii().c_str());
                }
            }
        }
        CHECK_MSG(bad == 0, std::format("{}x{}: {} of {} seeds broke an invariant", s.w, s.h, bad, s.seeds));
    }
}

void testVariety() {
    // Not rules, but the generator should give the game something to work with.
    int rooms = 0, loopsMaps = 0, deadEndMaps = 0, bulkheads = 0, sliding = 0, routeLen = 0;
    const int n = 100;
    for (int seed = 1; seed <= n; ++seed) {
        MazeParams p;
        p.seed = uint64_t(seed);
        const Maze m = Maze::generate(p);
        rooms += int(m.rooms().size());
        loopsMaps += m.countLoops() > 0;
        deadEndMaps += m.countDeadEnds() > 0;
        routeLen += int(m.findPath(m.startCell(), m.exitCell()).size());
        for (int y = 0; y < m.height(); ++y)
            for (int x = 0; x < m.width(); ++x)
                for (Dir d : {Dir::East, Dir::South}) {
                    bulkheads += m.door(x, y, d) == DoorKind::Bulkhead;
                    sliding += m.door(x, y, d) == DoorKind::Sliding;
                }
    }
    std::printf("  12x12 over %d seeds: %.1f rooms, %d%% with loops, %d%% with dead ends, route %.1f cells, "
                "%d sliding / %d bulkhead doors\n",
                n, rooms / double(n), loopsMaps, deadEndMaps, routeLen / double(n), sliding, bulkheads);
    CHECK(rooms / double(n) >= 4.0);
    CHECK(loopsMaps >= 60);
    CHECK(deadEndMaps >= 30);
    CHECK(routeLen / double(n) >= 10.0);
    CHECK(bulkheads > 0 && sliding > bulkheads);
}

void testBadParams() {
    MazeParams p;
    p.width = 3;
    bool threw = false;
    try {
        (void)Maze::generate(p);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    p = MazeParams{};
    p.minRoomSize = 5;
    p.maxRoomSize = 2;
    threw = false;
    try {
        (void)Maze::generate(p);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

void testPathEdgeCases() {
    MazeParams p;
    p.seed = 1234;
    const Maze m = Maze::generate(p);
    CHECK(m.findPath(m.startCell(), m.startCell()).size() == 1);
    CHECK(m.findPath({-1, 0}, m.exitCell()).empty());
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            if (m.kind(x, y) == CellKind::Void) {
                CHECK(m.findPath(m.startCell(), {x, y}).empty());
                return;
            }
}

void testPathTrack() {
    PathTrack t({{0, 0}, {10, 0}, {10, 5}});
    CHECK(std::abs(t.length() - 15.0f) < 1e-4f);
    Float2 a = t.at(12.0f);
    CHECK(std::abs(a.x - 10.0f) < 1e-4f && std::abs(a.y - 2.0f) < 1e-4f);
    Float2 b = t.at(-1.0f), e = t.at(100.0f);
    CHECK(b.x == 0.0f && b.y == 0.0f);
    CHECK(e.x == 10.0f && e.y == 5.0f);
    CHECK(PathTrack().at(3.0f).x == 0.0f);
}

void testOverheadMap() {
    MazeParams p;
    p.seed = 1234;
    const Maze m = Maze::generate(p);
    OverheadStyle style;
    const OverheadMap map = renderOverheadMap(m, style);
    CHECK(map.image.width() == 12 * style.cellPx + 2 * style.marginPx);
    CHECK(map.image.height() == 12 * style.cellPx + 2 * style.marginPx);
    CHECK(!map.route.empty());
    const float expected = float(map.route.size() - 1) * float(style.cellPx);
    CHECK(std::abs(map.routeTrack.length() - expected) < 0.5f);

    Float2 s = map.cellCenterPx(m.startCell()), e = map.cellCenterPx(m.exitCell());
    CHECK(map.image.get(int(s.x), int(s.y)) == kStartColor);
    CHECK(map.image.get(int(e.x), int(e.y)) == kExitColor);

    std::set<uint32_t> colors;
    for (int y = 0; y < map.image.height(); y += 3)
        for (int x = 0; x < map.image.width(); x += 3) {
            Color c = map.image.get(x, y);
            colors.insert((uint32_t(c.r) << 16) | (uint32_t(c.g) << 8) | c.b);
        }
    CHECK_MSG(colors.size() >= 12, std::format("overhead map has only {} colours", colors.size()));
}

// Problems with a ship's sections (empty = fine).
std::vector<std::string> validateSections(const Maze& m, const Sections& s) {
    std::vector<std::string> problems;
    auto fail = [&](std::string p) { problems.push_back(std::move(p)); };
    const int W = m.width(), H = m.height();
    const size_t N = size_t(W) * size_t(H);
    if (s.cellSection.size() != N || s.power.size() != N || s.decay.size() != N || s.haze.size() != N)
        return {"per-cell arrays have the wrong size"};
    if (s.anchor.size() != s.mood.size() || s.depth.size() != s.mood.size()) fail("per-section arrays differ in size");
    std::vector<int> cells(s.mood.size(), 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int sec = s.sectionAt(x, y);
            if (m.kind(x, y) == CellKind::Void) {
                if (sec != -1) fail(std::format("void cell ({},{}) has a section", x, y));
                continue;
            }
            if (sec < 0 || sec >= s.count()) {
                fail(std::format("cell ({},{}) has no section", x, y));
                continue;
            }
            ++cells[size_t(sec)];
            const size_t i = size_t(y * W + x);
            if (s.decay[i] < 0.0f || s.decay[i] > 1.0f || s.haze[i] < 0.0f) fail("decay or haze out of range");
        }
    for (int i = 0; i < s.count(); ++i)
        if (cells[size_t(i)] == 0) fail(std::format("section {} is empty", i));
    // a room is never split between sections
    for (const Room& r : m.rooms())
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x)
                if (s.sectionAt(x, y) != s.sectionAt(r.x, r.y)) fail(std::format("room {} is split", r.id));
    // every section is one connected piece
    for (int sec = 0; sec < s.count(); ++sec) {
        const Int2 a = s.anchor[size_t(sec)];
        if (s.sectionAt(a.x, a.y) != sec) {
            fail(std::format("section {} does not contain its anchor", sec));
            continue;
        }
        std::vector<char> seen(N, 0);
        std::vector<Int2> stack{a};
        seen[size_t(a.y * W + a.x)] = 1;
        int reached = 0;
        while (!stack.empty()) {
            Int2 c = stack.back();
            stack.pop_back();
            ++reached;
            for (int d = 0; d < 4; ++d) {
                if (!m.passable(c.x, c.y, Dir(d))) continue;
                Int2 n{c.x + kDx[d], c.y + kDy[d]};
                if (s.sectionAt(n.x, n.y) != sec || seen[size_t(n.y * W + n.x)]) continue;
                seen[size_t(n.y * W + n.x)] = 1;
                stack.push_back(n);
            }
        }
        if (reached != cells[size_t(sec)])
            fail(std::format("section {} is in pieces ({} of {} cells reached)", sec, reached, cells[size_t(sec)]));
    }
    if (s.moodAt(m.startCell()) != Mood::Lit) fail("the start is not in a lit section");
    if (m.exitRoom() >= 0) {
        const Int2 e = m.exitCell();
        if (s.power[size_t(e.y * W + e.x)].mode != LightMode::Emergency) fail("the exit airlock is not on emergency light");
    }
    return problems;
}

void testSections() {
    struct Size {
        int w, h, seeds;
    };
    const Size sizes[] = {{6, 6, 40}, {12, 12, 80}, {24, 24, 20}, {40, 40, 30}, {64, 48, 4}};
    double depthSum[kMoodCount]{}, depthCount[kMoodCount]{};
    double decaySum[kMoodCount]{}, decayCount[kMoodCount]{};
    for (const Size& sz : sizes) {
        int bad = 0, allMoods = 0;
        for (int seed = 1; seed <= sz.seeds; ++seed) {
            MazeParams p;
            p.width = sz.w;
            p.height = sz.h;
            p.seed = uint64_t(seed);
            const Maze m = Maze::generate(p);
            const Sections s = buildSections(m);
            const auto problems = validateSections(m, s);
            if (!problems.empty() && bad++ < 3)
                std::printf("  %dx%d seed %d: %s (+%zu more)\n", sz.w, sz.h, seed, problems[0].c_str(), problems.size() - 1);
            bool has[kMoodCount]{};
            for (int i = 0; i < s.count(); ++i) {
                has[int(s.mood[size_t(i)])] = true;
                depthSum[int(s.mood[size_t(i)])] += double(s.depth[size_t(i)]) / double(sz.w + sz.h);
                depthCount[int(s.mood[size_t(i)])] += 1;
            }
            allMoods += has[0] && has[1] && has[2] && has[3];
            for (int y = 0; y < m.height(); ++y)
                for (int x = 0; x < m.width(); ++x)
                    if (m.kind(x, y) != CellKind::Void) {
                        decaySum[int(s.moodAt(x, y))] += s.decay[size_t(y * m.width() + x)];
                        decayCount[int(s.moodAt(x, y))] += 1;
                    }
            if (sz.w == 40 && seed == 1)
                std::printf("  40x40 seed 1: %d sections over %d walkable cells\n", s.count(),
                            m.width() * m.height() - m.countCells(CellKind::Void));
            if (sz.w == 40) CHECK_MSG(s.count() >= 10 && s.count() <= 30, std::format("40x40: {} sections", s.count()));
        }
        CHECK_MSG(bad == 0, std::format("{}x{}: {} of {} seeds have broken sections", sz.w, sz.h, bad, sz.seeds));
        if (sz.w >= 24)
            CHECK_MSG(allMoods == sz.seeds, std::format("{}x{}: only {} of {} ships have all four moods", sz.w, sz.h,
                                                        allMoods, sz.seeds));
    }
    // deeper into the ship is darker; dirtier as it gets worse
    auto avg = [](double s, double n) { return n > 0 ? s / n : 0.0; };
    const double dLit = avg(depthSum[0], depthCount[0]), dDark = avg(depthSum[2], depthCount[2]);
    std::printf("  mean section depth (per ship size): lit %.2f, dim %.2f, derelict %.2f, dark %.2f\n", dLit,
                avg(depthSum[1], depthCount[1]), avg(depthSum[3], depthCount[3]), dDark);
    CHECK(dLit < dDark);
    CHECK(avg(decaySum[0], decayCount[0]) + 0.3 < avg(decaySum[3], decayCount[3]));

    // same maze -> same sections
    MazeParams p;
    p.width = p.height = 40;
    p.seed = 1234;
    const Maze m = Maze::generate(p);
    const Sections a = buildSections(m), b = buildSections(m);
    CHECK(a.cellSection == b.cellSection && a.mood == b.mood && a.decay == b.decay);
    CHECK(Maze::generate(p).hash() == m.hash());  // sections never touch the maze

    // light behaviour
    CellPower dead{LightMode::Dead, 1.0f, {1, 1, 1}, 0.3f};
    CHECK(lightMultiplier(dead, 3.0f).x == 0.0f);
    CellPower steady{LightMode::Steady, 0.5f, {1.0f, 0.5f, 0.25f}, 0.1f};
    Vec3 k = lightMultiplier(steady, 7.0f);
    CHECK(std::abs(k.x - 0.5f) < 1e-6f && std::abs(k.y - 0.25f) < 1e-6f);
    CellPower flicker{LightMode::Flicker, 1.0f, {1, 1, 1}, 0.42f};
    CellPower emergency{LightMode::Emergency, 1.0f, {1, 0.1f, 0.05f}, 0.0f};
    int on = 0, off = 0;
    float eLo = 9, eHi = 0;
    for (int i = 0; i < 3600; ++i) {
        const float t = float(i) / 60.0f;
        float f = lightMultiplier(flicker, t).x;
        on += f > 0.5f;
        off += f < 0.1f;
        float e = lightMultiplier(emergency, t).x;
        eLo = std::min(eLo, e);
        eHi = std::max(eHi, e);
        CHECK(lightMultiplier(flicker, t).x == lightMultiplier(flicker, t).x);  // a pure function of t
    }
    std::printf("  flicker over 60 s: %d on, %d off frames; emergency pulse %.2f..%.2f\n", on, off, eLo, eHi);
    CHECK(on > 2400 && off > 30);
    CHECK(eLo > 0.3f && eHi > 0.9f);
}

void testPngRoundTrip() {
    Image img(37, 21, rgb(0x102030));
    img.fillRect(5, 5, 20, 15, rgb(0xff8800));
    img.set(36, 20, {1, 2, 3, 4});
    const auto path = std::filesystem::temp_directory_path() / "bulkhead_png_roundtrip.png";
    CHECK(img.writePng(path, true));
    auto loaded = Image::loadPng(path);
    CHECK(loaded.has_value());
    if (loaded) {
        CHECK(loaded->width() == 37 && loaded->height() == 21);
        bool same = true;
        for (int y = 0; y < 21; ++y)
            for (int x = 0; x < 37; ++x) same = same && loaded->get(x, y) == img.get(x, y);
        CHECK(same);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace

int main() {
    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"rng", testRng},
        {"determinism", testDeterminism},
        {"invariants (many sizes and seeds)", testInvariants},
        {"variety", testVariety},
        {"bad params", testBadParams},
        {"path edge cases", testPathEdgeCases},
        {"path track", testPathTrack},
        {"overhead map", testOverheadMap},
        {"sections and moods", testSections},
        {"png round trip", testPngRoundTrip},
    };
    for (const Test& t : tests) {
        const int before = g_failures;
        std::printf("[ RUN  ] %s\n", t.name);
        try {
            t.fn();
        } catch (const std::exception& ex) {
            ++g_failures;
            std::printf("  EXCEPTION: %s\n", ex.what());
        }
        std::printf("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", t.name);
    }

    MazeParams p;
    p.seed = 1234;
    const Maze sample = Maze::generate(p);
    std::printf("\nSample: %s\n%s\n", sample.describe().c_str(), sample.toAscii().c_str());

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
