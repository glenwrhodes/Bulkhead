#include "world/Sections.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <numeric>

#include "world/Random.h"

namespace bh {

std::string_view toString(Mood m) {
    switch (m) {
        case Mood::Lit: return "lit";
        case Mood::Dim: return "dim";
        case Mood::Dark: return "dark";
        case Mood::Derelict: return "derelict";
    }
    return "?";
}

std::string_view toString(LightMode m) {
    switch (m) {
        case LightMode::Steady: return "steady";
        case LightMode::Flicker: return "flicker";
        case LightMode::Strobe: return "strobe";
        case LightMode::Dead: return "dead";
        case LightMode::Emergency: return "emergency";
    }
    return "?";
}

namespace {

// Separate random streams: the maze uses the default one, so these never disturb it.
constexpr uint64_t kSectionStream = 0x5ec7105ec7105ull;
constexpr uint64_t kPowerStream = 0x9e3779b97f4a7c15ull;

uint32_t mix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

float hash01(uint32_t a, uint32_t b) { return float(mix32(a * 0x9e3779b1u ^ mix32(b)) >> 8) * (1.0f / 16777216.0f); }

// Integer time step: the same t always gives the same state, in any build.
uint32_t step(float t, float rate) { return uint32_t(int64_t(std::floor(t * rate)) & 0x7fffffff); }

// 0..1 brightness of the failing tube at time t.
float pattern(const CellPower& p, float t) {
    const uint32_t ph = uint32_t(p.phase * 1000003.0f);
    switch (p.mode) {
        case LightMode::Steady: return 1.0f;
        case LightMode::Dead: return 0.0f;
        case LightMode::Flicker: {
            // mostly on; every so often a bad spell where the tube stutters
            float shift = p.phase * 5.0f;
            bool bad = hash01(step(t + shift, 0.7f), ph) > 0.55f;
            if (!bad) return 0.93f + 0.07f * hash01(step(t, 30.0f), ph + 7u);
            return hash01(step(t, 21.0f), ph + 13u) > 0.4f ? 0.85f : 0.04f;
        }
        case LightMode::Strobe: {
            // hard, irregular flashes
            float h = hash01(step(t + p.phase, 7.0f), ph + 29u);
            return h > 0.58f ? 1.0f : 0.02f;
        }
        case LightMode::Emergency: {
            float s = 0.5f + 0.5f * std::sin(2.0f * kPi * (t / 1.8f + p.phase));
            return 0.45f + 0.55f * s * s;
        }
    }
    return 1.0f;
}

}  // namespace

Vec3 lightMultiplier(const CellPower& p, float t) {
    float k = p.level * pattern(p, t);
    return {p.tint.x * k, p.tint.y * k, p.tint.z * k};
}

Vec3 emissiveMultiplier(const CellPower& p, float t) {
    if (p.mode == LightMode::Dead) return {0.0015f, 0.0015f, 0.0015f};
    // panels read a little brighter than the light they cast in dim cells, so a dim
    // corridor still shows where its fixtures are
    float k = std::pow(std::max(p.level, 0.0f), 0.8f) * pattern(p, t);
    return {p.tint.x * k, p.tint.y * k, p.tint.z * k};
}

Sections buildSections(const Maze& maze) {
    Sections out;
    const int W = maze.width(), H = maze.height();
    const size_t N = size_t(W) * size_t(H);
    out.width = W;
    out.height = H;
    out.cellSection.assign(N, -1);
    auto idx = [W](Int2 c) { return size_t(c.y) * size_t(W) + size_t(c.x); };

    std::vector<Int2> walkable;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (maze.kind(x, y) != CellKind::Void) walkable.push_back({x, y});
    if (walkable.empty()) return out;

    Rng rng(maze.params().seed, kSectionStream);

    // --- anchors: spread out by walking distance (farthest-point sampling) ---
    const int target = std::clamp(int(walkable.size()) / 85, 4, 64);
    const int count = std::max(1, std::min(target, int(walkable.size()) / 6));
    std::vector<Int2> anchors{maze.startCell()};
    std::vector<int> minDist = maze.distancesFrom(maze.startCell());
    std::vector<char> roomHasAnchor(maze.rooms().size(), 0);
    if (maze.roomAt(anchors[0].x, anchors[0].y) >= 0) roomHasAnchor[size_t(maze.roomAt(anchors[0].x, anchors[0].y))] = 1;
    while (int(anchors.size()) < count) {
        int best = 0;
        for (Int2 c : walkable) best = std::max(best, minDist[idx(c)]);
        if (best <= 1) break;
        std::vector<Int2> candidates;
        for (Int2 c : walkable) {
            int r = maze.roomAt(c.x, c.y);
            if (r >= 0 && roomHasAnchor[size_t(r)]) continue;
            if (float(minDist[idx(c)]) >= 0.85f * float(best)) candidates.push_back(c);
        }
        if (candidates.empty()) break;
        Int2 pick = candidates[rng.below(uint32_t(candidates.size()))];
        anchors.push_back(pick);
        if (int r = maze.roomAt(pick.x, pick.y); r >= 0) roomHasAnchor[size_t(r)] = 1;
        const std::vector<int> d = maze.distancesFrom(pick);
        for (size_t i = 0; i < N; ++i)
            if (d[i] >= 0 && (minDist[i] < 0 || d[i] < minDist[i])) minDist[i] = d[i];
    }

    // --- grow every section at once; a room joins whole ---
    std::deque<Int2> queue;
    auto claim = [&](Int2 c, int s) {
        int r = maze.roomAt(c.x, c.y);
        if (r < 0) {
            out.cellSection[idx(c)] = s;
            queue.push_back(c);
            return;
        }
        const Room& room = maze.rooms()[size_t(r)];
        for (int y = room.y; y < room.y + room.h; ++y)
            for (int x = room.x; x < room.x + room.w; ++x)
                if (out.cellSection[idx({x, y})] < 0) {
                    out.cellSection[idx({x, y})] = s;
                    queue.push_back({x, y});
                }
    };
    for (size_t s = 0; s < anchors.size(); ++s)
        if (out.cellSection[idx(anchors[s])] < 0) claim(anchors[s], int(s));
    while (!queue.empty()) {
        Int2 c = queue.front();
        queue.pop_front();
        const int s = out.cellSection[idx(c)];
        for (int d = 0; d < 4; ++d) {
            if (!maze.passable(c.x, c.y, Dir(d))) continue;
            Int2 n{c.x + kDx[d], c.y + kDy[d]};
            if (out.cellSection[idx(n)] < 0) claim(n, s);
        }
    }

    // drop sections that ended up empty, keeping the order
    std::vector<int> cellsIn(anchors.size(), 0);
    for (int s : out.cellSection)
        if (s >= 0) ++cellsIn[size_t(s)];
    std::vector<int> remap(anchors.size(), -1);
    for (size_t s = 0; s < anchors.size(); ++s)
        if (cellsIn[s] > 0) {
            remap[s] = int(out.anchor.size());
            out.anchor.push_back(anchors[s]);
        }
    for (int& s : out.cellSection)
        if (s >= 0) s = remap[size_t(s)];
    const int K = int(out.anchor.size());

    const std::vector<int> fromStart = maze.distancesFrom(maze.startCell());
    for (Int2 a : out.anchor) out.depth.push_back(std::max(0, fromStart[idx(a)]));

    // --- moods: darker with depth, with some local shuffling ---
    std::vector<int> order(static_cast<size_t>(K), 0);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return out.depth[size_t(a)] < out.depth[size_t(b)]; });

    std::vector<Mood> ranked;
    {
        int lit = std::max(1, int(std::lround(0.28 * K)));
        int dim = std::max(1, int(std::lround(0.24 * K)));
        int derelict = std::max(1, int(std::lround(0.26 * K)));
        int dark = K - lit - dim - derelict;
        while (dark < 1 && K >= 4) {
            if (lit >= dim && lit >= derelict && lit > 1) --lit;
            else if (derelict >= dim && derelict > 1) --derelict;
            else if (dim > 1) --dim;
            else break;
            dark = K - lit - dim - derelict;
        }
        for (int i = 0; i < lit; ++i) ranked.push_back(Mood::Lit);
        for (int i = 0; i < dim; ++i) ranked.push_back(Mood::Dim);
        for (int i = 0; i < derelict; ++i) ranked.push_back(Mood::Derelict);
        for (int i = 0; i < dark; ++i) ranked.push_back(Mood::Dark);
        ranked.resize(size_t(K), Mood::Lit);
    }
    // jitter the ranks a little (the start section keeps rank 0)
    {
        std::vector<std::pair<float, Mood>> keyed;
        for (int i = 0; i < K; ++i)
            keyed.push_back({i == 0 ? -1.0f : float(i) + rng.unit() * 2.6f, ranked[size_t(i)]});
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (int i = 0; i < K; ++i) ranked[size_t(i)] = keyed[size_t(i)].second;
        ranked[0] = Mood::Lit;
    }
    out.mood.assign(size_t(K), Mood::Lit);
    for (int i = 0; i < K; ++i) out.mood[size_t(order[size_t(i)])] = ranked[size_t(i)];

    // neighbouring sections with the same mood read as one; swap a few apart
    std::vector<std::vector<int>> adj(static_cast<size_t>(K), std::vector<int>{});
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (Dir d : {Dir::East, Dir::South}) {
                if (!maze.passable(x, y, d)) continue;
                int a = out.sectionAt(x, y), b = out.sectionAt(x + kDx[int(d)], y + kDy[int(d)]);
                if (a < 0 || b < 0 || a == b) continue;
                if (std::find(adj[size_t(a)].begin(), adj[size_t(a)].end(), b) == adj[size_t(a)].end()) {
                    adj[size_t(a)].push_back(b);
                    adj[size_t(b)].push_back(a);
                }
            }
    auto conflicts = [&](int s) {
        int n = 0;
        for (int t : adj[size_t(s)]) n += out.mood[size_t(t)] == out.mood[size_t(s)];
        return n;
    };
    const int startSection = out.sectionAt(maze.startCell().x, maze.startCell().y);
    for (int i = 1; i < K; ++i) {
        const int s = order[size_t(i)];
        if (conflicts(s) == 0) continue;
        for (int j = std::max(1, i - 3); j < std::min(K, i + 4); ++j) {
            const int t = order[size_t(j)];
            if (t == s || t == startSection || out.mood[size_t(t)] == out.mood[size_t(s)]) continue;
            const int before = conflicts(s) + conflicts(t);
            std::swap(out.mood[size_t(s)], out.mood[size_t(t)]);
            if (conflicts(s) + conflicts(t) < before) break;
            std::swap(out.mood[size_t(s)], out.mood[size_t(t)]);
        }
    }

    // --- per-cell lights, wear and haze ---
    Rng prng(maze.params().seed, kPowerStream);
    out.power.assign(N, CellPower{});
    out.decay.assign(N, 0.0f);
    out.haze.assign(N, 0.0f);
    const Vec3 warm{1.0f, 0.66f, 0.36f}, sickly{1.0f, 0.93f, 0.7f}, red{1.0f, 0.14f, 0.06f};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = idx({x, y});
            CellPower p;
            p.phase = prng.unit();
            const float roll = prng.unit();
            const float jitter = prng.unit() - 0.5f;
            if (maze.kind(x, y) == CellKind::Void) {
                out.power[i] = p;
                continue;
            }
            const Mood m = out.moodAt(x, y);
            switch (m) {
                case Mood::Lit:
                    if (roll < 0.05f) p.mode = LightMode::Flicker;
                    out.decay[i] = 0.12f + 0.15f * jitter;
                    out.haze[i] = 0.002f;
                    break;
                case Mood::Dim:
                    p.level = 0.17f;
                    p.tint = warm;
                    if (roll < 0.15f) p.mode = LightMode::Flicker;
                    else if (roll < 0.38f) p.mode = LightMode::Dead;
                    out.decay[i] = 0.4f + 0.2f * jitter;
                    out.haze[i] = 0.007f;
                    break;
                case Mood::Dark:
                    if (roll < 0.72f) {
                        p.mode = LightMode::Dead;
                    } else {
                        p.mode = LightMode::Emergency;
                        p.level = 0.5f;
                        p.tint = red;
                    }
                    out.decay[i] = 0.55f + 0.2f * jitter;
                    out.haze[i] = 0.012f;
                    break;
                case Mood::Derelict:
                    p.tint = sickly;
                    if (roll < 0.4f) {
                        p.mode = LightMode::Dead;
                    } else if (roll < 0.66f) {
                        p.mode = LightMode::Flicker;
                        p.level = 0.5f;
                    } else if (roll < 0.76f) {
                        p.mode = LightMode::Strobe;
                        p.level = 0.8f;
                    } else {
                        p.level = 0.3f;
                    }
                    out.decay[i] = 0.92f + 0.15f * jitter;
                    out.haze[i] = 0.016f;
                    break;
            }
            // rooms are big open volumes with a light in every cell: the same density
            // would read as steam there
            if (maze.kind(x, y) == CellKind::Room) out.haze[i] *= 0.35f;
            out.power[i] = p;
        }
    // the spawn room is steady; the exit airlock runs on red emergency light
    auto roomPower = [&](int roomId, CellPower p) {
        if (roomId < 0) return;
        const Room& r = maze.rooms()[size_t(roomId)];
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x) {
                p.phase = out.power[idx({x, y})].phase;
                out.power[idx({x, y})] = p;
            }
    };
    roomPower(maze.startRoom(), CellPower{});
    roomPower(maze.exitRoom(), CellPower{LightMode::Emergency, 1.2f, red, 0.0f});

    // soften the borders between sections, then let void cells copy their neighbours
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<float> d2 = out.decay, h2 = out.haze;
        for (Int2 c : walkable) {
            float sd = 0, sh = 0;
            int n = 0;
            for (int d = 0; d < 4; ++d) {
                if (!maze.passable(c.x, c.y, Dir(d))) continue;
                size_t j = idx({c.x + kDx[d], c.y + kDy[d]});
                sd += out.decay[j];
                sh += out.haze[j];
                ++n;
            }
            if (n == 0) continue;
            d2[idx(c)] = 0.5f * out.decay[idx(c)] + 0.5f * sd / float(n);
            h2[idx(c)] = 0.5f * out.haze[idx(c)] + 0.5f * sh / float(n);
        }
        out.decay.swap(d2);
        out.haze.swap(h2);
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (maze.kind(x, y) != CellKind::Void) continue;
            float sd = 0, sh = 0;
            int n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = x + dx, ny = y + dy;
                    if (!maze.inBounds(nx, ny) || maze.kind(nx, ny) == CellKind::Void) continue;
                    sd += out.decay[idx({nx, ny})];
                    sh += out.haze[idx({nx, ny})];
                    ++n;
                }
            out.decay[idx({x, y})] = n ? sd / float(n) : 0.3f;
            out.haze[idx({x, y})] = n ? sh / float(n) : 0.01f;
        }
    for (float& d : out.decay) d = std::clamp(d, 0.0f, 1.0f);
    return out;
}

}  // namespace bh
