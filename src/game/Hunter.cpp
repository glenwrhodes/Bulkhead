#include "game/Hunter.h"

#include <algorithm>
#include <cmath>

namespace bh {

const char* toString(HunterState s) {
    switch (s) {
        case HunterState::Roam: return "roam";
        case HunterState::Stalk: return "stalk";
        case HunterState::Chase: return "chase";
        case HunterState::Leap: return "leap";
        case HunterState::Feed: return "feed";
        case HunterState::Dying: return "dying";
        case HunterState::Dead: return "dead";
        case HunterState::Threat: return "threat";
        case HunterState::Coil: return "coil";
    }
    return "?";
}

namespace {

float smooth(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

Int2 cellOf(Vec3 p) { return {int(std::floor(p.x / kCell + 0.5f)), int(std::floor(p.z / kCell + 0.5f))}; }

bool straightCell(const Maze& m, Int2 c) {
    if (m.kind(c) != CellKind::Corridor) return false;
    const bool n = m.passable(c.x, c.y, Dir::North), s = m.passable(c.x, c.y, Dir::South);
    const bool e = m.passable(c.x, c.y, Dir::East), w = m.passable(c.x, c.y, Dir::West);
    return (n && s && !e && !w) || (e && w && !n && !s);
}

// A corridor closet: the only way out is back. Round 11's goals avoid them (turning round
// takes it most of a second, and a closet is a dull place to send it).
bool deadEnd(const Maze& m, Int2 c) { return m.kind(c) == CellKind::Corridor && m.passableCount(c.x, c.y) <= 1; }

uint32_t cellHash(Int2 c, uint32_t seed) {
    uint32_t h = uint32_t(c.x) * 73856093u ^ uint32_t(c.y) * 19349663u ^ seed * 83492791u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

}  // namespace

Hunter::Hunter(const Kit& kit, uint64_t seed)
    : creature_(kit), rng_(seed, 0x4a17e5u), surfaceSeed_(uint32_t(seed * 2654435761u) ^ uint32_t(seed >> 32)) {}

Int2 Hunter::cell() const { return cellOf(creature_.bodyCenter()); }

float Hunter::uForSurface(int surface) const {
    const CrawlProfile& pr = path_.profile();
    switch (surface) {
        case 0: return 0.0f;
        case 1: return pr.rightWallU();
        case 3: return pr.leftWallU();
        default: return pr.ceilingU();
    }
}

float Hunter::uOf(float s) const {
    if (cellS_.empty()) return 0.0f;
    if (s <= cellS_.front()) return cellU_.front();
    if (s >= cellS_.back()) return cellU_.back();
    const size_t i = size_t(std::upper_bound(cellS_.begin(), cellS_.end(), s) - cellS_.begin());
    const float a = cellU_[i - 1], b = cellU_[i];
    const float total = path_.profile().length();
    float d = b - a;
    if (d > total * 0.5f + 1e-3f) d -= total;
    if (d < -total * 0.5f - 1e-3f) d += total;
    const float t = (s - cellS_[i - 1]) / std::max(1e-4f, cellS_[i] - cellS_[i - 1]);
    return a + d * smooth(0.12f, 0.88f, t);
}

float Hunter::surfaceU() const { return uOf(s_); }

int Hunter::surfaceKind() const {
    if (state_ == HunterState::Leap) return 3;
    if (!alive() || state_ == HunterState::Feed) return -1;
    const CrawlProfile& pr = path_.profile();
    const float total = pr.length();
    float u = std::fmod(surfaceU(), total);
    if (u < 0.0f) u += total;
    auto near = [&](float a) {
        float d = std::abs(u - a);
        return std::min(d, total - d);
    };
    const float floorD = near(0.0f), ceilD = near(pr.ceilingU());
    const float wallD = std::min(near(pr.rightWallU()), near(pr.leftWallU()));
    if (floorD <= wallD && floorD <= ceilD) return 0;
    return ceilD <= wallD ? 2 : 1;
}

bool Hunter::route(const Maze& maze, Int2 goal, bool allowReverse) {
    // where it is now on the old path
    size_t i = 0;
    if (!cellS_.empty()) {
        float best = 1e9f;
        for (size_t k = 0; k < cellS_.size(); ++k)
            if (std::abs(cellS_[k] - s_) < best) {
                best = std::abs(cellS_[k] - s_);
                i = k;
            }
    }
    const Int2 cur = cells_.empty() ? goal : cells_[i];
    std::vector<Int2> p = maze.findPath(cur, goal);
    if (p.size() < 2) return false;
    std::vector<Int2> cells;
    std::vector<float> keepU;
    const float offset = cellS_.empty() ? 0.0f : s_ - cellS_[i];
    const float curU = cellU_.empty() ? uForSurface(surfacePref_) : uOf(s_);
    const Vec3 heading = cellS_.empty() ? Vec3{0, 0, -1} : path_.tangent(s_);  // (on the old path)
    // keep the cells just behind it; turning round, start one cell further on instead
    size_t prefix = 0;
    bool reverse = false;
    if (!cells_.empty()) {
        const size_t from = i >= 2 ? i - 2 : 0;
        // Turning (round 13). Kept cells behind make a corner at the cell it is in, and a
        // rounded corner runs up to half a metre from the centre: if it has to turn there it
        // would snap sideways. Instead the new path starts with one virtual cell (a wall's,
        // if need be) so the body lies straight on it, and it swings onto it (see pose):
        //   back      the way on is behind it (or, already past the centre, anything but
        //             straight ahead): the virtual cell is the one ahead; it turns round,
        //             goes back to the centre and takes the new way
        //   corner    at the centre, the way on to one side: the virtual cell is behind
        //             it on the new way; it turns on the spot
        // (A path of one cell, just spawned, has no heading yet.)
        bool back = i >= 1 && cells_[i - 1] == p[1];
        bool corner = false;
        if (cells_.size() >= 2) {
            Vec3 way = cellCenter(p[1]) - cellCenter(cur);
            way = normalize(Vec3{way.x, 0.0f, way.z});
            const float d = dot(way, heading);
            back = back || d < -0.5f || (offset > 0.15f && d < 0.7f);
            corner = !back && d < 0.7f && std::abs(offset) <= 0.15f;
        }
        if (back && !allowReverse) return false;
        reverse = back || corner;
        if (reverse) {
            if (back) ++uTurns_;
            Int2 virt;
            if (back) {
                const bool alongX = std::abs(heading.x) >= std::abs(heading.z);
                virt = alongX ? Int2{cur.x + (heading.x > 0.0f ? 1 : -1), cur.y}
                              : Int2{cur.x, cur.y + (heading.z > 0.0f ? 1 : -1)};
            } else {
                virt = Int2{cur.x - (p[1].x - cur.x), cur.y - (p[1].y - cur.y)};
            }
            cells.push_back(virt);
            keepU.push_back(curU);
            ++prefix;
        } else {
            for (size_t k = from; k < i; ++k) {
                cells.push_back(cells_[k]);
                keepU.push_back(cellU_[k]);
                ++prefix;
            }
        }
    }
    for (Int2 c : p) cells.push_back(c);
    CrawlPath path(cells);
    std::vector<float> cs, cu;
    for (size_t k = 0; k < cells.size(); ++k) {
        float sk = path.project(cellCenter(cells[k]));
        if (!cs.empty()) sk = std::max(sk, cs.back() + 0.5f);
        cs.push_back(sk);
    }
    cells_ = cells;
    path_ = std::move(path);
    cellS_ = cs;
    const size_t n = cells.size();
    for (size_t k = 0; k < n; ++k) {
        if (k < prefix) {
            cu.push_back(keepU[k]);
            continue;
        }
        if (k == prefix) {  // the cell it is in keeps its surface
            cu.push_back(curU);
            continue;
        }
        cu.push_back(uForSurface(surfaceFor(maze, cells, k)));
    }
    cellU_ = cu;
    if (reverse) {
        // its middle stays where it was: found on the new path near the cell it is in (the
        // thorax sits tz ahead of s), and it swings from the way it faced onto the new path's
        // way there (about half a turn; less where the new path bends away)
        const float tz = -creature_.restPivot("thorax").z;
        const Vec3 middle = creature_.bodyCenter();
        const float around = path_.project(middle, cellS_[prefix] - 4.0f, cellS_[prefix] + 4.0f);
        s_ = std::max(0.0f, around - tz);
        const float sm = s_ + tz;
        const Vec3 nrm = path_.normal(sm, uOf(sm));
        const Vec3 now = path_.tangent(sm);
        turnAngle_ = std::atan2(dot(nrm, cross(now, heading)), dot(now, heading));
        turnTime_ = std::max(0.3f, kTurnTime * std::abs(turnAngle_) / kPi);  // a quarter turn: 0.4 s
        turnT_ = 0.0f;
    } else {
        s_ = cellS_[prefix] + offset;
    }
    return true;
}

int Hunter::surfaceFor(const Maze& maze, const std::vector<Int2>& cells, size_t k) const {
    const size_t n = cells.size();
    const Int2 c = cells[k];
    const bool room = maze.kind(c) == CellKind::Room;
    const bool doorNext = (k + 1 < n && maze.kind(cells[k + 1]) == CellKind::Room) ||
                          (k > 0 && maze.kind(cells[k - 1]) == CellKind::Room);
    int surf = surfacePref_;
    const bool hunting = state_ == HunterState::Chase || state_ == HunterState::Stalk;
    if (!approachOn_ && state_ == HunterState::Chase && k + 3 >= n) surf = 0;  // drops for the last metres
    if (approachOn_ && hunting && straightCell(maze, c)) {
        // round 13: over the ceiling and both walls, a choice per cell (the same cell always
        // gets the same, so a re-route doesn't change the ones ahead)
        const float r = float(cellHash(c, surfaceSeed_) >> 8) * (1.0f / 16777216.0f);
        surf = r < 0.45f ? 2 : r < 0.725f ? 1 : 3;
    }
    if (room || doorNext) surf = 0;
    else if ((surf == 1 || surf == 3) && !straightCell(maze, c)) surf = 2;
    return surf;
}

void Hunter::reaim(const Maze& maze, Int2 goal) {
    if (!routeAhead(maze, goal)) route(maze, goal);  // (behind it: it turns round)
}

bool Hunter::routeAhead(const Maze& maze, Int2 goal) {
    if (cellS_.empty()) return route(maze, goal, false);
    // the anchor: the first cell centre at least 3.4 m ahead (the new path's first bend
    // starts at most 2.2 m before it, so nothing from the head back moves)
    size_t a = 0;
    while (a < cellS_.size() && cellS_[a] <= s_ + 3.4f) ++a;
    if (a == 0 || a >= cells_.size()) {  // at the end of its path
        lastRoute_ = "ahead, at the end of its path";
        return route(maze, goal, false);
    }
    const size_t i = a - 1;
    const std::vector<Int2> p = maze.findPath(cells_[a], goal);
    if (p.size() < 2 || p[1] == cells_[i]) return false;  // behind it
    const size_t from = i >= 2 ? i - 2 : 0;
    std::vector<Int2> cells(cells_.begin() + std::ptrdiff_t(from), cells_.begin() + std::ptrdiff_t(a));
    std::vector<float> cu(cellU_.begin() + std::ptrdiff_t(from), cellU_.begin() + std::ptrdiff_t(a + 1));
    for (Int2 c : p) cells.push_back(c);
    const float offset = s_ - cellS_[i];
    CrawlPath path(cells);
    std::vector<float> cs;
    for (size_t k = 0; k < cells.size(); ++k) {
        // each cell centre is searched for just past the last (cells are 4 m apart)
        const float lo = cs.empty() ? 0.0f : cs.back() + 0.5f;
        cs.push_back(path.project(cellCenter(cells[k]), lo, lo + 8.0f));
    }
    const size_t n = cells.size();
    for (size_t k = cu.size(); k < n; ++k) cu.push_back(uForSurface(surfaceFor(maze, cells, k)));
    cells_ = cells;
    path_ = std::move(path);
    cellS_ = cs;
    cellU_ = cu;
    s_ = cellS_[i - from] + offset;
    return true;
}

bool Hunter::chooseRoamGoal(const Maze& maze, const Level& level, bool avoidDeadEnds, bool ahead) {
    const Int2 c = cells_.empty() ? cell() : cells_[cells_.size() - 1];
    if (ahead) {
        // one cell before the end of its leg: the next leg branches off from the last cell,
        // so the new corner starts ahead of it (planned at the very end, a turn there would
        // snap it sideways). Never turns round: that waits for the end.
        for (int attempt = 0; attempt < 120; ++attempt) {
            Int2 g{c.x + rng_.range(-9, 9), c.y + rng_.range(-9, 9)};
            if (!maze.inBounds(g) || maze.kind(g) == CellKind::Void) continue;
            if (avoidDeadEnds && deadEnd(maze, g)) continue;
            const int dist = std::abs(g.x - c.x) + std::abs(g.y - c.y);
            if (dist < 4) continue;
            if (level.sections.moodAt(g) == Mood::Lit && !rng_.chance(0.25f)) continue;
            const int keep = surfacePref_;
            const float r = rng_.unit();
            surfacePref_ = r < 0.5f ? 2 : r < 0.65f ? 1 : r < 0.8f ? 3 : 0;
            lastRoute_ = "roam (planned ahead)";
            if (routeAhead(maze, g)) return true;
            surfacePref_ = keep;
        }
        return false;
    }
    for (int attempt = 0; attempt < 60; ++attempt) {
        Int2 g{c.x + rng_.range(-9, 9), c.y + rng_.range(-9, 9)};
        if (!maze.inBounds(g) || maze.kind(g) == CellKind::Void) continue;
        if (avoidDeadEnds && deadEnd(maze, g)) continue;
        const int dist = std::abs(g.x - c.x) + std::abs(g.y - c.y);
        if (dist < 4) continue;
        if (level.sections.moodAt(g) == Mood::Lit && !rng_.chance(0.25f)) continue;
        const float r = rng_.unit();
        surfacePref_ = r < 0.5f ? 2 : r < 0.65f ? 1 : r < 0.8f ? 3 : 0;
        // no U-turns unless there is no other way (a dead end)
        lastRoute_ = attempt >= 45 ? "roam (may turn round)" : "roam";
        if (route(maze, g, attempt >= 45)) return true;
    }
    return false;
}

bool Hunter::chooseDriftGoal(const Maze& maze, const HuntWorld& w) {
    const Int2 pc = cellOf(w.player);
    const int r = std::max(2, w.tuning->driftSpread);
    for (int attempt = 0; attempt < 40; ++attempt) {
        Int2 g{pc.x + rng_.range(-r, r), pc.y + rng_.range(-r, r)};
        if (!maze.inBounds(g) || maze.kind(g) == CellKind::Void || deadEnd(maze, g)) continue;
        if (std::abs(g.x - pc.x) + std::abs(g.y - pc.y) < 2) continue;  // roughly, never exactly
        const int keep = surfacePref_;
        const float s = rng_.unit();
        surfacePref_ = s < 0.5f ? 2 : s < 0.65f ? 1 : s < 0.8f ? 3 : 0;
        lastRoute_ = "drift";
        if (routeAhead(maze, g)) return true;
        surfacePref_ = keep;
    }
    return false;  // you are behind it: it tries again at the next cell centre
}

void Hunter::spawn(const Maze& maze, Int2 c, int surface) {
    cells_ = {c};
    cellS_ = {0.0f};
    surfacePref_ = surface;
    cellU_ = {0.0f};
    path_ = CrawlPath(cells_);
    cellU_ = {uForSurface(surface)};
    s_ = 0.0f;
    health_ = kHealth;
    flinch_ = 0.0f;
    turnT_ = -1.0f;
    setState(HunterState::Roam);
    // a first leg of roaming (no cells behind it yet, so no U-turn to worry about)
    const int pref = surfacePref_;
    for (int attempt = 0; attempt < 60; ++attempt) {
        Int2 g{c.x + rng_.range(-8, 8), c.y + rng_.range(-8, 8)};
        if (!maze.inBounds(g) || maze.kind(g) == CellKind::Void || std::abs(g.x - c.x) + std::abs(g.y - c.y) < 4)
            continue;
        lastRoute_ = "spawn";
        if (route(maze, g)) break;
    }
    surfacePref_ = pref;
}

void Hunter::setState(HunterState s) {
    if (s != state_) stateTime_ = 0.0f;
    state_ = s;
}

bool Hunter::canSee(const HuntWorld& w) const {
    const Vec3 h = creature_.headPosition();
    Vec3 d = w.player - h;
    const float dist = length(d);
    if (dist > 22.0f) return false;
    if (dist < 0.5f) return true;
    d = d * (1.0f / dist);
    Vec3 n;
    const float t = raycastLevel(*w.maze, *w.level, h, d, dist, *w.doorOpen, n);
    return t >= dist - 0.3f;
}

void Hunter::alert(const HuntWorld& w, bool charge) {
    if (!alive() || state_ == HunterState::Leap || state_ == HunterState::Feed) return;
    surfacePref_ = 2;
    investigate_ = false;
    drifting_ = false;
    setState(charge ? HunterState::Chase : HunterState::Stalk);
    stalkTime_ = 0.0f;
    lastRoute_ = "alert";
    reaim(*w.maze, cellOf(w.player));
}

bool Hunter::raycast(Vec3 o, Vec3 d, float maxDist, float& t, Vec3& n) const {
    if (state_ == HunterState::Dead) return false;
    return creature_.raycast(o, d, maxDist, t, n);
}

void Hunter::hit(const HuntWorld& w) {
    if (!alive()) return;
    flinch_ = 1.0f;
    if (--health_ <= 0) {
        creature_.captureDeath();
        setState(HunterState::Dying);
        return;
    }
    if (state_ == HunterState::Roam || state_ == HunterState::Stalk) alert(w, true);
    if (state_ == HunterState::Threat) setState(HunterState::Coil);  // stung: it springs sooner
}

void Hunter::startLeap(const HuntWorld& w, Vec3 head) {
    const Vec3 d = normalize(w.player - head);
    const float dist = length(w.player - head);
    leapFrom_ = creature_.bodyMatrix();
    leapFwd_ = d;
    const Vec3 headRest = creature_.restPivot("head") + Vec3{0, 0, -0.42f};
    const float reach = length(headRest - creature_.restPivot("thorax"));
    leapTarget_ = w.player - d * (0.3f + reach) - Vec3{0, 0.12f, 0};
    if (approachOn_) {
        // round 13: the flight takes longer the farther it springs; off a wall or the
        // ceiling it drops at you with little arc and twists upright on the way
        leapTime_ = std::clamp(dist / 8.0f, kLeapTime, 0.8f);
        leapArc_ = surfaceKind() == 0 ? 0.35f : 0.08f;
        leapBlend_ = 2.5f;
    } else {
        leapTime_ = kLeapTime;
        leapArc_ = 0.35f;
        leapBlend_ = 4.0f;
    }
    turnT_ = -1.0f;
    setState(HunterState::Leap);
}

void Hunter::update(float dt, const HuntWorld& w) {
    stateTime_ += dt;
    flinch_ = std::max(0.0f, flinch_ - dt / 0.35f);
    const Maze& maze = *w.maze;

    if (state_ == HunterState::Dying || state_ == HunterState::Dead) {
        if (state_ == HunterState::Dying && stateTime_ > 2.0f) {
            state_ = HunterState::Dead;  // keeps counting stateTime_ from the death
        }
        creature_.poseDeath(std::min(stateTime_, 8.0f), w.time);
        return;
    }
    if (state_ == HunterState::Feed) return;  // posed by the game on the player's face
    approachOn_ = approach(w);
    threatAgain_ = std::max(0.0f, threatAgain_ - dt);

    const Vec3 head = creature_.headPosition();
    const float dist = length(w.player - head);

    if (state_ == HunterState::Leap) {
        const float t = std::min(1.0f, stateTime_ / leapTime_);
        const Vec3 th = creature_.restPivot("thorax");
        const Vec3 up0{0, 1, 0};
        Vec3 up = normalize(up0 - leapFwd_ * dot(up0, leapFwd_));
        const Vec3 right = cross(leapFwd_, up);
        const Vec3 from = leapFrom_.point(th);
        const Vec3 pos = lerp(from, leapTarget_, t) + up * (leapArc_ * std::sin(kPi * t));
        const Mat4 flying = Mat4::basis(right, up, -leapFwd_, pos) * Mat4::translation(-th);
        const Mat4 body = Creature::blendFrames(leapFrom_, flying, std::min(1.0f, t * leapBlend_));
        creature_.poseRigid(body, 1.0f, 2.2f, 1.0f, -std::min(1.0f, t * 2.5f), w.time);
        if (t >= 1.0f) {
            const bool reached = w.playerAlive && length(w.player - creature_.headPosition()) < 1.4f;
            if (reached && w.grabbable) {
                setState(HunterState::Feed);
            } else {  // missed: lands and comes again
                if (reached) ++reachedPlayer_;  // (benchmarks: the game replaces it far away)
                cells_.clear();
                cellS_.clear();
                cellU_.clear();
                surfacePref_ = 0;
                const int hp = health_;
                spawn(maze, cell(), 0);
                health_ = hp;
                alert(w, true);
                threatAgain_ = 4.0f;  // (it comes straight on for a while: no second threat)
            }
        }
        return;
    }

    // round 13: the threat (rears, jaws, scythes, hiss) and the coil, standing its ground
    if (state_ == HunterState::Threat || state_ == HunterState::Coil) {
        if (!w.playerAlive) {
            setState(HunterState::Roam);
            chooseRoamGoal(maze, *w.level, w.tuning && w.tuning->drift);
        } else if (state_ == HunterState::Threat) {
            if (dist > kThreatRange + 2.5f) {  // you backed off: after you again
                setState(HunterState::Chase);
                reaim(maze, cellOf(w.player));
            } else if (stateTime_ >= kThreatTime) {
                setState(HunterState::Coil);
            }
        } else if (stateTime_ >= kCoilTime) {
            if (dist <= kLeapMax) {
                startLeap(w, head);
                update(0.0f, w);
                return;
            }
            setState(HunterState::Chase);  // out of reach now: close in again
            reaim(maze, cellOf(w.player));
        }
        if (state_ == HunterState::Threat || state_ == HunterState::Coil) {
            pose(w, dt);
            return;
        }
    }

    // senses
    senseTimer_ -= dt;
    bool sees = false;
    if (senseTimer_ <= 0.0f) {
        senseTimer_ = 0.2f;
        sees = w.playerAlive && canSee(w);
        if (state_ == HunterState::Roam) {
            if (sees) alert(w, false);
            else if (w.playerAlive && dist < 5.0f) alert(w, true);
            else if (w.playerAlive && w.playerDist && w.noiseRange > 0.0f && hearCooldown_ <= 0.0f) {
                // footsteps (round 11): within earshot by walking distance, it goes to look
                const Int2 c = cell();
                if (maze.inBounds(c)) {
                    const int d = (*w.playerDist)[size_t(c.y * maze.width() + c.x)];
                    if (d >= 0 && float(d) * kCell <= w.noiseRange) {
                        investigate_ = true;
                        investigateCell_ = cellOf(w.player);
                        hearCooldown_ = 6.0f;
                    }
                }
            }
        }
    }
    hearCooldown_ = std::max(0.0f, hearCooldown_ - dt);
    // drifting toward the player (round 11): armed while roaming, used at the end of a leg
    if (state_ == HunterState::Roam && w.tuning && w.tuning->drift) {
        if (driftTimer_ < 0.0f) driftTimer_ = w.tuning->driftMin + rng_.unit() * (w.tuning->driftMax - w.tuning->driftMin);
        driftTimer_ = std::max(0.0f, driftTimer_ - dt);
    }
    if (w.shotFired && w.playerAlive && dist < 30.0f && state_ != HunterState::Chase) alert(w, true);
    if (!w.playerAlive && state_ != HunterState::Roam) {
        setState(HunterState::Roam);
        chooseRoamGoal(maze, *w.level, w.tuning && w.tuning->drift);
    }
    if (state_ == HunterState::Stalk) {
        stalkTime_ += dt;
        if (dist < 7.5f || stalkTime_ > 4.5f) {
            setState(HunterState::Chase);
            reaim(maze, cellOf(w.player));
        }
    }
    // round 13: close enough, with a clear line, it stops to threaten
    if (approachOn_ && (state_ == HunterState::Chase || state_ == HunterState::Stalk) && w.playerAlive &&
        threatAgain_ <= 0.0f && dist < kThreatRange && dist > 2.2f && turnT_ < 0.0f) {
        Vec3 n;
        const Vec3 d = normalize(w.player - head);
        if (raycastLevel(maze, *w.level, head, d, dist, *w.doorOpen, n) >= dist - 0.3f) {
            setState(HunterState::Threat);
            pose(w, dt);
            return;
        }
    }
    if (state_ == HunterState::Chase && dist < kLeapRange && w.playerAlive && turnT_ < 0.0f) {
        Vec3 n;
        Vec3 d = normalize(w.player - head);
        if (raycastLevel(maze, *w.level, head, d, dist, *w.doorOpen, n) >= dist - 0.3f) {
            startLeap(w, head);
            update(0.0f, w);
            return;
        }
    }

    // move
    float speed = state_ == HunterState::Chase ? (approachOn_ ? kApproachSpeed : kChaseSpeed)
                  : state_ == HunterState::Stalk ? kStalkSpeed : kRoamSpeed;
    if (state_ == HunterState::Roam && drifting_) speed = kDriftSpeed;
    // turning round on the spot: it holds its place while it swings
    if (turnT_ >= 0.0f) {
        turnT_ += dt;
        speed = 0.0f;
    }
    // stalking: a pause to look and hiss when it first sees you
    if (state_ == HunterState::Stalk && stateTime_ < 1.1f) speed *= 0.15f;
    speed *= 1.0f - 0.85f * flinch_;
    const float sOld = s_;
    s_ += speed * dt;
    // at each cell centre: chasers and stalkers re-aim at the player
    bool crossed = false;
    size_t crossedCell = 0;
    for (size_t k = 0; k < cellS_.size(); ++k)
        if (sOld < cellS_[k] && s_ >= cellS_[k]) {
            crossed = true;
            crossedCell = k;
        }
    const bool atEnd = s_ >= cellS_.back();
    if (atEnd) s_ = cellS_.back();
    // roaming, one cell before the end of the leg: the next leg is planned now (see chooseRoamGoal)
    const bool lastButOne = crossed && !atEnd && cellS_.size() >= 2 && crossedCell + 2 == cellS_.size();
    if ((crossed || atEnd) && (state_ == HunterState::Chase || state_ == HunterState::Stalk)) {
        lastRoute_ = "chase";
        reaim(maze, cellOf(w.player));  // (same cell as the player but out of reach: it creeps in)
    } else if ((crossed || atEnd) && state_ == HunterState::Roam && investigate_) {
        // heard you: from this cell centre, to where the sound came from
        // (behind it: it carries on, and turns at the end of its leg like any roamer)
        investigate_ = false;
        drifting_ = false;
        lastRoute_ = "heard";
        if (!routeAhead(maze, investigateCell_) && atEnd) chooseRoamGoal(maze, *w.level, w.tuning && w.tuning->drift);
    } else if ((crossed || atEnd) && state_ == HunterState::Roam && w.tuning && w.tuning->drift &&
               driftTimer_ == 0.0f && w.playerAlive && chooseDriftGoal(maze, w)) {
        // drift: from this cell centre, toward roughly where you are; then a new wait
        driftTimer_ = -1.0f;
        drifting_ = true;
        ++drifts_;
    } else if (lastButOne && state_ == HunterState::Roam) {
        if (chooseRoamGoal(maze, *w.level, w.tuning && w.tuning->drift, true)) drifting_ = false;
    } else if (atEnd && state_ == HunterState::Roam) {
        drifting_ = false;
        chooseRoamGoal(maze, *w.level, w.tuning && w.tuning->drift);
    }
    pose(w, dt);
}

void Hunter::pose(const HuntWorld& w, float dt) {
    const bool threat = state_ == HunterState::Threat, coil = state_ == HunterState::Coil;
    // (threatening and coiling, its seams and throat flare: it lights the surface it is on)
    const float hunting = threat || coil ? 2.0f
                          : state_ == HunterState::Chase ? 1.0f : state_ == HunterState::Stalk ? 0.6f : 0.0f;
    const float targetGlow = 0.55f + 0.9f * hunting + 0.12f * std::sin(w.time * (2.0f + 4.0f * hunting));
    glow_ += (targetGlow - glow_) * std::min(1.0f, dt * 3.0f);
    float jawTarget = 0.06f + 0.05f * std::sin(w.time * 1.3f);
    if (state_ == HunterState::Stalk) jawTarget = stateTime_ < 1.1f ? 0.75f : 0.3f + 0.15f * std::sin(w.time * 5.0f);
    if (state_ == HunterState::Chase) jawTarget = 0.45f + 0.2f * std::sin(w.time * 9.0f);
    // round 13: jaws wide and quivering for the threat, half shut as it coils
    if (threat) jawTarget = 0.92f + 0.08f * std::sin(w.time * 31.0f);
    if (coil) jawTarget = 0.5f;
    jaw_ += (jawTarget - jaw_) * std::min(1.0f, dt * 8.0f);
    // the threat: rear up, scythes high; the coil: sink low, scythes still up
    const float rearTarget = threat ? 1.0f : 0.0f;
    const float crouchTarget = coil ? 1.0f : threat ? 0.1f : 0.0f;
    const float armsTarget = threat || coil ? 1.0f : 0.0f;
    const float k = 1.0f - std::exp(-(coil ? 10.0f : 6.0f) * dt);
    rear_ += (rearTarget - rear_) * k;
    crouch_ += (crouchTarget - crouch_) * k;
    arms_ += (armsTarget - arms_) * (1.0f - std::exp(-8.0f * dt));
    CreaturePose p;
    p.s = s_;
    p.uOf = [this](float s) { return uOf(s); };
    p.time = w.time;
    p.jaw = jaw_;
    p.glow = glow_;
    p.look = hunting > 0.0f;
    p.lookAt = w.player;
    p.flinch = flinch_;
    p.rear = rear_;
    p.crouch = crouch_;
    p.arms = arms_;
    creature_.pose(path_, p);
    // turning round: the whole of it swings round its middle onto the new path
    if (turnT_ >= 0.0f) {
        const float e = smooth(0.0f, turnTime_, turnT_);
        const float sm = s_ - creature_.restPivot("thorax").z;
        creature_.rotateAboutCenter(path_.normal(sm, uOf(sm)), turnAngle_ * (1.0f - e));
        if (turnT_ >= turnTime_) turnT_ = -1.0f;
    }
}

void Hunter::poseOnFace(const Camera& cam, float t, float time) {
    const Vec3 F = cam.forward(), U = cam.up();
    const Vec3 fwd = -F;  // it faces you
    const Vec3 right = cross(fwd, U);
    const Vec3 th = creature_.restPivot("thorax");
    const Vec3 headRest = creature_.restPivot("head") + Vec3{0, 0, -0.42f};
    const float reach = length(headRest - th);
    // biting: it pumps toward you, jaws working
    const float pump = 0.07f * std::max(0.0f, std::sin(time * 9.0f));
    const float settle = std::min(1.0f, t / 0.25f);
    const Vec3 headAt = cam.position + F * (0.62f - pump) - U * (0.06f * settle);
    const Vec3 pos = headAt + F * reach;
    const Vec3 up = normalize(U - fwd * dot(U, fwd));
    const Mat4 body = Mat4::basis(right, up, -fwd, pos) * Mat4::translation(-th);
    const float jaw = 0.75f + 0.25f * std::sin(time * 18.0f);
    creature_.poseRigid(body, jaw, 1.1f, 1.0f, -0.7f, time);
    glow_ = 0.5f;  // its light: dimmer, so the jaws keep their shape
}

}  // namespace bh
