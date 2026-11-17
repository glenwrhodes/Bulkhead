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
    Builder(const Maze& m, const Kit& k, Level& l) : maze(m), kit(k), level(l) {}

    int place(const char* name, Mat4 t, int door = -1) {
        int idx = kit.find(name);
        if (idx < 0) {
            if (std::find(level.missingModels.begin(), level.missingModels.end(), name) == level.missingModels.end())
                level.missingModels.push_back(name);
            return -1;
        }
        level.placed.push_back({idx, t, door});
        for (const KitLight& l : kit.model(idx).lights) {
            WorldLight w;
            w.position = t.point(l.position);
            w.radiance = l.color * l.intensity;
            w.radius = l.radius;
            level.lights.push_back(w);
        }
        return idx;
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
                    place(p.model, at(c, k * kPi / 2));
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

    const Maze& maze;
    const Kit& kit;
    Level& level;
};

}  // namespace

Vec3 doorLeafOffset(const std::string& name, float open) {
    if (name.find("LeafL") != std::string::npos) return {-0.8f * open, 0, 0};
    if (name.find("LeafR") != std::string::npos) return {0.8f * open, 0, 0};
    if (name.find("_Leaf") != std::string::npos) return {0, 2.4f * open, 0};
    return {0, 0, 0};
}

Level buildLevel(const Maze& maze, const Kit& kit) {
    Level level;
    Builder b(maze, kit, level);
    Rng rng(maze.params().seed * 7919u + 17u);
    level.route = maze.findPath(maze.startCell(), maze.exitCell());

    for (int y = 0; y < maze.height(); ++y)
        for (int x = 0; x < maze.width(); ++x) {
            Vec3 c = cellCenter(x, y);
            switch (maze.kind(x, y)) {
                case CellKind::Void: b.box(c, -kHalf, -kHalf, kHalf, kHalf); break;
                case CellKind::Corridor: b.corridor(x, y); break;
                case CellKind::Room: b.roomCell(x, y, rng); break;
            }
        }
    for (const Room& r : maze.rooms()) b.roomCorners(r);
    // the exit airlock runs on red emergency lighting
    if (maze.exitRoom() >= 0) {
        const Room& exitRoom = maze.rooms()[size_t(maze.exitRoom())];
        for (WorldLight& l : level.lights) {
            int cx = int(std::floor(l.position.x / kCell + 0.5f)), cy = int(std::floor(l.position.z / kCell + 0.5f));
            if (!exitRoom.contains(cx, cy)) continue;
            float power = l.radiance.x + l.radiance.y + l.radiance.z;
            l.radiance = Vec3{0.85f, 0.12f, 0.06f} * power * 0.6f;
        }
    }
    b.doors();
    b.props(rng);
    b.lightGrid();

    level.spawn = cellCenter(maze.startCell()) + Vec3{0, kEyeHeight, 0};
    if (level.route.size() > 1) {
        Vec3 d = cellCenter(level.route[1]) - cellCenter(level.route[0]);
        level.spawnYaw = std::atan2(-d.x, -d.z);
    }
    level.exitPoint = cellCenter(maze.exitCell());
    return level;
}

}  // namespace bh
