#include "world/OverheadMap.h"

#include <algorithm>
#include <cmath>

namespace bh {

Color roomFloorColor(RoomKind k) {
    switch (k) {
        case RoomKind::Spawn: return rgb(0x2c5a45);
        case RoomKind::Airlock: return rgb(0x6b2a2a);
        case RoomKind::Storage: return rgb(0x51492f);
        case RoomKind::Engineering: return rgb(0x2f4760);
        case RoomKind::Crew: return rgb(0x433456);
        case RoomKind::Medbay: return rgb(0x2a5558);
        case RoomKind::Reactor: return rgb(0x5e4026);
    }
    return rgb(0x444444);
}

Color moodColor(Mood m) {
    switch (m) {
        case Mood::Lit: return rgb(0x3b5470);       // cool steel blue
        case Mood::Dim: return rgb(0x5c4826);       // amber
        case Mood::Dark: return rgb(0x1e1a3a);      // deep indigo
        case Mood::Derelict: return rgb(0x5a2e22);  // rust
    }
    return rgb(0x2c3644);
}

namespace {

Color mix(Color a, Color b, float t) {
    auto ch = [t](uint8_t x, uint8_t y) { return uint8_t(std::lround(float(x) + (float(y) - float(x)) * t)); };
    return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), 255};
}

constexpr Color kSectionGate = rgb(0xb8c8d8);

constexpr Color kBackground = rgb(0x0b0f14);
constexpr Color kVoid = rgb(0x10151b);
constexpr Color kVoidHatch = rgb(0x18202a);
constexpr Color kCorridor = rgb(0x2c3644);
constexpr Color kCorridorGrate = rgb(0x232b36);
constexpr Color kWall = rgb(0xc6d0db);
constexpr Color kDoorFrame = rgb(0xe9eef4);
constexpr Color kSlidingDoor = rgb(0xe8a33a);
constexpr Color kDoorWindow = rgb(0x9fe3ff);
constexpr Color kHazardYellow = rgb(0xe2c044);
constexpr Color kHazardBlack = rgb(0x262626);
constexpr Color kAirlockDoor = rgb(0xff4646);
constexpr Color kAirlockStripe = rgb(0xffb0b0);
constexpr Color kRoute = rgb(0x3fd0e0);

// One grid edge as drawn on the map. Vertical edges run along y at x = line.
struct EdgeLine {
    bool vertical;
    int line;   // pixel coordinate across the edge
    int start;  // pixel coordinate where the edge begins along its length
};

// Fills a band of `thickness` pixels centred on the edge line, covering [a, b) along it.
// colorAt receives the distance along the band from `a`.
template <class ColorFn>
void fillAlong(Image& img, const EdgeLine& e, int a, int b, int thickness, ColorFn colorAt) {
    const int across0 = e.line - thickness / 2;
    for (int along = a; along < b; ++along)
        for (int across = across0; across < across0 + thickness; ++across) {
            Color c = colorAt(along - a);
            if (e.vertical) img.set(across, along, c);
            else img.set(along, across, c);
        }
}

}  // namespace

OverheadMap renderOverheadMap(const Maze& maze, const OverheadStyle& style) {
    const int c = std::max(8, style.cellPx);
    const int mg = std::max(0, style.marginPx);
    OverheadMap out;
    out.cellPx = c;
    out.marginPx = mg;
    out.image = Image(maze.width() * c + 2 * mg, maze.height() * c + 2 * mg, kBackground);
    Image& img = out.image;

    // --- Floors -----------------------------------------------------------
    const int grateInset = std::max(2, c / 10);
    const int grateStep = std::max(3, c / 11);
    const int panelInset = std::max(2, c / 16);
    const Sections* sec = style.sections;
    // neighbouring sections with the same mood differ a little in brightness
    auto sectionShade = [&](int x, int y) {
        int s = sec->sectionAt(x, y);
        return s < 0 ? 1.0f : 0.9f + 0.2f * float((uint32_t(s) * 2654435761u) >> 24) / 255.0f;
    };
    for (int y = 0; y < maze.height(); ++y)
        for (int x = 0; x < maze.width(); ++x) {
            const int px = mg + x * c, py = mg + y * c;
            switch (maze.kind(x, y)) {
                case CellKind::Void:
                    for (int j = py; j < py + c; ++j)
                        for (int i = px; i < px + c; ++i) img.set(i, j, ((i + j) % 10) < 2 ? kVoidHatch : kVoid);
                    break;
                case CellKind::Corridor: {
                    Color floor = sec ? shade(moodColor(sec->moodAt(x, y)), sectionShade(x, y)) : kCorridor;
                    Color grate = sec ? shade(floor, 0.75f) : kCorridorGrate;
                    img.fillRect(px, py, px + c, py + c, floor);
                    for (int j = py + grateInset; j < py + c - grateInset; ++j)
                        for (int i = px + grateInset; i < px + c - grateInset; ++i)
                            if ((i - px) % grateStep == 0 || (j - py) % grateStep == 0) img.set(i, j, grate);
                    break;
                }
                case CellKind::Room: {
                    Color floor = roomFloorColor(maze.rooms()[size_t(maze.roomAt(x, y))].kind);
                    if (sec) floor = shade(mix(floor, moodColor(sec->moodAt(x, y)), 0.5f), sectionShade(x, y));
                    img.fillRect(px, py, px + c, py + c, shade(floor, 0.8f));  // tile seams
                    img.fillRect(px + 1, py + 1, px + c - 1, py + c - 1, floor);
                    img.fillRect(px + panelInset, py + panelInset, px + c - panelInset, py + c - panelInset,
                                 shade(floor, 1.1f));
                    break;
                }
            }
        }

    // --- Route --------------------------------------------------------------
    if (style.drawRoute) {
        out.route = maze.findPath(maze.startCell(), maze.exitCell());
        std::vector<Float2> pts;
        for (Int2 cell : out.route) pts.push_back(out.cellCenterPx(cell));
        out.routeTrack = PathTrack(pts);

        const int t = std::max(3, c / 9);
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            int x0 = int(std::min(pts[i].x, pts[i + 1].x)), x1 = int(std::max(pts[i].x, pts[i + 1].x));
            int y0 = int(std::min(pts[i].y, pts[i + 1].y)), y1 = int(std::max(pts[i].y, pts[i + 1].y));
            img.fillRect(x0 - t / 2, y0 - t / 2, x1 - t / 2 + t, y1 - t / 2 + t, kRoute);
        }
    }

    // --- Section borders: a dashed gate across every opening between sections ---
    if (sec) {
        const int gt = std::max(2, c / 20);
        const int dash = std::max(3, c / 10);
        for (int y = 0; y < maze.height(); ++y)
            for (int x = 0; x < maze.width(); ++x)
                for (Dir d : {Dir::East, Dir::South}) {
                    if (!maze.passable(x, y, d)) continue;
                    const int nx = x + kDx[int(d)], ny = y + kDy[int(d)];
                    if (sec->sectionAt(x, y) == sec->sectionAt(nx, ny)) continue;
                    const int px = mg + x * c, py = mg + y * c;
                    EdgeLine e = d == Dir::East ? EdgeLine{true, px + c, py} : EdgeLine{false, py + c, px};
                    const int a = e.start + c / 5, b = e.start + c - c / 5;
                    for (int s0 = a; s0 < b; s0 += 2 * dash)
                        fillAlong(img, e, s0, std::min(s0 + dash, b), gt, [](int) { return kSectionGate; });
                }
    }

    // --- Walls and doors ---------------------------------------------------
    const int wt = std::max(2, c / 12);
    const int half = wt / 2;
    const int postT = wt + 2;
    const int doorT = wt + std::max(2, c / 16);
    const int doorA = int(c * 0.24f), doorB = int(c * 0.76f);
    const int stripe = std::max(3, c / 12);

    auto drawEdge = [&](int x, int y, Dir d, const EdgeLine& e) {
        const int nx = x + kDx[int(d)], ny = y + kDy[int(d)];
        if (maze.kind(x, y) == CellKind::Void && maze.kind(nx, ny) == CellKind::Void) return;
        const Edge type = maze.edge(x, y, d);
        if (type == Edge::Open) return;
        auto solid = [](Color col) { return [col](int) { return col; }; };
        if (type == Edge::Wall) {
            fillAlong(img, e, e.start - half, e.start + c + (wt - half), wt, solid(kWall));
            return;
        }
        // Door: frame posts at both ends, the door leaf in the middle.
        fillAlong(img, e, e.start - half, e.start + doorA, postT, solid(kDoorFrame));
        fillAlong(img, e, e.start + doorB, e.start + c + (wt - half), postT, solid(kDoorFrame));
        const int a = e.start + doorA, b = e.start + doorB;
        switch (maze.door(x, y, d)) {
            case DoorKind::Bulkhead:
                fillAlong(img, e, a, b, doorT, [&](int along) { return (along / stripe) % 2 ? kHazardBlack : kHazardYellow; });
                break;
            case DoorKind::Airlock:
                fillAlong(img, e, a, b, doorT, solid(kAirlockDoor));
                fillAlong(img, e, a + 2, b - 2, std::max(1, doorT / 4), solid(kAirlockStripe));
                break;
            default: {  // sliding door with a small window
                fillAlong(img, e, a, b, doorT, solid(kSlidingDoor));
                const int win = std::max(2, c / 7);
                const int mid = (a + b) / 2;
                fillAlong(img, e, mid - win / 2, mid - win / 2 + win, std::max(1, doorT - 4), solid(kDoorWindow));
                break;
            }
        }
    };

    for (int y = 0; y < maze.height(); ++y)
        for (int x = 0; x < maze.width(); ++x) {
            const int px = mg + x * c, py = mg + y * c;
            drawEdge(x, y, Dir::East, {true, px + c, py});
            drawEdge(x, y, Dir::South, {false, py + c, px});
            if (x == 0) drawEdge(x, y, Dir::West, {true, px, py});
            if (y == 0) drawEdge(x, y, Dir::North, {false, py, px});
        }

    // --- Start and exit markers --------------------------------------------
    const Color white = rgb(0xffffff);
    Float2 s = out.cellCenterPx(maze.startCell());
    img.fillCircle(s.x, s.y, c * 0.3f, kStartColor);
    img.strokeCircle(s.x, s.y, c * 0.3f, 3.0f, white);

    Float2 e = out.cellCenterPx(maze.exitCell());
    img.fillCircle(e.x, e.y, c * 0.32f, kExitColor);
    img.fillCircle(e.x, e.y, c * 0.2f, rgb(0x3a0d0d));
    img.fillCircle(e.x, e.y, c * 0.1f, kExitColor);
    img.strokeCircle(e.x, e.y, c * 0.32f, 3.0f, white);

    return out;
}

}  // namespace bh
