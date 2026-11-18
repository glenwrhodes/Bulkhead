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

}  // namespace

Hunter::Hunter(const Kit& kit, uint64_t seed) : creature_(kit), rng_(seed, 0x4a17e5u) {}

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
    // keep the cells just behind it, unless it is turning round
    size_t prefix = 0;
    if (!cells_.empty()) {
        const size_t from = i >= 2 ? i - 2 : 0;
        bool reverse = i >= 1 && cells_[i - 1] == p[1];
        if (reverse && !allowReverse) return false;
        if (!reverse)
            for (size_t k = from; k < i; ++k) {
                cells.push_back(cells_[k]);
                keepU.push_back(cellU_[k]);
                ++prefix;
            }
    }
    const float offset = cellS_.empty() ? 0.0f : s_ - cellS_[i];
    const float curU = cellU_.empty() ? uForSurface(surfacePref_) : uOf(s_);
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
        const Int2 c = cells[k];
        const bool room = maze.kind(c) == CellKind::Room;
        const bool doorNext = (k + 1 < n && maze.kind(cells[k + 1]) == CellKind::Room) ||
                              (k > 0 && maze.kind(cells[k - 1]) == CellKind::Room);
        int surf = surfacePref_;
        if (state_ == HunterState::Chase && k + 3 >= n) surf = 0;  // drops for the last metres
        if (room || doorNext) surf = 0;
        else if ((surf == 1 || surf == 3) && !straightCell(maze, c)) surf = 2;
        cu.push_back(uForSurface(surf));
    }
    cellU_ = cu;
    s_ = cellS_[prefix] + offset;
    return true;
}

void Hunter::chooseRoamGoal(const Maze& maze, const Level& level) {
    const Int2 c = cells_.empty() ? cell() : cells_[cells_.size() - 1];
    for (int attempt = 0; attempt < 60; ++attempt) {
        Int2 g{c.x + rng_.range(-9, 9), c.y + rng_.range(-9, 9)};
        if (!maze.inBounds(g) || maze.kind(g) == CellKind::Void) continue;
        const int dist = std::abs(g.x - c.x) + std::abs(g.y - c.y);
        if (dist < 4) continue;
        if (level.sections.moodAt(g) == Mood::Lit && !rng_.chance(0.25f)) continue;
        const float r = rng_.unit();
        surfacePref_ = r < 0.5f ? 2 : r < 0.65f ? 1 : r < 0.8f ? 3 : 0;
        // no U-turns unless there is no other way (a dead end)
        if (route(maze, g, attempt >= 45)) return;
    }
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
    setState(HunterState::Roam);
    // a first leg of roaming (no cells behind it yet, so no U-turn to worry about)
    const int pref = surfacePref_;
    for (int attempt = 0; attempt < 60; ++attempt) {
        Int2 g{c.x + rng_.range(-8, 8), c.y + rng_.range(-8, 8)};
        if (!maze.inBounds(g) || maze.kind(g) == CellKind::Void || std::abs(g.x - c.x) + std::abs(g.y - c.y) < 4)
            continue;
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
    setState(charge ? HunterState::Chase : HunterState::Stalk);
    stalkTime_ = 0.0f;
    route(*w.maze, cellOf(w.player));
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

    const Vec3 head = creature_.headPosition();
    const float dist = length(w.player - head);

    if (state_ == HunterState::Leap) {
        const float t = std::min(1.0f, stateTime_ / kLeapTime);
        const Vec3 th = creature_.restPivot("thorax");
        const Vec3 up0{0, 1, 0};
        Vec3 up = normalize(up0 - leapFwd_ * dot(up0, leapFwd_));
        const Vec3 right = cross(leapFwd_, up);
        const Vec3 from = leapFrom_.point(th);
        const Vec3 pos = lerp(from, leapTarget_, t) + up * (0.35f * std::sin(kPi * t));
        const Mat4 flying = Mat4::basis(right, up, -leapFwd_, pos) * Mat4::translation(-th);
        const Mat4 body = Creature::blendFrames(leapFrom_, flying, std::min(1.0f, t * 4.0f));
        creature_.poseRigid(body, 1.0f, 2.2f, 1.0f, -std::min(1.0f, t * 2.5f), w.time);
        if (t >= 1.0f) {
            if (w.playerAlive && length(w.player - creature_.headPosition()) < 1.4f) {
                setState(HunterState::Feed);
            } else {  // missed: lands and comes again
                cells_.clear();
                cellS_.clear();
                cellU_.clear();
                surfacePref_ = 0;
                const int hp = health_;
                spawn(maze, cell(), 0);
                health_ = hp;
                alert(w, true);
            }
        }
        return;
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
        }
    }
    if (w.shotFired && w.playerAlive && dist < 30.0f && state_ != HunterState::Chase) alert(w, true);
    if (!w.playerAlive && state_ != HunterState::Roam) {
        setState(HunterState::Roam);
        chooseRoamGoal(maze, *w.level);
    }
    if (state_ == HunterState::Stalk) {
        stalkTime_ += dt;
        if (dist < 7.5f || stalkTime_ > 4.5f) {
            setState(HunterState::Chase);
            route(maze, cellOf(w.player));
        }
    }
    if (state_ == HunterState::Chase && dist < kLeapRange && w.playerAlive) {
        Vec3 n;
        Vec3 d = normalize(w.player - head);
        if (raycastLevel(maze, *w.level, head, d, dist, *w.doorOpen, n) >= dist - 0.3f) {
            leapFrom_ = creature_.bodyMatrix();
            leapFwd_ = d;
            const Vec3 headRest = creature_.restPivot("head") + Vec3{0, 0, -0.42f};
            const float reach = length(headRest - creature_.restPivot("thorax"));
            leapTarget_ = w.player - d * (0.3f + reach) - Vec3{0, 0.12f, 0};
            setState(HunterState::Leap);
            update(0.0f, w);
            return;
        }
    }

    // move
    float speed = state_ == HunterState::Chase ? kChaseSpeed : state_ == HunterState::Stalk ? kStalkSpeed : kRoamSpeed;
    // stalking: a pause to look and hiss when it first sees you
    if (state_ == HunterState::Stalk && stateTime_ < 1.1f) speed *= 0.15f;
    speed *= 1.0f - 0.85f * flinch_;
    const float sOld = s_;
    s_ += speed * dt;
    // at each cell centre: chasers and stalkers re-aim at the player
    bool crossed = false;
    for (float cs : cellS_)
        if (sOld < cs && s_ >= cs) crossed = true;
    const bool atEnd = s_ >= cellS_.back();
    if (atEnd) s_ = cellS_.back();
    if ((crossed || atEnd) && (state_ == HunterState::Chase || state_ == HunterState::Stalk)) {
        if (!route(maze, cellOf(w.player)) && atEnd) {
            // same cell as the player but out of leap reach: creep in
        }
    } else if (atEnd && state_ == HunterState::Roam) {
        chooseRoamGoal(maze, *w.level);
    }
    pose(w, dt);
}

void Hunter::pose(const HuntWorld& w, float dt) {
    const float hunting = state_ == HunterState::Chase ? 1.0f : state_ == HunterState::Stalk ? 0.6f : 0.0f;
    const float targetGlow = 0.55f + 0.9f * hunting + 0.12f * std::sin(w.time * (2.0f + 4.0f * hunting));
    glow_ += (targetGlow - glow_) * std::min(1.0f, dt * 3.0f);
    float jawTarget = 0.06f + 0.05f * std::sin(w.time * 1.3f);
    if (state_ == HunterState::Stalk) jawTarget = stateTime_ < 1.1f ? 0.75f : 0.3f + 0.15f * std::sin(w.time * 5.0f);
    if (state_ == HunterState::Chase) jawTarget = 0.45f + 0.2f * std::sin(w.time * 9.0f);
    jaw_ += (jawTarget - jaw_) * std::min(1.0f, dt * 8.0f);
    CreaturePose p;
    p.s = s_;
    p.uOf = [this](float s) { return uOf(s); };
    p.time = w.time;
    p.jaw = jaw_;
    p.glow = glow_;
    p.look = hunting > 0.0f;
    p.lookAt = w.player;
    p.flinch = flinch_;
    creature_.pose(path_, p);
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
