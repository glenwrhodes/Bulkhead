#include "game/Encounters.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <thread>

#include "game/Game.h"

namespace bh {
namespace {

constexpr float kBotSpeed = 3.4f;     // the window's walking speed
constexpr float kSeeRange = 25.0f;
constexpr float kSightingGap = 8.0f;  // out of view this long, then back: a new sighting

Int2 cellAt(Vec3 p) { return {int(std::floor(p.x / kCell + 0.5f)), int(std::floor(p.z / kCell + 0.5f))}; }

// The simulated player: heads for the nearest cell the map has not shown yet.
class Explorer {
public:
    Explorer(const Game& g, uint64_t seed) : game_(g), rng_(seed, 0xe791u) { reset(); }

    void reset() {
        pos_ = game_.level().spawn;
        yaw_ = game_.level().spawnYaw;
        wps_.clear();
        target_ = {-1, -1};
    }

    Camera step(float dt) {
        const Maze& m = game_.maze();
        bool arrived = wps_.empty();
        float remaining = kBotSpeed * dt;
        Vec3 heading{};
        while (remaining > 0.0f && !wps_.empty()) {
            Vec3 d = wps_.front() - pos_;
            d.y = 0.0f;
            const float len = length(d);
            if (len > 1e-4f) heading = d * (1.0f / len);
            if (len <= remaining) {
                pos_ = wps_.front();
                wps_.erase(wps_.begin());
                remaining -= len;
                arrived = true;
                if (needPlan()) plan(m);
            } else {
                pos_ = pos_ + d * (remaining / len);
                remaining = 0.0f;
            }
        }
        if (arrived && needPlan()) plan(m);
        if (length(heading) > 0.5f) {
            const float want = std::atan2(-heading.x, -heading.z);
            float dy = want - yaw_;
            while (dy > kPi) dy -= 2.0f * kPi;
            while (dy < -kPi) dy += 2.0f * kPi;
            yaw_ += dy * (1.0f - std::exp(-7.0f * dt));
        }
        Camera c;
        c.position = pos_;
        c.yaw = yaw_;
        c.pitch = 0.0f;
        return c;
    }

private:
    bool needPlan() const {
        if (wps_.empty()) return true;
        const Maze& m = game_.maze();
        // the goal has been seen: find the next unseen one (at a cell centre)
        return m.inBounds(target_) && game_.explored()[size_t(target_.y * m.width() + target_.x)] != 0;
    }

    void plan(const Maze& m) {
        const Int2 cur = cellAt(pos_);
        if (!m.inBounds(cur)) return;
        const std::vector<int> dist = m.distancesFrom(cur);
        const auto& seen = game_.explored();
        int best = 1 << 30;
        std::vector<Int2> ties;
        for (int y = 0; y < m.height(); ++y)
            for (int x = 0; x < m.width(); ++x) {
                const size_t i = size_t(y * m.width() + x);
                if (m.kind(x, y) == CellKind::Void || seen[i] || dist[i] <= 0) continue;
                if (dist[i] < best) {
                    best = dist[i];
                    ties.clear();
                }
                if (dist[i] == best) ties.push_back({x, y});
            }
        if (ties.empty()) {  // all of it seen: wander somewhere far
            for (int attempt = 0; attempt < 400; ++attempt) {
                Int2 c{rng_.range(0, m.width() - 1), rng_.range(0, m.height() - 1)};
                const size_t i = size_t(c.y * m.width() + c.x);
                if (m.kind(c) != CellKind::Void && dist[i] >= 6) {
                    ties.push_back(c);
                    break;
                }
            }
            if (ties.empty()) return;
        }
        target_ = ties[size_t(rng_.range(0, int(ties.size()) - 1))];
        const std::vector<Int2> path = m.findPath(cur, target_);
        wps_.clear();
        for (size_t k = 1; k < path.size(); ++k) wps_.push_back(cellCenter(path[k]) + Vec3{0, kEyeHeight, 0});
    }

    const Game& game_;
    Rng rng_;
    Vec3 pos_;
    float yaw_ = 0.0f;
    std::vector<Vec3> wps_;
    Int2 target_{-1, -1};
};

bool inView(const Game& g, const Camera& cam, Vec3 p) {
    const Vec3 d = p - cam.position;
    const float dist = length(d);
    if (dist > kSeeRange) return false;
    const float tanV = std::tan(cam.fovY * 0.5f), tanH = tanV * 16.0f / 9.0f;
    const float z = dot(d, cam.forward());
    if (z < 0.1f) return false;
    if (std::abs(dot(d, cam.right())) > tanH * z || std::abs(dot(d, cam.up())) > tanV * z) return false;
    Vec3 n;
    const float t = raycastLevel(g.maze(), g.level(), cam.position, d * (1.0f / dist), dist, g.doorOpen(), n);
    return t >= dist - 0.3f;
}

}  // namespace

EncounterRun runEncounter(const Kit& kit, uint64_t seed, int variant, const EncounterConfig& cfg) {
    MazeParams mp;
    mp.width = cfg.mazeWidth;
    mp.height = cfg.mazeHeight;
    mp.seed = seed;
    Game game(kit, mp);
    game.setHuntTuning(cfg.tuning);
    game.setPlayerNoise(1);  // always walking
    game.spawnHunters(cfg.tuning.creatures, uint64_t(variant));
    Explorer bot(game, seed * 7919u + uint64_t(variant) * 104729u + 1u);

    EncounterRun r;
    r.seed = seed;
    r.variant = variant;
    r.spawned = int(game.hunters().size());
    std::vector<float> lastSeen;
    const int steps = int(std::ceil(cfg.minutes * 60.0f / cfg.dt));
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < steps; ++i) {
        const float t = float(i + 1) * cfg.dt;
        if (game.playerDead()) {
            game.stepWorld(cfg.dt);
            if (game.deathTime() > Game::kDeathLength) {
                game.restart();
                bot.reset();
            }
            continue;
        }
        game.movePlayer(bot.step(cfg.dt));
        game.stepWorld(cfg.dt);
        if (game.playerDead()) {
            ++r.deaths;
            if (r.firstGrab < 0.0f) r.firstGrab = t;
            continue;
        }
        const Camera& cam = game.camera();
        const auto& hs = game.hunters();
        lastSeen.resize(hs.size(), -1e9f);
        int inViewNow = 0;
        for (size_t k = 0; k < hs.size(); ++k) {
            const Hunter& h = hs[k];
            const int surf = h.surfaceKind();
            if (surf < 0) continue;
            const float d = std::min(length(h.headPosition() - cam.position), length(h.bodyCenter() - cam.position));
            if (d <= kSeeRange && r.firstNear < 0.0f) r.firstNear = t;
            if (d > kSeeRange) continue;
            if (!inView(game, cam, h.headPosition()) && !inView(game, cam, h.bodyCenter())) continue;
            ++inViewNow;
            r.viewSeconds[surf] += cfg.dt;
            if (r.firstSeen < 0.0f) {
                r.firstSeen = t;
                r.seenSurface = surf;
                r.seenState = int(h.state());
                r.seenDistance = d;
            }
            if (t - lastSeen[k] > kSightingGap) ++r.sightings;
            lastSeen[k] = t;
        }
        r.maxInView = std::max(r.maxInView, float(inViewNow));
    }
    r.msPerStep = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / steps;
    r.encounter = r.firstSeen;
    if (r.firstGrab >= 0.0f && (r.encounter < 0.0f || r.firstGrab < r.encounter)) r.encounter = r.firstGrab;
    for (uint8_t e : game.explored()) r.explored += e ? 1 : 0;
    for (const Hunter& h : game.hunters()) r.drifts += h.drifts();
    return r;
}

std::vector<EncounterRun> runEncounters(const Kit& kit, const std::vector<uint64_t>& seeds, int variants,
                                        const EncounterConfig& cfg) {
    const size_t n = seeds.size() * size_t(std::max(1, variants));
    std::vector<EncounterRun> out(n);
    std::atomic<size_t> next{0};
    const unsigned workers = std::max(1u, std::min(unsigned(n), std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < workers; ++w)
        pool.emplace_back([&] {
            for (size_t j = next++; j < n; j = next++)
                out[j] = runEncounter(kit, seeds[j / size_t(variants)], int(j % size_t(variants)), cfg);
        });
    for (std::thread& t : pool) t.join();
    return out;
}

std::vector<std::string> summarizeEncounters(const std::vector<EncounterRun>& runs, const EncounterConfig& cfg) {
    std::vector<std::string> L;
    const float cap = cfg.minutes * 60.0f;
    auto stats = [&](const char* label, auto get) {
        std::vector<float> v;
        int none = 0;
        for (const EncounterRun& r : runs) {
            const float x = get(r);
            if (x < 0.0f) ++none;
            v.push_back(x < 0.0f ? cap : x);
        }
        if (v.empty()) return;
        std::sort(v.begin(), v.end());
        float mean = 0.0f;
        for (float x : v) mean += x;
        mean /= float(v.size());
        auto pct = [&](float p) { return v[size_t(p * float(v.size() - 1) + 0.5f)]; };
        L.push_back(std::format("  {:<34} mean {:6.1f} s{}  median {:6.1f}  25% {:6.1f}  75% {:6.1f}  90% {:6.1f}  "
                                "shortest {:5.1f}  longest {:6.1f}  none in {:.0f} min: {}/{}",
                                label, mean, none ? "+" : " ", pct(0.5f), pct(0.25f), pct(0.75f), pct(0.9f), v.front(),
                                v.back(), cfg.minutes, none, v.size()));
    };
    L.push_back(std::format("Creatures {}, drift {} (every {:.0f}-{:.0f} s, within {} cells), hearing walk {:.0f} m "
                            "run {:.0f} m; {} runs of {:.0f} min at {:.0f} steps/s",
                            cfg.tuning.creatures, cfg.tuning.drift ? "on" : "off", cfg.tuning.driftMin,
                            cfg.tuning.driftMax, cfg.tuning.driftSpread, cfg.tuning.hearWalk, cfg.tuning.hearRun,
                            runs.size(), cfg.minutes, 1.0f / cfg.dt));
    stats("first encounter (seen or killed)", [](const EncounterRun& r) { return r.encounter; });
    stats("first seen", [](const EncounterRun& r) { return r.firstSeen; });
    stats("first within 25 m (scanner range)", [](const EncounterRun& r) { return r.firstNear; });
    stats("first death", [](const EncounterRun& r) { return r.firstGrab; });
    int ambush = 0, surf[4] = {0, 0, 0, 0}, seenN = 0;
    float sightings = 0, deaths = 0, view[4] = {0, 0, 0, 0}, maxView = 0, explored = 0, drifts = 0, spawned = 0;
    int minSpawned = 1 << 30;
    double ms = 0;
    for (const EncounterRun& r : runs) {
        if (r.firstGrab >= 0.0f && (r.firstSeen < 0.0f || r.firstGrab < r.firstSeen)) ++ambush;
        if (r.seenSurface >= 0 && r.seenSurface < 4) {
            ++surf[r.seenSurface];
            ++seenN;
        }
        sightings += float(r.sightings);
        deaths += float(r.deaths);
        for (int k = 0; k < 4; ++k) view[k] += r.viewSeconds[k];
        maxView = std::max(maxView, r.maxInView);
        explored += float(r.explored);
        drifts += float(r.drifts);
        minSpawned = std::min(minSpawned, r.spawned);
        spawned += float(r.spawned);
        ms += r.msPerStep;
    }
    const float n = float(std::max<size_t>(1, runs.size()));
    const float per10 = 10.0f / cfg.minutes;
    const float viewAll = std::max(1e-3f, view[0] + view[1] + view[2] + view[3]);
    L.push_back(std::format("  creatures that found a place at the start: {:.1f} on average, {} at least", spawned / n,
                            minSpawned));
    L.push_back(std::format("  killed before ever seeing one: {}/{} runs", ambush, runs.size()));
    L.push_back(std::format("  first seen on: floor {}, wall {}, ceiling {}, in the air {} (of {})", surf[0], surf[1],
                            surf[2], surf[3], seenN));
    L.push_back(std::format("  per 10 minutes: {:.1f} sightings, {:.1f} deaths; creatures in view {:.1f} s "
                            "(floor {:.0f}%, wall {:.0f}%, ceiling {:.0f}%, air {:.0f}%); at most {:.0f} in view at once",
                            sightings / n * per10, deaths / n * per10, viewAll / n * per10, 100 * view[0] / viewAll,
                            100 * view[1] / viewAll, 100 * view[2] / viewAll, 100 * view[3] / viewAll, maxView));
    L.push_back(std::format("  cells the map showed after {:.0f} min: {:.0f} on average; drift legs started {:.1f} a run (since the last death); "
                            "CPU {:.3f} ms a step (with every run going at once)",
                            cfg.minutes, explored / n, drifts / n, ms / n));
    return L;
}

}  // namespace bh
