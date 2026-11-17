#include "game/Game.h"

#include <algorithm>
#include <cmath>

namespace bh {
namespace {

constexpr float kPlayerRadius = 0.32f;
constexpr float kDoorTrigger = 3.4f;   // metres from a door at which it opens
constexpr float kDoorTime = 0.7f;      // seconds to open fully

float yawToward(Vec3 from, Vec3 to) {
    Vec3 d = to - from;
    return std::atan2(-d.x, -d.z);
}

}  // namespace

Game::Game(const Kit& kit, const MazeParams& params)
    : kit_(kit), maze_(Maze::generate(params)), level_(buildLevel(maze_, kit)), walk_(level_.route) {
    doorOpen_.assign(level_.doors.size(), 0.0f);
    camera_.position = level_.spawn;
    camera_.yaw = level_.spawnYaw;
    gunModel_ = kit.find("handgun");
    explored_.assign(size_t(maze_.width() * maze_.height()), 0);
    updateLighting();
    lightingChange_ = 0.0f;
    reveal();
}

void Game::setCamera(const Camera& c) {
    camera_ = c;
    reveal();
}

void Game::setTime(float t) {
    time_ = t;
    updateLighting();
}

Int2 Game::cameraCell() const {
    return {int(std::floor(camera_.position.x / kCell + 0.5f)), int(std::floor(camera_.position.z / kCell + 0.5f))};
}

Mood Game::moodAtCamera() const { return level_.sections.moodAt(cameraCell()); }

void Game::updateLighting() {
    const Sections& sec = level_.sections;
    const size_t n = sec.power.size();
    const Int2 cc = cameraCell();
    float before = 0.0f, after = 0.0f, diff = 0.0f;
    auto lum = [](Vec3 v) { return 0.2126f * v.x + 0.7152f * v.y + 0.0722f * v.z; };
    const bool first = cellLight_.size() != n;
    cellLight_.resize(n);
    cellEmissive_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const Vec3 old = cellLight_[i];
        cellLight_[i] = lightMultiplier(sec.power[i], time_);
        cellEmissive_[i] = emissiveMultiplier(sec.power[i], time_);
        const int x = int(i) % sec.width, y = int(i) / sec.width;
        if (!first && std::abs(x - cc.x) <= 2 && std::abs(y - cc.y) <= 2) {
            before += lum(old);
            after += lum(cellLight_[i]);
            diff += std::abs(lum(cellLight_[i]) - lum(old));
        }
    }
    lightingChange_ = diff / std::max(0.15f, std::max(before, after));
    lightRadiance_.resize(level_.lights.size());
    for (size_t i = 0; i < level_.lights.size(); ++i) {
        const WorldLight& l = level_.lights[i];
        const Vec3 k = cellLight_[size_t(l.cell)];
        lightRadiance_[i] = {l.radiance.x * k.x, l.radiance.y * k.y, l.radiance.z * k.z};
    }
}

// Marks what the player can see from where they stand: their cell (a whole room when in
// one), straight down each open corridor a few cells, and the side openings along it.
// Void cells next to a seen cell are marked too, so the map shows the walls around it.
void Game::reveal() {
    const Int2 c = cameraCell();
    if (!maze_.inBounds(c) || maze_.kind(c) == CellKind::Void) return;
    const int W = maze_.width();
    auto mark = [&](Int2 m) {
        if (!maze_.inBounds(m)) return;
        explored_[size_t(m.y * W + m.x)] = 1;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                Int2 n{m.x + dx, m.y + dy};
                if (maze_.inBounds(n) && maze_.kind(n) == CellKind::Void) explored_[size_t(n.y * W + n.x)] = 1;
            }
    };
    auto see = [&](Int2 from, Dir d) {
        const Edge e = maze_.edge(from.x, from.y, d);
        if (e == Edge::Wall) return false;
        if (e == Edge::Door) {
            // through a door only while it is open
            for (const Door& door : level_.doors) {
                Int2 b{door.cell.x + kDx[int(door.side)], door.cell.y + kDy[int(door.side)]};
                Int2 to{from.x + kDx[int(d)], from.y + kDy[int(d)]};
                bool same = (door.cell == from && b == to) || (door.cell == to && b == from);
                if (same) return doorOpen_[size_t(&door - level_.doors.data())] > 0.5f;
            }
            return false;
        }
        return true;
    };
    mark(c);
    if (int r = maze_.roomAt(c.x, c.y); r >= 0) {
        const Room& room = maze_.rooms()[size_t(r)];
        for (int y = room.y; y < room.y + room.h; ++y)
            for (int x = room.x; x < room.x + room.w; ++x) mark({x, y});
    }
    for (int di = 0; di < 4; ++di) {
        Int2 cur = c;
        for (int k = 0; k < 7; ++k) {
            if (!see(cur, Dir(di))) break;
            cur = {cur.x + kDx[di], cur.y + kDy[di]};
            mark(cur);
            // peek into side openings
            for (int si : {(di + 1) & 3, (di + 3) & 3})
                if (maze_.edge(cur.x, cur.y, Dir(si)) == Edge::Open) mark({cur.x + kDx[si], cur.y + kDy[si]});
            if (maze_.kind(cur) == CellKind::Room) break;
        }
    }
}

Flashlight Game::flashlight() const {
    Flashlight f;
    f.on = flashlightOn_;
    const Vec3 R = camera_.right(), U = camera_.up(), F = camera_.forward();
    // under the barrel, just past the muzzle, aimed where the crosshair looks at ~8 m
    f.position = camera_.position + R * 0.15f + U * -0.13f + F * 0.55f;
    f.direction = normalize(camera_.position + F * 8.0f - f.position);
    f.intensity = 46.0f;
    f.cosInner = std::cos(9.0f * kPi / 180.0f);
    f.cosOuter = std::cos(24.0f * kPi / 180.0f);
    return f;
}

void Game::update(float dt, const InputState& in) {
    const float sens = 0.0022f;
    camera_.yaw -= in.mouseDx * sens;
    camera_.pitch = std::clamp(camera_.pitch - in.mouseDy * sens, -1.35f, 1.35f);

    Vec3 f = camera_.forward();
    Vec2 fwd = Vec2{f.x, f.z};
    float fl = length(fwd);
    fwd = fl > 1e-5f ? fwd * (1.0f / fl) : Vec2{0, -1};
    Vec3 r3 = camera_.right();  // already horizontal
    Vec2 right{r3.x, r3.z};
    Vec2 wish{};
    if (in.forward) wish += fwd;
    if (in.back) wish += fwd * -1.0f;
    if (in.right) wish += right;
    if (in.left) wish += right * -1.0f;
    float wl = length(wish);
    float speed = in.run ? 5.8f : 3.4f;
    Vec2 target = wl > 0 ? wish * (speed / wl) : Vec2{};
    float k = std::min(1.0f, dt * 12.0f);
    velocity_ = velocity_ + (target - velocity_) * k;

    Vec2 p{camera_.position.x, camera_.position.z};
    Vec2 np = level_.collision.move(p, velocity_ * dt, kPlayerRadius, doorOpen_);
    float moved = length(np - p);
    // one vertical bob per footstep, at about two steps a second
    float spd = length(velocity_);
    float stepLength = std::max(0.6f, spd / stepRate(spd));
    bob_ += moved / stepLength * kPi;
    bobAmount_ += ((wl > 0 ? 1.0f : 0.0f) - bobAmount_) * std::min(1.0f, dt * 6.0f);
    float amp = in.run ? 0.03f : 0.022f;
    Vec2 side = right * (0.012f * bobAmount_ * std::sin(bob_));
    camera_.position = {np.x + side.x, kEyeHeight + amp * bobAmount_ * std::sin(2.0f * bob_), np.y + side.y};
    updateDoors(dt);
    time_ += dt;
    updateLighting();
    reveal();
}

float Game::walkDuration() const { return kStartPause + walk_.length() / kWalkSpeed + kEndPause; }

void Game::setWalkTime(float t) {
    float s = std::clamp((t - kStartPause) * kWalkSpeed, 0.0f, walk_.length());
    Vec2 p = walk_.at(s);
    // average a few look-ahead directions so turns ease in
    Vec2 d{};
    // short look-aheads: turning the head too early stares into door jambs
    for (float la : {0.7f, 1.4f, 2.1f}) d += walk_.direction(s, la);
    camera_.yaw = std::atan2(-d.x, -d.y);
    camera_.pitch = -0.035f;
    float walking = (s > 0.0f && s < walk_.length()) ? 1.0f : 0.0f;
    bobAmount_ = walking;
    bob_ = s / (kWalkSpeed / stepRate(kWalkSpeed)) * kPi;
    Vec3 r = camera_.right();
    float side = 0.012f * walking * std::sin(bob_);
    camera_.position = {p.x + r.x * side, kEyeHeight + 0.022f * walking * std::sin(2.0f * bob_), p.y + r.z * side};
    time_ = t;
    updateLighting();
    reveal();
}

void Game::updateDoors(float dt) {
    for (size_t i = 0; i < level_.doors.size(); ++i) {
        Vec3 c = level_.doors[i].center;
        float dx = camera_.position.x - c.x, dz = camera_.position.z - c.z;
        bool want = dx * dx + dz * dz < kDoorTrigger * kDoorTrigger;
        float step = dt / kDoorTime;
        doorOpen_[i] = std::clamp(doorOpen_[i] + (want ? step : -step), 0.0f, 1.0f);
    }
}

void Game::openDoorsNear(Vec3 p, float radius) {
    for (size_t i = 0; i < level_.doors.size(); ++i) {
        Vec3 c = level_.doors[i].center;
        float dx = p.x - c.x, dz = p.z - c.z;
        doorOpen_[i] = (dx * dx + dz * dz < radius * radius) ? 1.0f : 0.0f;
    }
}

bool Game::reachedExit() const {
    float dx = camera_.position.x - level_.exitPoint.x, dz = camera_.position.z - level_.exitPoint.z;
    return dx * dx + dz * dz < 1.6f * 1.6f;
}

void Game::buildInstances(std::vector<SceneInstance>& out, bool withGun) const {
    out.clear();
    for (const PlacedModel& pm : level_.placed) {
        const KitModel& m = kit_.model(pm.model);
        for (uint32_t part : m.parts) {
            Mat4 t = pm.transform;
            if (pm.door >= 0) {
                // ease the leaves so they start and stop softly
                float o = doorOpen_[size_t(pm.door)];
                o = o * o * (3.0f - 2.0f * o);
                t = t * Mat4::translation(doorLeafOffset(kit_.parts()[part].name, o));
            }
            Vec3 e = pm.cell >= 0 && size_t(pm.cell) < cellEmissive_.size() ? cellEmissive_[size_t(pm.cell)]
                                                                            : Vec3{1, 1, 1};
            out.push_back({part, t, 0, e});
        }
    }
    if (withGun && gunModel_ >= 0) {
        Vec3 R = camera_.right(), U = camera_.up(), F = camera_.forward();
        float sway = bobAmount_;
        Vec3 pos = camera_.position + R * (0.15f + 0.006f * sway * std::sin(bob_)) +
                   U * (-0.165f + 0.004f * sway * std::sin(2.0f * bob_)) + F * 0.36f;
        // model axes: +X right, +Y up, -Z forward
        Mat4 view = Mat4::basis(R, U, -F, pos);
        Mat4 t = view * Mat4::rotationY(0.055f) * Mat4::rotationX(0.025f);
        for (uint32_t part : kit_.model(gunModel_).parts) out.push_back({part, t, kInstanceGun});
    }
}

std::vector<Shot> Game::findShots() const {
    std::vector<Shot> shots;
    const Maze& m = maze_;
    auto passable = [&](Int2 c, Dir d) { return m.inBounds(c) && m.passable(c.x, c.y, d); };
    auto step = [](Int2 c, Dir d) { return Int2{c.x + kDx[int(d)], c.y + kDy[int(d)]}; };

    // corridor: the longest straight run of corridor cells
    {
        int best = 0;
        Camera cam;
        for (int y = 0; y < m.height(); ++y)
            for (int x = 0; x < m.width(); ++x) {
                if (m.kind(x, y) != CellKind::Corridor) continue;
                for (int di = 0; di < 4; ++di) {
                    Dir d = Dir(di);
                    if (passable({x, y}, opposite(d))) continue;  // start of a run
                    int run = 0;
                    Int2 c{x, y};
                    while (m.kind(c) == CellKind::Corridor && passable(c, d)) {
                        c = step(c, d);
                        ++run;
                    }
                    if (run > best) {
                        best = run;
                        Vec3 p = cellCenter(x, y) - dirVector(d) * 0.4f;
                        cam.position = {p.x, kEyeHeight, p.z};
                        cam.yaw = yawFor(d);
                        cam.pitch = -0.02f;
                    }
                }
            }
        if (best > 0) shots.push_back({"corridor", cam});
    }
    // door: seen from the corridor side, prefer a sliding door
    {
        int bestScore = -1;
        Camera cam;
        for (const Door& door : level_.doors) {
            Int2 a = door.cell, b = step(a, door.side);
            Int2 corr = m.kind(a) == CellKind::Corridor ? a : b;
            Dir toward = corr == a ? door.side : opposite(door.side);
            if (m.kind(corr) != CellKind::Corridor) continue;
            int score = door.kind == DoorKind::Sliding ? 2 : 1;
            // a straight approach reads best
            if (passable(corr, opposite(toward))) score += 2;
            if (score > bestScore) {
                bestScore = score;
                Vec3 p = cellCenter(corr) - dirVector(toward) * 1.3f;
                cam.position = {p.x + dirVector(toward).z * 0.25f, kEyeHeight, p.z - dirVector(toward).x * 0.25f};
                cam.yaw = yawToward(cam.position, door.center);
                cam.pitch = -0.06f;
            }
        }
        if (bestScore >= 0) shots.push_back({"door", cam});
    }
    // room: the biggest room, from one corner toward the opposite one
    {
        const Room* best = nullptr;
        for (const Room& r : m.rooms())
            if (!best || r.w * r.h > best->w * best->h) best = &r;
        if (best) {
            // from a corner that is clear of props, toward the opposite corner
            const std::vector<float> closed(level_.doors.size(), 0.0f);
            const int x0 = best->x, x1 = best->x + best->w - 1, y0 = best->y, y1 = best->y + best->h - 1;
            const struct {
                Int2 from, to;
                float sx, sz;
            } corners[] = {{{x0, y1}, {x1, y0}, -1, 1}, {{x1, y0}, {x0, y1}, 1, -1},
                           {{x0, y0}, {x1, y1}, -1, -1}, {{x1, y1}, {x0, y0}, 1, 1}};
            for (const auto& k : corners) {
                Vec3 c0 = cellCenter(k.from) + Vec3{0.9f * k.sx, 0, 0.9f * k.sz};
                if (level_.collision.overlaps({c0.x, c0.z}, 0.35f, closed)) continue;
                Camera cam;
                cam.position = {c0.x, kEyeHeight, c0.z};
                cam.yaw = yawToward(cam.position, cellCenter(k.to));
                cam.pitch = -0.08f;
                shots.push_back({"room", cam});
                break;
            }
        }
    }
    // junction: look into a cross or tee from a neighbouring corridor cell
    {
        int bestOpen = 0;
        Camera cam;
        for (int y = 0; y < m.height(); ++y)
            for (int x = 0; x < m.width(); ++x) {
                if (m.kind(x, y) != CellKind::Corridor || m.passableCount(x, y) < 3) continue;
                int open = m.passableCount(x, y);
                if (open <= bestOpen) continue;
                for (int di = 0; di < 4; ++di) {
                    Dir d = Dir(di);
                    Int2 nb = step({x, y}, d);
                    if (!passable({x, y}, d) || m.kind(nb) != CellKind::Corridor) continue;
                    bestOpen = open;
                    Vec3 p = cellCenter(nb) + dirVector(d) * 0.8f;
                    cam.position = {p.x, kEyeHeight, p.z};
                    cam.yaw = yawFor(opposite(d));
                    cam.pitch = -0.04f;
                    break;
                }
            }
        if (bestOpen > 0) shots.push_back({"junction", cam});
    }
    // one corridor per mood: the longest straight run of corridor cells inside that mood
    const Sections& sec = level_.sections;
    for (int mi = 0; mi < kMoodCount; ++mi) {
        const Mood mood = Mood(mi);
        auto inMood = [&](Int2 c) { return m.kind(c) == CellKind::Corridor && sec.moodAt(c) == mood; };
        int best = 0;
        Camera cam;
        for (int y = 0; y < m.height(); ++y)
            for (int x = 0; x < m.width(); ++x) {
                if (!inMood({x, y})) continue;
                for (int di = 0; di < 4; ++di) {
                    Dir d = Dir(di);
                    // start of a run: nothing in this mood straight behind
                    if (passable({x, y}, opposite(d)) && inMood(step({x, y}, opposite(d)))) continue;
                    int run = 1;
                    Int2 c{x, y};
                    while (passable(c, d) && inMood(step(c, d))) {
                        c = step(c, d);
                        ++run;
                    }
                    if (run > best) {
                        best = run;
                        Vec3 p = cellCenter(x, y) - dirVector(d) * 0.4f;
                        cam.position = {p.x, kEyeHeight, p.z};
                        cam.yaw = yawFor(d);
                        cam.pitch = -0.03f;
                    }
                }
            }
        if (best >= 2) shots.push_back({"corridor_" + std::string(toString(mood)), cam});
    }
    return shots;
}

}  // namespace bh
