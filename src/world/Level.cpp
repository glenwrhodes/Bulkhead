#include "world/Level.h"

#include <algorithm>
#include <cmath>

#include "world/Random.h"

namespace bh {
namespace {

constexpr float kHalf = 2.0f;
constexpr float kWalk = 1.2f;  // corridor walkway half width

int bitOf(Dir d) { return 1 << int(d); }

// Canonical open-side masks of the corridor pieces (N=1, E=2, S=4, W=8).
struct CorridorPiece {
    const char* model;
    int mask;
};
constexpr CorridorPiece kCorridors[] = {
    {"corridor_straight", 1 | 4}, {"corridor_corner", 1 | 2}, {"corridor_tee", 1 | 2 | 4},
    {"corridor_cross", 15}, {"corridor_end", 1}};

// Rotating a piece by one quarter turn counter-clockwise maps N->W, E->N, S->E, W->S.
int rotateMask(int mask, int quarterTurns) {
    int out = 0;
    for (int d = 0; d < 4; ++d)
        if (mask & (1 << d)) out |= 1 << ((d + 3 * quarterTurns) % 4);
    return out;
}

class Builder {
public:
    Builder(const Maze& m, const Kit& k, Level& l)
        : maze(m), kit(k), level(l), dress(m.params().seed, kDressStream), windows(m.params().seed, kWindowStream) {}

    // The draws place() would have taken from the dressing stream for a model it now
    // doesn't place (one per spark or steam marker), so later cells don't change.
    void skipDraws(const char* name) {
        const int idx = kit.find(name);
        if (idx < 0) return;
        for (const KitMarker& mk : kit.model(idx).markers)
            if (mk.name.starts_with("spark") || mk.name.starts_with("steam")) (void)dress.unit();
    }

    // Viewports (part 3): about half the straight corridor cells whose wall lies on the
    // ship's edge get corridor_straight_window, turned so the windows face out. Their own
    // random stream: the maze, the moods and the dressing don't change.
    void chooseViewports(bool enabled) {
        windowOut.assign(size_t(maze.width() * maze.height()), -1);
        if (!enabled || kit.find("corridor_straight_window") < 0) return;
        for (int y = 0; y < maze.height(); ++y)
            for (int x = 0; x < maze.width(); ++x) {
                if (!straightCell(x, y) || lowPipeCell[size_t(y * maze.width() + x)]) continue;
                int out = -1;
                if (x == 0 && !maze.passable(x, y, Dir::West)) out = int(Dir::West);
                else if (x == maze.width() - 1 && !maze.passable(x, y, Dir::East)) out = int(Dir::East);
                else if (y == 0 && !maze.passable(x, y, Dir::North)) out = int(Dir::North);
                else if (y == maze.height() - 1 && !maze.passable(x, y, Dir::South)) out = int(Dir::South);
                if (out >= 0 && windows.chance(0.5f)) windowOut[size_t(y * maze.width() + x)] = out;
            }
    }

    int place(const char* name, Mat4 t, int door = -1, bool selfPowered = false) {
        int idx = kit.find(name);
        if (idx < 0) {
            if (std::find(level.missingModels.begin(), level.missingModels.end(), name) == level.missingModels.end())
                level.missingModels.push_back(name);
            return -1;
        }
        level.placed.push_back({idx, t, door, cell, selfPowered});
        for (const KitLight& l : kit.model(idx).lights) {
            WorldLight w;
            w.position = t.point(l.position);
            w.radiance = l.color * l.intensity;
            w.radius = l.radius;
            w.beacon = selfPowered;
            level.lights.push_back(w);
        }
        // effect anchors: sparks and steam
        for (const KitMarker& mk : kit.model(idx).markers) {
            Emitter e;
            if (mk.name.starts_with("spark")) e.kind = Emitter::Spark;
            else if (mk.name.starts_with("steam")) e.kind = Emitter::Steam;
            else continue;
            e.position = t.point(mk.position);
            e.direction = normalize(t.dir(mk.direction));
            e.cell = cell;
            e.phase = dress.unit();
            level.emitters.push_back(e);
        }
        return idx;
    }

    // A collision box given in a straight corridor piece's canonical Blender frame
    // (x east, y north): x0..x1, y0..y1, placed with transform t.
    void boxCanon(const Mat4& t, float x0, float y0, float x1, float y1, float bottom = 0.0f) {
        float ax = 1e9f, az = 1e9f, bx = -1e9f, bz = -1e9f;
        for (float bxv : {x0, x1})
            for (float byv : {y0, y1}) {
                Vec3 p = t.point({bxv, 0.0f, -byv});  // Blender (x, y) -> model (x, 0, -y)
                ax = std::min(ax, p.x), az = std::min(az, p.z), bx = std::max(bx, p.x), bz = std::max(bz, p.z);
            }
        level.collision.add({ax, az, bx, bz, -1, bottom});
    }

    bool straightCell(int x, int y) const {
        if (maze.kind(x, y) != CellKind::Corridor) return false;
        int open = 0;
        for (int d = 0; d < 4; ++d)
            if (maze.passable(x, y, Dir(d))) open |= 1 << d;
        return open == (1 | 4) || open == (2 | 8);
    }

    // Low pipe bundles: two on the walk route (about a third and two thirds of the way,
    // outside lit sections, so the film always ducks under one), and a few elsewhere.
    void chooseLowPipes() {
        lowPipeCell.assign(size_t(maze.width() * maze.height()), 0);
        const auto& route = level.route;
        std::vector<int> candidates;
        for (int i = 3; i + 3 < int(route.size()); ++i)
            if (straightCell(route[size_t(i)].x, route[size_t(i)].y) &&
                level.sections.moodAt(route[size_t(i)]) != Mood::Lit)
                candidates.push_back(i);
        int last = -100;
        for (float f : {0.35f, 0.7f}) {
            int want = int(f * float(route.size())), best = -1;
            for (int i : candidates)
                if (std::abs(i - last) >= 6 && (best < 0 || std::abs(i - want) < std::abs(best - want))) best = i;
            if (best < 0) continue;
            lowPipeCell[size_t(route[size_t(best)].y * maze.width() + route[size_t(best)].x)] = 1;
            last = best;
        }
        for (int y = 0; y < maze.height(); ++y)
            for (int x = 0; x < maze.width(); ++x)
                if (straightCell(x, y) && level.sections.moodAt(x, y) != Mood::Lit && dress.chance(0.03f))
                    lowPipeCell[size_t(y * maze.width() + x)] = 1;
    }

    // Derelict dressing of a straight corridor cell, by mood.
    // windowYaw >= 0: the cell has viewports on the wall that a wall piece turned by
    // windowYaw would sit on; pieces there are skipped (their draws still taken).
    void dressCorridor(int x, int y, Vec3 c, float yaw, Mood mood, bool lowPipe, float windowYaw = -1.0f) {
        auto onWindowWall = [&](float pieceYaw) {
            if (windowYaw < 0.0f) return false;
            float d = std::fmod(pieceYaw - windowYaw, 2.0f * kPi);
            if (d < 0.0f) d += 2.0f * kPi;
            return d < 0.1f || d > 2.0f * kPi - 0.1f;
        };
        if (lowPipe) {
            const Mat4 t = at(c, yaw);
            place("pipes_low", t);
            boxCanon(t, -kWalk, 0.62f, kWalk, 1.12f, 1.30f);
            level.lowPipes.push_back({t.point({0, 0, -0.88f}), normalize(t.dir({0, 0, -1})), 1.30f});
        }
        float pSteam = 0, pDebris = 0, pBeacon = 0;
        switch (mood) {
            case Mood::Lit: break;
            case Mood::Dim: pSteam = 0.1f, pDebris = 0.06f; break;
            case Mood::Dark: pSteam = 0.08f, pDebris = 0.12f, pBeacon = 0.3f; break;
            case Mood::Derelict: pSteam = 0.2f, pDebris = 0.3f, pBeacon = 0.04f; break;
        }
        const float side = dress.chance(0.5f) ? kPi : 0.0f;  // which wall
        const float other = side + kPi;
        if (dress.chance(pSteam)) {
            if (onWindowWall(yaw + side)) {
                skipDraws("pipe_burst");
            } else {
                const Mat4 t = at(c, yaw + side);
                place("pipe_burst", t);
                boxCanon(t, 0.9f, 0.0f, kWalk, 0.5f);
                ++level.steamPipes;
            }
        }
        if (dress.chance(pDebris)) {
            if (onWindowWall(yaw + other)) {
                skipDraws("debris");
            } else {
                const Mat4 t = at(c, yaw + other);
                place("debris", t);
                boxCanon(t, 0.5f, -0.95f, kWalk, 0.95f);
                ++level.debrisPiles;
            }
        }
        if (dress.chance(pBeacon)) {
            if (onWindowWall(yaw + other)) {
                skipDraws("beacon");
            } else {
                place("beacon", at(c, yaw + other), -1, true);
                ++level.beacons;
            }
        }
        (void)x, (void)y;
    }

    static Mat4 at(Vec3 p, float yaw) { return Mat4::translation(p) * Mat4::rotationY(yaw); }

    void box(Vec3 c, float x0, float z0, float x1, float z1, int door = -1) {
        level.collision.add({c.x + x0, c.z + z0, c.x + x1, c.z + z1, door});
    }

    // Box in a frame where +u runs along an edge and +n points away from it.
    void boxLocal(Vec3 origin, Vec3 u, Vec3 n, float u0, float n0, float u1, float n1, int door = -1) {
        Vec3 a = origin + u * u0 + n * n0, b = origin + u * u1 + n * n1;
        level.collision.add({std::min(a.x, b.x), std::min(a.z, b.z), std::max(a.x, b.x), std::max(a.z, b.z), door});
    }

    void corridor(int x, int y) {
        int open = 0;
        for (int d = 0; d < 4; ++d)
            if (maze.passable(x, y, Dir(d))) open |= 1 << d;
        Vec3 c = cellCenter(x, y);
        for (const CorridorPiece& p : kCorridors)
            for (int k = 0; k < 4; ++k)
                if (rotateMask(p.mask, k) == open) {
                    float yaw = float(k) * kPi / 2;
                    if (p.mask == (1 | 4)) {
                        // straight: maybe the broken-ceiling variant, then derelict dressing
                        const Mood mood = level.sections.moodAt(x, y);
                        const bool lowPipe = lowPipeCell[size_t(y * maze.width() + x)] != 0;
                        float pBroken = mood == Mood::Derelict ? 0.35f : mood == Mood::Dark ? 0.1f
                                        : mood == Mood::Dim     ? 0.05f : 0.0f;
                        const int out = windowOut[size_t(y * maze.width() + x)];
                        float windowYaw = -1.0f;
                        if (out >= 0) {
                            // viewports: the east wall (canonical) turned to face out; the
                            // dressing stream takes the draws it would have without them
                            if (dress.chance(pBroken)) {
                                if (dress.chance(0.5f)) yaw += kPi;
                                skipDraws("corridor_straight_broken");
                            }
                            int kw = 0;
                            while ((1 + 3 * kw) % 4 != out) ++kw;
                            windowYaw = float(kw) * kPi / 2;
                            place("corridor_straight_window", at(c, windowYaw));
                            level.viewports.push_back({{x, y}, Dir(out)});
                        } else if (!lowPipe && dress.chance(pBroken)) {
                            if (dress.chance(0.5f)) yaw += kPi;  // either end of the cell
                            place("corridor_straight_broken", at(c, yaw));
                            ++level.brokenCeilings;
                        } else {
                            place(p.model, at(c, yaw));
                        }
                        dressCorridor(x, y, c, yaw, mood, lowPipe, windowYaw);
                    } else {
                        place(p.model, at(c, yaw));
                    }
                    goto placed;
                }
    placed:
        // walls: the four corner blocks, plus the arm of every closed side
        for (float sx : {-1.0f, 1.0f})
            for (float sz : {-1.0f, 1.0f})
                box(c, std::min(sx * kWalk, sx * kHalf), std::min(sz * kWalk, sz * kHalf),
                    std::max(sx * kWalk, sx * kHalf), std::max(sz * kWalk, sz * kHalf));
        for (int d = 0; d < 4; ++d) {
            if (open & (1 << d)) continue;
            Vec3 n = dirVector(Dir(d));
            Vec3 u{-n.z, 0, n.x};
            boxLocal(c, u, n, -kWalk, kWalk, kWalk, kHalf);
        }
    }

    void roomCell(int x, int y, Rng& rng) {
        Vec3 c = cellCenter(x, y);
        place("room_floor", at(c, 0));
        place("room_ceiling", at(c, 0));
        for (int d = 0; d < 4; ++d) {
            Edge e = maze.edge(x, y, Dir(d));
            if (e == Edge::Open) continue;
            Vec3 n = dirVector(Dir(d));         // toward the wall
            Vec3 u{-n.z, 0, n.x};
            Vec3 edge = c + n * kHalf;
            float yaw = yawFor(opposite(Dir(d)));
            if (e == Edge::Door) {
                place("room_wall_door", at(c, yaw));
                boxLocal(edge, u, -n, -kHalf, 0.0f, -0.75f, 0.15f);
                boxLocal(edge, u, -n, 0.75f, 0.0f, kHalf, 0.15f);
            } else {
                place(rng.chance(0.5f) ? "room_wall_a" : "room_wall_b", at(c, yaw));
                boxLocal(edge, u, -n, -kHalf, 0.0f, kHalf, 0.15f);
            }
        }
    }

    void roomCorners(const Room& r) {
        struct Corner {
            int x, y;
            float yaw;
            float sx, sz;  // which corner of the cell
        };
        const Corner cs[] = {{r.x, r.y + r.h - 1, 0.0f, -1, 1},
                             {r.x + r.w - 1, r.y + r.h - 1, kPi / 2, 1, 1},
                             {r.x + r.w - 1, r.y, kPi, 1, -1},
                             {r.x, r.y, -kPi / 2, -1, -1}};
        for (const Corner& k : cs) {
            Vec3 c = cellCenter(k.x, k.y);
            place("room_corner", at(c, k.yaw));
            float x0 = k.sx * kHalf, x1 = k.sx * (kHalf - 0.55f);
            float z0 = k.sz * kHalf, z1 = k.sz * (kHalf - 0.55f);
            box(c, std::min(x0, x1), std::min(z0, z1), std::max(x0, x1), std::max(z0, z1));
        }
    }

    void doors() {
        for (int y = 0; y < maze.height(); ++y)
            for (int x = 0; x < maze.width(); ++x)
                for (Dir d : {Dir::East, Dir::South}) {
                    if (maze.edge(x, y, d) != Edge::Door) continue;
                    setCell(x, y);
                    Door door;
                    door.cell = {x, y};
                    door.side = d;
                    door.kind = maze.door(x, y, d);
                    door.center = cellCenter(x, y) + dirVector(d) * kHalf;
                    int index = int(level.doors.size());
                    level.doors.push_back(door);
                    float yaw = (d == Dir::East) ? kPi / 2 : 0.0f;
                    place("door_frame", at(door.center, yaw));
                    const char* leaf = door.kind == DoorKind::Bulkhead ? "door_bulkhead"
                                       : door.kind == DoorKind::Airlock ? "door_airlock"
                                                                        : "door_sliding";
                    place(leaf, at(door.center, yaw), index);
                    // jambs (both sides of the edge) and the closed leaf
                    Vec3 n = dirVector(d);
                    Vec3 u{-n.z, 0, n.x};
                    boxLocal(door.center, u, n, -1.25f, -0.36f, -0.75f, 0.36f);
                    boxLocal(door.center, u, n, 0.75f, -0.36f, 1.25f, 0.36f);
                    boxLocal(door.center, u, n, -0.75f, -0.09f, 0.75f, 0.09f, index);
                }
    }

    void props(Rng& rng) {
        std::vector<char> onRoute(size_t(maze.width() * maze.height()), 0);
        for (Int2 c : level.route) onRoute[size_t(c.y * maze.width() + c.x)] = 1;
        for (const Room& r : maze.rooms()) {
            for (int y = r.y; y < r.y + r.h; ++y)
                for (int x = r.x; x < r.x + r.w; ++x) {
                    if (onRoute[size_t(y * maze.width() + x)]) continue;
                    bool doorCell = false;
                    for (int d = 0; d < 4; ++d) doorCell |= maze.edge(x, y, Dir(d)) == Edge::Door;
                    if (doorCell) continue;
                    std::vector<Dir> walls;
                    for (int d = 0; d < 4; ++d)
                        if (maze.edge(x, y, Dir(d)) == Edge::Wall) walls.push_back(Dir(d));
                    if (walls.empty() || !rng.chance(0.7f)) continue;
                    setCell(x, y);
                    cluster(x, y, walls[rng.below(uint32_t(walls.size()))], rng);
                }
        }
    }

    void prop(const char* name, Vec3 pos, float yaw) {
        int idx = place(name, at(pos, yaw));
        if (idx < 0) return;
        ++level.propCount;
        const KitModel& m = kit.model(idx);
        Mat4 t = at(pos, yaw);
        float x0 = 1e9f, z0 = 1e9f, x1 = -1e9f, z1 = -1e9f;
        for (float bx : {m.boundsMin.x, m.boundsMax.x})
            for (float bz : {m.boundsMin.z, m.boundsMax.z}) {
                Vec3 p = t.point({bx, 0, bz});
                x0 = std::min(x0, p.x), z0 = std::min(z0, p.z), x1 = std::max(x1, p.x), z1 = std::max(z1, p.z);
            }
        level.collision.add({x0, z0, x1, z1});
    }

    void cluster(int x, int y, Dir wall, Rng& rng) {
        Vec3 c = cellCenter(x, y);
        Vec3 n = dirVector(wall);       // toward the wall
        Vec3 u{-n.z, 0, n.x};           // along the wall
        Vec3 base = c + n * kHalf;      // point on the wall
        float along = (rng.unit() - 0.5f) * 1.6f;
        // models' long side is local X; turn it along the wall
        float yawAlong = (wall == Dir::North || wall == Dir::South) ? 0.0f : kPi / 2;
        float jitter = (rng.unit() - 0.5f) * 0.25f;
        auto P = [&](float s, float depth) { return base + u * s - n * depth; };
        switch (rng.below(6)) {
            case 0:
            case 1:
                prop("crate_medium", P(along, 0.35f + 0.42f), yawAlong + jitter * 0.3f);
                prop("crate_small", P(along + 0.05f, 0.35f + 0.42f) + Vec3{0, 0.8f, 0}, yawAlong + jitter);
                if (rng.chance(0.6f)) prop("crate_small", P(along + (along > 0 ? -1.0f : 1.0f), 0.35f + 0.32f),
                                            yawAlong - jitter);
                break;
            case 2: {
                const char* b0 = rng.chance(0.5f) ? "barrel_red" : "barrel_blue";
                prop(b0, P(along - 0.33f, 0.35f + 0.32f), rng.unit() * 6.28f);
                prop(rng.chance(0.5f) ? "barrel_red" : "barrel_blue", P(along + 0.33f, 0.35f + 0.32f), rng.unit() * 6.28f);
                if (rng.chance(0.5f)) prop(b0, P(along, 0.35f + 0.9f), rng.unit() * 6.28f);
                break;
            }
            case 3:
                prop("crate_long", P(along * 0.5f, 0.35f + 0.37f), yawAlong + jitter * 0.2f);
                break;
            case 4:
                prop("canister", P(along, 0.35f + 0.3f), rng.unit() * 6.28f);
                if (rng.chance(0.5f)) prop("canister", P(along + 0.55f, 0.35f + 0.3f), rng.unit() * 6.28f);
                break;
            default:
                // console with its screen facing into the room (model front is +Z)
                prop("console", P(along * 0.5f, 0.35f + 0.55f), yawFor(wall));
                break;
        }
    }

    void lightGrid() {
        level.gridW = maze.width();
        level.gridH = maze.height();
        size_t n = size_t(level.gridW * level.gridH);
        std::vector<std::vector<uint32_t>> buckets(n);
        for (size_t i = 0; i < level.lights.size(); ++i) {
            const Vec3& p = level.lights[i].position;
            int cx = int(std::floor(p.x / kCell + 0.5f)), cy = int(std::floor(p.z / kCell + 0.5f));
            cx = std::clamp(cx, 0, level.gridW - 1);
            cy = std::clamp(cy, 0, level.gridH - 1);
            level.lights[i].cell = cy * level.gridW + cx;
            buckets[size_t(cy * level.gridW + cx)].push_back(uint32_t(i));
        }
        level.cellStart.resize(n);
        level.cellCount.resize(n);
        for (size_t i = 0; i < n; ++i) {
            level.cellStart[i] = uint32_t(level.cellLights.size());
            level.cellCount[i] = uint32_t(buckets[i].size());
            level.cellLights.insert(level.cellLights.end(), buckets[i].begin(), buckets[i].end());
        }
    }

    void setCell(int x, int y) { cell = y * maze.width() + x; }

    const Maze& maze;
    const Kit& kit;
    Level& level;
    int cell = -1;  // cell being built (tags placed models)
    // Derelict dressing has its own random stream, so it never shifts the room walls and
    // props drawn from the level's stream.
    static constexpr uint64_t kDressStream = 0xd3e1c7a5b9f2u;
    Rng dress;
    std::vector<char> lowPipeCell;
    static constexpr uint64_t kWindowStream = 0x5ca7e2f1d04bu;
    Rng windows;
    std::vector<int> windowOut;  // per cell: the Dir the viewports face, -1 none
};

}  // namespace

float ceilingAt(const Maze& maze, Vec3 p) {
    const int cx = int(std::floor(p.x / kCell + 0.5f)), cy = int(std::floor(p.z / kCell + 0.5f));
    switch (maze.kind(cx, cy)) {
        case CellKind::Room: return 3.4f;
        case CellKind::Corridor: return 2.85f;
        case CellKind::Void: return 0.0f;
    }
    return 0.0f;
}

float raycastLevel(const Maze& maze, const Level& level, Vec3 o, Vec3 d, float maxDist,
                   const std::vector<float>& doorOpen, Vec3& normal) {
    auto blocked = [&](Vec3 p, Vec3 prev, Vec3& n) {
        if (p.y <= 0.0f) {
            n = {0, 1, 0};
            return true;
        }
        if (p.y >= ceilingAt(maze, p)) {
            n = {0, -1, 0};
            return true;
        }
        if (const Box2* b = level.collision.blockedAt({p.x, p.z}, p.y, doorOpen)) {
            if (prev.x < b->x0) n = {-1, 0, 0};
            else if (prev.x > b->x1) n = {1, 0, 0};
            else if (prev.z < b->z0) n = {0, 0, -1};
            else if (prev.z > b->z1) n = {0, 0, 1};
            else n = prev.y < b->bottom ? Vec3{0, -1, 0} : Vec3{0, 1, 0};
            return true;
        }
        return false;
    };
    const float step = 0.04f;
    Vec3 prev = o;
    for (float t = step; t <= maxDist; t += step) {
        Vec3 p = o + d * t;
        Vec3 n;
        if (blocked(p, prev, n)) {
            // narrow it down between the last free sample and this one
            float lo = t - step, hi = t;
            for (int i = 0; i < 8; ++i) {
                float mid = 0.5f * (lo + hi);
                Vec3 nm;
                if (blocked(o + d * mid, o + d * lo, nm)) hi = mid;
                else lo = mid;
            }
            normal = n;
            return hi;
        }
        prev = p;
    }
    normal = {0, 1, 0};
    return maxDist;
}

Vec3 doorLeafOffset(const std::string& name, float open) {
    if (name.find("LeafL") != std::string::npos) return {-0.8f * open, 0, 0};
    if (name.find("LeafR") != std::string::npos) return {0.8f * open, 0, 0};
    if (name.find("_Leaf") != std::string::npos) return {0, 2.4f * open, 0};
    return {0, 0, 0};
}

Level buildLevel(const Maze& maze, const Kit& kit, bool viewports) {
    Level level;
    Builder b(maze, kit, level);
    Rng rng(maze.params().seed * 7919u + 17u);
    level.route = maze.findPath(maze.startCell(), maze.exitCell());

    level.sections = buildSections(maze);
    b.chooseLowPipes();
    b.chooseViewports(viewports);

    for (int y = 0; y < maze.height(); ++y)
        for (int x = 0; x < maze.width(); ++x) {
            Vec3 c = cellCenter(x, y);
            b.setCell(x, y);
            switch (maze.kind(x, y)) {
                case CellKind::Void: b.box(c, -kHalf, -kHalf, kHalf, kHalf); break;
                case CellKind::Corridor: b.corridor(x, y); break;
                case CellKind::Room: b.roomCell(x, y, rng); break;
            }
        }
    for (const Room& r : maze.rooms()) {
        b.setCell(r.x, r.y);
        b.roomCorners(r);
    }
    // (the exit airlock's red emergency light comes from its sections power)
    b.doors();
    b.props(rng);
    b.lightGrid();
    level.collision.buildIndex(kCell, maze.width(), maze.height());

    level.spawn = cellCenter(maze.startCell()) + Vec3{0, kEyeHeight, 0};
    if (level.route.size() > 1) {
        Vec3 d = cellCenter(level.route[1]) - cellCenter(level.route[0]);
        level.spawnYaw = std::atan2(-d.x, -d.z);
    }
    level.exitPoint = cellCenter(maze.exitCell());
    return level;
}

}  // namespace bh
