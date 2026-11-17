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
}

void Game::setCamera(const Camera& c) { camera_ = c; }

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
    bob_ += moved / 0.75f * kPi;
    bobAmount_ += ((wl > 0 ? 1.0f : 0.0f) - bobAmount_) * std::min(1.0f, dt * 6.0f);
    camera_.position = {np.x, kEyeHeight + 0.025f * bobAmount_ * std::sin(2.0f * bob_), np.y};
    updateDoors(dt);
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
    bob_ = s / 0.75f * kPi;
    camera_.position = {p.x, kEyeHeight + 0.02f * walking * std::sin(2.0f * bob_), p.y};
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
            out.push_back({part, t, 0});
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
            Camera cam;
            Vec3 c0 = cellCenter(best->x, best->y + best->h - 1) + Vec3{-0.9f, 0, 0.9f};
            Vec3 c1 = cellCenter(best->x + best->w - 1, best->y);
            cam.position = {c0.x, kEyeHeight, c0.z};
            cam.yaw = yawToward(cam.position, c1);
            cam.pitch = -0.08f;
            shots.push_back({"room", cam});
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
    return shots;
}

}  // namespace bh
