#include "world/Maze.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <format>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <tuple>

#include "world/Random.h"

namespace bh {

std::string_view toString(RoomKind k) {
    switch (k) {
        case RoomKind::Spawn: return "Spawn";
        case RoomKind::Airlock: return "Airlock";
        case RoomKind::Storage: return "Storage";
        case RoomKind::Engineering: return "Engineering";
        case RoomKind::Crew: return "Crew";
        case RoomKind::Medbay: return "Medbay";
        case RoomKind::Reactor: return "Reactor";
    }
    return "?";
}

std::string_view toString(DoorKind k) {
    switch (k) {
        case DoorKind::None: return "None";
        case DoorKind::Sliding: return "Sliding";
        case DoorKind::Bulkhead: return "Bulkhead";
        case DoorKind::Airlock: return "Airlock";
    }
    return "?";
}

namespace {

constexpr int kMinMapSize = 6;
constexpr int kMaxMapSize = 128;

struct UnionFind {
    std::vector<int> parent;
    explicit UnionFind(int n) : parent(size_t(n)) { std::iota(parent.begin(), parent.end(), 0); }
    int find(int a) {
        while (parent[size_t(a)] != a) {
            parent[size_t(a)] = parent[size_t(parent[size_t(a)])];
            a = parent[size_t(a)];
        }
        return a;
    }
    bool unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a == b) return false;
        parent[size_t(b)] = a;
        return true;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Construction and accessors
// ---------------------------------------------------------------------------

Maze::Maze(const MazeParams& p)
    : params_(p),
      width_(p.width),
      height_(p.height),
      cells_(size_t(p.width) * size_t(p.height), CellKind::Void),
      roomId_(size_t(p.width) * size_t(p.height), -1),
      east_(size_t(p.width) * size_t(p.height)),
      south_(size_t(p.width) * size_t(p.height)) {}

CellKind Maze::kind(int x, int y) const { return inBounds(x, y) ? cells_[index(x, y)] : CellKind::Void; }

int Maze::roomAt(int x, int y) const { return inBounds(x, y) ? roomId_[index(x, y)] : -1; }

const Maze::EdgeData* Maze::edgePtr(int x, int y, Dir d) const {
    int nx = x + kDx[int(d)], ny = y + kDy[int(d)];
    if (!inBounds(x, y) || !inBounds(nx, ny)) return nullptr;
    switch (d) {
        case Dir::East: return &east_[index(x, y)];
        case Dir::South: return &south_[index(x, y)];
        case Dir::West: return &east_[index(nx, ny)];
        case Dir::North: return &south_[index(nx, ny)];
    }
    return nullptr;
}

void Maze::setEdge(int x, int y, Dir d, Edge type, DoorKind door) {
    auto* e = const_cast<EdgeData*>(edgePtr(x, y, d));
    if (!e) throw std::logic_error("Maze::setEdge on the map border");
    e->type = type;
    e->door = (type == Edge::Door) ? door : DoorKind::None;
}

Edge Maze::edge(int x, int y, Dir d) const {
    const EdgeData* e = edgePtr(x, y, d);
    return e ? e->type : Edge::Wall;
}

DoorKind Maze::door(int x, int y, Dir d) const {
    const EdgeData* e = edgePtr(x, y, d);
    return e ? e->door : DoorKind::None;
}

int Maze::passableCount(int x, int y) const {
    int n = 0;
    for (int d = 0; d < 4; ++d) n += passable(x, y, Dir(d)) ? 1 : 0;
    return n;
}

bool Maze::isDeadEnd(int x, int y) const { return kind(x, y) == CellKind::Corridor && passableCount(x, y) == 1; }

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

Maze Maze::generate(const MazeParams& p) {
    if (p.width < kMinMapSize || p.height < kMinMapSize || p.width > kMaxMapSize || p.height > kMaxMapSize)
        throw std::invalid_argument(std::format("Maze size {}x{} is outside {}..{}", p.width, p.height,
                                                kMinMapSize, kMaxMapSize));
    if (p.minRoomSize < 1 || p.maxRoomSize < p.minRoomSize)
        throw std::invalid_argument("Maze room sizes must satisfy 1 <= min <= max");

    Maze m(p);
    Rng rng(p.seed);
    m.placeRooms(rng);
    m.carveCorridors(rng);
    m.connectRegions(rng);
    m.braidAndPrune(rng);
    m.chooseStartAndExit(rng);
    m.assignKinds(rng);
    return m;
}

void Maze::addRoom(int x, int y, int w, int h) {
    Room r;
    r.id = int(rooms_.size());
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    rooms_.push_back(r);
    for (int cy = y; cy < y + h; ++cy)
        for (int cx = x; cx < x + w; ++cx) {
            setKind(cx, cy, CellKind::Room);
            roomId_[index(cx, cy)] = r.id;
            if (cx + 1 < x + w) setEdge(cx, cy, Dir::East, Edge::Open);
            if (cy + 1 < y + h) setEdge(cx, cy, Dir::South, Edge::Open);
        }
}

// Rooms are scattered first. Rooms keep at least one cell between them so every
// room is reached through a corridor and a door.
void Maze::placeRooms(Rng& rng) {
    const MazeParams& p = params_;
    const int area = width_ * height_;
    const int attempts = p.roomAttempts > 0 ? p.roomAttempts : std::max(60, area);
    const int targetCells = int(float(area) * p.roomCoverage);

    auto fits = [&](int x, int y, int w, int h) {
        for (const Room& r : rooms_)
            if (x < r.x + r.w + 1 && r.x < x + w + 1 && y < r.y + r.h + 1 && r.y < y + h + 1) return false;
        return true;
    };

    int roomCells = 0;
    for (int i = 0; i < attempts && roomCells < targetCells; ++i) {
        int w = rng.range(p.minRoomSize, p.maxRoomSize);
        int h = rng.range(p.minRoomSize, p.maxRoomSize);
        if (rng.chance(0.25f)) {
            if (rng.chance(0.5f)) ++w;
            else ++h;
        }
        w = std::min(w, width_ - 2);
        h = std::min(h, height_ - 2);
        int x = rng.range(0, width_ - w);
        int y = rng.range(0, height_ - h);
        if (!fits(x, y, w, h)) continue;
        addRoom(x, y, w, h);
        roomCells += w * h;
    }

    if (rooms_.size() < 2) {
        // Guarantee a start and an exit: two small rooms in opposite corners.
        rooms_.clear();
        std::fill(cells_.begin(), cells_.end(), CellKind::Void);
        std::fill(roomId_.begin(), roomId_.end(), -1);
        std::fill(east_.begin(), east_.end(), EdgeData{});
        std::fill(south_.begin(), south_.end(), EdgeData{});
        addRoom(0, 0, 2, 2);
        addRoom(width_ - 2, height_ - 2, 2, 2);
    }
}

// Fills every remaining cell with corridors using a depth-first carver that prefers
// to keep going straight (ship corridors are long runs, not twisty caves).
void Maze::carveCorridors(Rng& rng) {
    struct Step {
        Int2 cell;
        int cameFrom;  // direction moved to enter this cell, -1 for the seed cell
    };
    std::vector<Step> stack;
    for (int sy = 0; sy < height_; ++sy)
        for (int sx = 0; sx < width_; ++sx) {
            if (kind(sx, sy) != CellKind::Void) continue;
            setKind(sx, sy, CellKind::Corridor);
            stack.push_back({{sx, sy}, -1});
            while (!stack.empty()) {
                const Step top = stack.back();
                int options[4];
                int n = 0;
                for (int d = 0; d < 4; ++d) {
                    int nx = top.cell.x + kDx[d], ny = top.cell.y + kDy[d];
                    if (inBounds(nx, ny) && kind(nx, ny) == CellKind::Void) options[n++] = d;
                }
                if (n == 0) {
                    stack.pop_back();
                    continue;
                }
                int dir = options[rng.below(uint32_t(n))];
                if (top.cameFrom >= 0 && rng.chance(params_.straightness))
                    for (int i = 0; i < n; ++i)
                        if (options[i] == top.cameFrom) dir = top.cameFrom;
                int nx = top.cell.x + kDx[dir], ny = top.cell.y + kDy[dir];
                setEdge(top.cell.x, top.cell.y, Dir(dir), Edge::Open);
                setKind(nx, ny, CellKind::Corridor);
                stack.push_back({{nx, ny}, dir});
            }
        }
}

// Rooms and separate corridor networks are "regions". A random spanning tree of
// connections joins them all (doors where a room is involved); then each room may
// get extra doors, which creates loops so the creatures can flank the player.
void Maze::connectRegions(Rng& rng) {
    const int roomCount = int(rooms_.size());
    std::vector<int> region(cells_.size(), -1);
    for (size_t i = 0; i < cells_.size(); ++i)
        if (cells_[i] == CellKind::Room) region[i] = roomId_[i];

    int regionCount = roomCount;
    std::vector<Int2> flood;
    for (int y = 0; y < height_; ++y)
        for (int x = 0; x < width_; ++x) {
            if (kind(x, y) != CellKind::Corridor || region[index(x, y)] >= 0) continue;
            const int id = regionCount++;
            region[index(x, y)] = id;
            flood.assign(1, {x, y});
            while (!flood.empty()) {
                Int2 c = flood.back();
                flood.pop_back();
                for (int d = 0; d < 4; ++d) {
                    if (edge(c.x, c.y, Dir(d)) != Edge::Open) continue;
                    Int2 n{c.x + kDx[d], c.y + kDy[d]};
                    if (region[index(n.x, n.y)] < 0) {
                        region[index(n.x, n.y)] = id;
                        flood.push_back(n);
                    }
                }
            }
        }

    struct Connector {
        int x, y;
        Dir dir;
        int a, b;  // regions on each side
    };
    std::vector<Connector> connectors;
    for (int y = 0; y < height_; ++y)
        for (int x = 0; x < width_; ++x)
            for (Dir d : {Dir::East, Dir::South}) {
                int nx = x + kDx[int(d)], ny = y + kDy[int(d)];
                if (!inBounds(nx, ny)) continue;
                int a = region[index(x, y)], b = region[index(nx, ny)];
                if (a < 0 || b < 0 || a == b || edge(x, y, d) != Edge::Wall) continue;
                connectors.push_back({x, y, d, a, b});
            }
    rng.shuffle(connectors);

    auto open = [&](const Connector& c) {
        bool touchesRoom = c.a < roomCount || c.b < roomCount;
        setEdge(c.x, c.y, c.dir, touchesRoom ? Edge::Door : Edge::Open, DoorKind::Sliding);
    };

    UnionFind uf(regionCount);
    std::vector<char> used(connectors.size(), 0);
    for (size_t i = 0; i < connectors.size(); ++i)
        if (uf.unite(connectors[i].a, connectors[i].b)) {
            open(connectors[i]);
            used[i] = 1;
        }

    // Two doors side by side on the same wall look wrong; keep them apart.
    auto nextToDoor = [&](const Connector& c) {
        if (c.dir == Dir::East)
            return edge(c.x, c.y - 1, Dir::East) == Edge::Door || edge(c.x, c.y + 1, Dir::East) == Edge::Door;
        return edge(c.x - 1, c.y, Dir::South) == Edge::Door || edge(c.x + 1, c.y, Dir::South) == Edge::Door;
    };

    for (int r = 0; r < roomCount; ++r) {
        int extra = (rng.chance(params_.extraDoorChance) ? 1 : 0) +
                    (rng.chance(params_.extraDoorChance * 0.5f) ? 1 : 0);
        if (extra == 0) continue;
        std::vector<size_t> candidates;
        for (size_t i = 0; i < connectors.size(); ++i)
            if (!used[i] && (connectors[i].a == r || connectors[i].b == r)) candidates.push_back(i);
        rng.shuffle(candidates);
        for (size_t i : candidates) {
            if (extra == 0) break;
            if (nextToDoor(connectors[i])) continue;
            open(connectors[i]);
            used[i] = 1;
            --extra;
        }
    }
}

// Dead ends: some are joined to a neighbouring corridor (more loops), then some of the
// rest are retracted back to the nearest junction. The survivors become closets and
// side passages (good spots for weapon pickups later).
void Maze::braidAndPrune(Rng& rng) {
    auto collectDeadEnds = [&] {
        std::vector<Int2> out;
        for (int y = 0; y < height_; ++y)
            for (int x = 0; x < width_; ++x)
                if (isDeadEnd(x, y)) out.push_back({x, y});
        rng.shuffle(out);
        return out;
    };

    for (Int2 c : collectDeadEnds()) {
        if (!isDeadEnd(c.x, c.y) || !rng.chance(params_.braidChance)) continue;
        std::vector<int> options, preferred;
        for (int d = 0; d < 4; ++d) {
            int nx = c.x + kDx[d], ny = c.y + kDy[d];
            if (edge(c.x, c.y, Dir(d)) != Edge::Wall || kind(nx, ny) != CellKind::Corridor) continue;
            if (!inBounds(nx, ny)) continue;
            options.push_back(d);
            if (isDeadEnd(nx, ny)) preferred.push_back(d);
        }
        const std::vector<int>& pick = preferred.empty() ? options : preferred;
        if (pick.empty()) continue;
        setEdge(c.x, c.y, Dir(pick[rng.below(uint32_t(pick.size()))]), Edge::Open);
    }

    for (Int2 c : collectDeadEnds()) {
        if (!isDeadEnd(c.x, c.y) || !rng.chance(params_.pruneChance)) continue;
        Int2 cur = c;
        while (isDeadEnd(cur.x, cur.y)) {
            int d = 0;
            while (!passable(cur.x, cur.y, Dir(d))) ++d;
            Int2 next{cur.x + kDx[d], cur.y + kDy[d]};
            setEdge(cur.x, cur.y, Dir(d), Edge::Wall);
            setKind(cur.x, cur.y, CellKind::Void);
            cur = next;
        }
    }
}

// Start: the room nearest a random corner. Exit: the room farthest away by walking distance.
void Maze::chooseStartAndExit(Rng& rng) {
    const uint32_t corner = rng.below(4);
    const Int2 target{(corner & 1u) ? width_ - 1 : 0, (corner & 2u) ? height_ - 1 : 0};
    int best = INT_MAX;
    for (const Room& r : rooms_) {
        Int2 c = r.center();
        int d = std::abs(c.x - target.x) + std::abs(c.y - target.y);
        if (d < best) {
            best = d;
            startRoom_ = r.id;
        }
    }

    const std::vector<int> dist = distancesFrom(rooms_[size_t(startRoom_)].center());
    int farthest = -1;
    for (const Room& r : rooms_) {
        if (r.id == startRoom_) continue;
        int d = dist[index(r.center().x, r.center().y)];
        if (d > farthest) {
            farthest = d;
            exitRoom_ = r.id;
        }
    }
}

void Maze::assignKinds(Rng& rng) {
    std::vector<RoomKind> bag = {RoomKind::Storage, RoomKind::Engineering, RoomKind::Crew, RoomKind::Medbay,
                                 RoomKind::Reactor};
    rng.shuffle(bag);
    size_t next = 0;
    for (Room& r : rooms_) {
        if (r.id == startRoom_) r.kind = RoomKind::Spawn;
        else if (r.id == exitRoom_) r.kind = RoomKind::Airlock;
        else r.kind = bag[next++ % bag.size()];
    }

    for (int y = 0; y < height_; ++y)
        for (int x = 0; x < width_; ++x)
            for (Dir d : {Dir::East, Dir::South}) {
                if (edge(x, y, d) != Edge::Door) continue;
                int a = roomAt(x, y), b = roomAt(x + kDx[int(d)], y + kDy[int(d)]);
                DoorKind k = DoorKind::Sliding;
                if (a == exitRoom_ || b == exitRoom_) k = DoorKind::Airlock;
                else if (rng.chance(params_.bulkheadDoorChance)) k = DoorKind::Bulkhead;
                setEdge(x, y, d, Edge::Door, k);
            }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::vector<int> Maze::distancesFrom(Int2 from) const {
    std::vector<int> dist(cells_.size(), -1);
    if (!inBounds(from) || kind(from) == CellKind::Void) return dist;
    std::queue<Int2> q;
    dist[index(from.x, from.y)] = 0;
    q.push(from);
    while (!q.empty()) {
        Int2 c = q.front();
        q.pop();
        const int dc = dist[index(c.x, c.y)];
        for (int d = 0; d < 4; ++d) {
            if (!passable(c.x, c.y, Dir(d))) continue;
            Int2 n{c.x + kDx[d], c.y + kDy[d]};
            if (dist[index(n.x, n.y)] >= 0) continue;
            dist[index(n.x, n.y)] = dc + 1;
            q.push(n);
        }
    }
    return dist;
}

std::vector<Int2> Maze::findPath(Int2 from, Int2 to) const {
    if (!inBounds(from) || !inBounds(to) || kind(from) == CellKind::Void || kind(to) == CellKind::Void) return {};
    const int start = int(index(from.x, from.y));
    const int goal = int(index(to.x, to.y));
    auto heuristic = [&](int i) { return std::abs(i % width_ - to.x) + std::abs(i / width_ - to.y); };

    std::vector<int> g(cells_.size(), INT_MAX);
    std::vector<int> parent(cells_.size(), -1);
    using Node = std::tuple<int, int, int>;  // f, g, cell index
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    g[size_t(start)] = 0;
    open.push({heuristic(start), 0, start});
    while (!open.empty()) {
        auto [f, gc, i] = open.top();
        open.pop();
        (void)f;
        if (gc != g[size_t(i)]) continue;
        if (i == goal) break;
        const int x = i % width_, y = i / width_;
        for (int d = 0; d < 4; ++d) {
            if (!passable(x, y, Dir(d))) continue;
            const int ni = int(index(x + kDx[d], y + kDy[d]));
            const int ng = gc + 1;
            if (ng < g[size_t(ni)]) {
                g[size_t(ni)] = ng;
                parent[size_t(ni)] = i;
                open.push({ng + heuristic(ni), ng, ni});
            }
        }
    }
    if (g[size_t(goal)] == INT_MAX) return {};

    std::vector<Int2> path;
    for (int i = goal; i >= 0; i = parent[size_t(i)]) path.push_back({i % width_, i / width_});
    std::reverse(path.begin(), path.end());
    return path;
}

int Maze::countCells(CellKind k) const { return int(std::count(cells_.begin(), cells_.end(), k)); }

int Maze::countDoors() const {
    int n = 0;
    for (size_t i = 0; i < cells_.size(); ++i)
        n += (east_[i].type == Edge::Door ? 1 : 0) + (south_[i].type == Edge::Door ? 1 : 0);
    return n;
}

int Maze::countDeadEnds() const {
    int n = 0;
    for (int y = 0; y < height_; ++y)
        for (int x = 0; x < width_; ++x) n += isDeadEnd(x, y) ? 1 : 0;
    return n;
}

int Maze::countLoops() const {
    int edges = 0;
    for (size_t i = 0; i < cells_.size(); ++i)
        edges += (east_[i].type != Edge::Wall ? 1 : 0) + (south_[i].type != Edge::Wall ? 1 : 0);
    const int walkable = int(cells_.size()) - countCells(CellKind::Void);
    // Room interiors are fully open grids; count only loops beyond them.
    int roomInternal = 0;
    for (const Room& r : rooms_) roomInternal += (r.w - 1) * (r.h - 1);
    return edges - walkable + 1 - roomInternal;
}

uint64_t Maze::hash() const {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xffu;
            h *= 1099511628211ULL;
        }
    };
    mix(uint64_t(width_));
    mix(uint64_t(height_));
    for (size_t i = 0; i < cells_.size(); ++i) {
        mix(uint64_t(cells_[i]));
        mix(uint64_t(int64_t(roomId_[i])));
        mix(uint64_t(east_[i].type) | (uint64_t(east_[i].door) << 8));
        mix(uint64_t(south_[i].type) | (uint64_t(south_[i].door) << 8));
    }
    for (const Room& r : rooms_) mix(uint64_t(r.kind));
    mix(uint64_t(startRoom_));
    mix(uint64_t(exitRoom_));
    return h;
}

std::string Maze::toAscii() const {
    const int cols = width_ * 2 + 1, rows = height_ * 2 + 1;
    std::vector<std::string> grid(size_t(rows), std::string(size_t(cols), '#'));
    auto doorChar = [](DoorKind k) { return k == DoorKind::Bulkhead ? 'B' : k == DoorKind::Airlock ? 'A' : 'D'; };
    for (int y = 0; y < height_; ++y)
        for (int x = 0; x < width_; ++x) {
            CellKind k = kind(x, y);
            if (k == CellKind::Void) continue;
            char floor = k == CellKind::Room ? '.' : ' ';
            grid[size_t(2 * y + 1)][size_t(2 * x + 1)] = floor;
            if (edge(x, y, Dir::East) == Edge::Open) grid[size_t(2 * y + 1)][size_t(2 * x + 2)] = floor;
            if (edge(x, y, Dir::South) == Edge::Open) grid[size_t(2 * y + 2)][size_t(2 * x + 1)] = floor;
            if (edge(x, y, Dir::East) == Edge::Door) grid[size_t(2 * y + 1)][size_t(2 * x + 2)] = doorChar(door(x, y, Dir::East));
            if (edge(x, y, Dir::South) == Edge::Door) grid[size_t(2 * y + 2)][size_t(2 * x + 1)] = doorChar(door(x, y, Dir::South));
            int r = roomAt(x, y);
            if (r >= 0 && roomAt(x + 1, y) == r && roomAt(x, y + 1) == r && roomAt(x + 1, y + 1) == r)
                grid[size_t(2 * y + 2)][size_t(2 * x + 2)] = '.';
        }
    Int2 s = startCell(), e = exitCell();
    grid[size_t(2 * s.y + 1)][size_t(2 * s.x + 1)] = 'S';
    grid[size_t(2 * e.y + 1)][size_t(2 * e.x + 1)] = 'E';
    std::string out;
    for (const std::string& row : grid) {
        out += row;
        out += '\n';
    }
    return out;
}

std::string Maze::describe() const {
    const size_t pathCells = findPath(startCell(), exitCell()).size();
    return std::format("{}x{} seed {}: {} rooms, {} corridor cells, {} void cells, {} doors, {} dead ends, "
                       "{} loops, start->exit route {} cells",
                       width_, height_, params_.seed, rooms_.size(), countCells(CellKind::Corridor),
                       countCells(CellKind::Void), countDoors(), countDeadEnds(), countLoops(), pathCells);
}

}  // namespace bh
