#include "game/Game.h"

#include <algorithm>
#include <cmath>

namespace bh {
namespace {

constexpr float kPlayerRadius = 0.32f;
constexpr float kDoorTrigger = 3.4f;   // metres from a door at which it opens
constexpr float kDoorTime = 0.7f;      // seconds to open fully
constexpr float kSparkSlot = 0.45f;    // spark emitters may burst once per slot
constexpr float kSparkLife = 1.1f;     // a burst's sparks are all gone after this
constexpr float kSteamSlot = 3.2f;     // steam hisses on or off per slot
constexpr float kEffectRange = 24.0f;  // effects farther than this from the camera are skipped
constexpr int kMaxDynamicLights = 16;

float yawToward(Vec3 from, Vec3 to) {
    Vec3 d = to - from;
    return std::atan2(-d.x, -d.z);
}

uint32_t mix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
uint32_t hash2(uint32_t a, uint32_t b) { return mix32(a * 0x9e3779b1u ^ mix32(b + 0x632be5abu)); }
float unit(uint32_t h) { return float(h >> 8) * (1.0f / 16777216.0f); }
uint32_t slotOf(float t, float slot) { return uint32_t(int64_t(std::floor(t / slot)) & 0x7fffffff); }

float smooth01(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// A spark burst of emitter i in slot m, if there is one: its start time and seed.
bool sparkBurst(uint32_t i, uint32_t m, float phase, float& t0, uint32_t& seed) {
    uint32_t h = hash2(i * 7919u + 1u, m);
    if (unit(h) < 0.8f) return false;
    t0 = (float(m) + 0.55f * unit(mix32(h))) * kSparkSlot - phase * kSparkSlot;
    seed = mix32(h ^ 0x5bd1e995u);
    return true;
}

// 0..1 how hard steam emitter i is blowing at time t: on or off per slot, easing between.
float steamStrength(uint32_t i, float t, float phase) {
    float local = t / kSteamSlot + phase * 3.0f;
    float m = std::floor(local);
    auto on = [&](float s) { return unit(hash2(i * 104729u + 7u, uint32_t(int64_t(s) & 0x7fffffff))) > 0.3f ? 1.0f : 0.0f; };
    float a = on(m), b = on(m + 1.0f);
    float w = smooth01(0.82f, 1.0f, local - m);
    float s = a + (b - a) * w;
    return s * (0.88f + 0.12f * std::sin(t * 19.0f + phase * 30.0f));
}

Vec3 anyPerpendicular(Vec3 d) {
    Vec3 ref = std::abs(d.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    return normalize(cross(ref, d));
}

}  // namespace

Game::Game(const Kit& kit, const MazeParams& params)
    : kit_(kit), maze_(Maze::generate(params)), level_(buildLevel(maze_, kit)), walk_(level_.route) {
    doorOpen_.assign(level_.doors.size(), 0.0f);
    camera_.position = level_.spawn;
    camera_.yaw = level_.spawnYaw;
    gunModel_ = kit.find("handgun");
    armsModel_ = kit.find("arms");
    flashModel_ = kit.find("muzzle_flash");
    boltModel_ = kit.find("bolt");
    sparkModel_ = kit.find("spark");
    scorchModel_ = kit.find("scorch");
    if (gunModel_ >= 0)
        for (const KitMarker& mk : kit.model(gunModel_).markers)
            if (mk.name == "muzzle") muzzleLocal_ = mk.position;
    explored_.assign(size_t(maze_.width() * maze_.height()), 0);
    buildWalkTiming();
    updateLighting();
    lightingChange_ = 0.0f;
    reveal();
    updateEffects();
}

void Game::setCamera(const Camera& c) {
    camera_ = c;
    reveal();
    updateEffects();
}

void Game::setTime(float t) {
    time_ = t;
    updateLighting();
    updateEffects();
}

// ---------------------------------------------------------------------------
// Scripted walk timing: the walk slows to a crouch under low pipes
// ---------------------------------------------------------------------------

void Game::buildWalkTiming() {
    walkS_.clear();
    walkT_.clear();
    walkC_.clear();
    const float ds = 0.05f;
    float t = 0.0f;
    for (float s = 0.0f;; s += ds) {
        s = std::min(s, walk_.length());
        Vec2 p = walk_.at(s);
        float dmin = 1e9f;
        for (const LowPipe& lp : level_.lowPipes) dmin = std::min(dmin, length(Vec2{p.x - lp.center.x, p.y - lp.center.z}));
        float c = 1.0f - smooth01(1.2f, 2.4f, dmin);
        if (!walkS_.empty()) {
            float v = kWalkSpeed + (kCrouchWalkSpeed - kWalkSpeed) * 0.5f * (c + walkC_.back());
            t += (s - walkS_.back()) / v;
        }
        walkS_.push_back(s);
        walkT_.push_back(t);
        walkC_.push_back(c);
        if (s >= walk_.length()) break;
    }
}

float Game::walkDistanceAt(float t) const {
    if (walkT_.empty() || t <= 0.0f) return 0.0f;
    if (t >= walkT_.back()) return walk_.length();
    size_t i = size_t(std::upper_bound(walkT_.begin(), walkT_.end(), t) - walkT_.begin());
    float f = (t - walkT_[i - 1]) / std::max(1e-6f, walkT_[i] - walkT_[i - 1]);
    return walkS_[i - 1] + (walkS_[i] - walkS_[i - 1]) * f;
}

float Game::walkCrouchAt(float s) const {
    if (walkS_.empty()) return 0.0f;
    float f = std::clamp(s / 0.05f, 0.0f, float(walkS_.size() - 1));
    size_t i = std::min(size_t(f), walkS_.size() - 1), j = std::min(i + 1, walkS_.size() - 1);
    return walkC_[i] + (walkC_[j] - walkC_[i]) * (f - float(i));
}

// ---------------------------------------------------------------------------
// The gun
// ---------------------------------------------------------------------------

Mat4 Game::gunMatrix() const {
    Vec3 R = camera_.right(), U = camera_.up(), F = camera_.forward();
    float sway = bobAmount_ * (1.0f - 0.5f * crouch_);
    // recoil: a sharp kick back and up, settling over a tenth of a second
    float k = time_ - lastShot_;
    float r = k < 0.0f ? 0.0f : k < 0.025f ? k / 0.025f : std::exp(-(k - 0.025f) / 0.08f);
    Vec3 pos = camera_.position + R * (0.125f + 0.006f * sway * std::sin(bob_)) +
               U * (-0.13f + 0.004f * sway * std::sin(2.0f * bob_)) + F * (0.34f - 0.04f * r);
    // model axes: +X right, +Y up, -Z forward
    Mat4 view = Mat4::basis(R, U, -F, pos);
    return view * Mat4::rotationY(0.05f) * Mat4::rotationX(0.03f + 0.16f * r);
}

Vec3 Game::muzzlePosition() const { return gunMatrix().point(muzzleLocal_); }

bool Game::fire() {
    if (time_ - lastShot_ < kFireInterval || gunModel_ < 0) return false;
    // aim where the crosshair is: find what the camera ray hits, then fly the bolt there
    // from the muzzle
    const Vec3 F = camera_.forward();
    Vec3 n;
    const float d = raycastLevel(maze_, level_, camera_.position, F, 80.0f, doorOpen_, n);
    const Vec3 target = camera_.position + F * d;
    lastShot_ = time_;
    ++shotsFired_;
    const Vec3 o = muzzlePosition();
    Vec3 to = target - o;
    const float dist = std::max(0.05f, length(to));
    bolts_.push_back({o, to * (1.0f / dist), time_, dist, n});
    camera_.pitch = std::min(1.35f, camera_.pitch + 0.012f);  // the kick nudges the aim up
    updateEffects();
    return true;
}

// ---------------------------------------------------------------------------
// Effects: bolts landing, spark bursts, steam, short-lived lights
// ---------------------------------------------------------------------------

void Game::updateEffects() {
    // bolts that have arrived become impacts and scorch marks
    for (size_t i = 0; i < bolts_.size();) {
        const Bolt& b = bolts_[i];
        const float arrive = b.t0 + b.dist / kBoltSpeed;
        if (time_ >= arrive) {
            const Vec3 at = b.origin + b.dir * b.dist;
            impacts_.push_back({at, b.normal, arrive, mix32(uint32_t(shotsFired_) * 2654435761u + uint32_t(i))});
            if (int(scorches_.size()) >= kMaxScorches) scorches_.erase(scorches_.begin());
            const uint32_t h = mix32(uint32_t(arrive * 1000.0f) + 17u);
            scorches_.push_back({at + b.normal * 0.006f, b.normal, unit(h) * 2.0f * kPi, 0.8f + 0.5f * unit(mix32(h))});
            bolts_.erase(bolts_.begin() + std::ptrdiff_t(i));
        } else {
            ++i;
        }
    }
    impacts_.erase(std::remove_if(impacts_.begin(), impacts_.end(),
                                  [&](const Impact& im) { return time_ - im.t0 > kSparkLife; }),
                   impacts_.end());

    dynamic_.clear();
    steam_.clear();
    nearEmitters_.clear();
    sparkCount_ = 0;
    const Vec3 cam = camera_.position;
    for (size_t i = 0; i < level_.emitters.size(); ++i) {
        const Emitter& e = level_.emitters[i];
        Vec3 d = e.position - cam;
        if (dot(d, d) > kEffectRange * kEffectRange) continue;
        nearEmitters_.push_back(int(i));
        if (e.kind == Emitter::Spark) {
            const uint32_t m0 = slotOf(time_ + e.phase * kSparkSlot, kSparkSlot);
            for (uint32_t m = m0 >= 3 ? m0 - 3 : 0; m <= m0; ++m) {
                float t0;
                uint32_t seed;
                if (!sparkBurst(uint32_t(i), m, e.phase, t0, seed)) continue;
                const float age = time_ - t0;
                if (age < 0.0f || age > kSparkLife) continue;
                ++sparkCount_;
                if (age < 0.16f) {
                    float f = 1.0f - age / 0.16f;
                    float flicker = 0.6f + 0.4f * unit(hash2(seed, uint32_t(age * 120.0f)));
                    dynamic_.push_back({e.position, Vec3{1.0f, 0.62f, 0.28f} * (7.0f * f * f * flicker), 0.03f, 7.0f});
                }
            }
        } else {
            const float s = steamStrength(uint32_t(i), time_, e.phase);
            if (s < 0.02f) continue;
            SteamPlume p;
            p.origin = e.position;
            p.direction = e.direction;
            p.length = 1.7f;
            p.strength = s;
            p.seed = e.phase * 100.0f;
            // light falling on the plume from the fixtures around it (current power)
            const int cx = int(std::floor(e.position.x / kCell + 0.5f)), cy = int(std::floor(e.position.z / kCell + 0.5f));
            Vec3 sum{};
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int x = cx + dx, y = cy + dy;
                    if (x < 0 || y < 0 || x >= level_.gridW || y >= level_.gridH) continue;
                    const size_t c = size_t(y * level_.gridW + x);
                    for (uint32_t k = 0; k < level_.cellCount[c]; ++k) {
                        const uint32_t li = level_.cellLights[level_.cellStart[c] + k];
                        const Vec3 to = level_.lights[li].position - e.position;
                        const float d2 = std::max(dot(to, to), 0.6f);
                        if (d2 > 81.0f) continue;
                        sum += lightRadiance_[li] * (1.0f / d2);
                    }
                }
            p.light = sum;
            steam_.push_back(p);
        }
    }
    // the gun: muzzle flash, bolts in flight, impacts
    const float k = time_ - lastShot_;
    if (k >= 0.0f && k < 0.07f) {
        const float f = 1.0f - k / 0.07f;
        dynamic_.push_back({muzzlePosition() + camera_.forward() * 0.12f, Vec3{0.55f, 0.85f, 1.0f} * (13.0f * f * f),
                            0.04f, 10.0f});
    }
    for (const Bolt& b : bolts_) {
        const float travelled = std::min(b.dist, (time_ - b.t0) * kBoltSpeed);
        dynamic_.push_back({b.origin + b.dir * travelled, Vec3{0.45f, 0.85f, 1.0f} * 5.0f, 0.02f, 6.0f});
    }
    for (const Impact& im : impacts_) {
        const float age = time_ - im.t0;
        ++sparkCount_;
        if (age < 0.14f) {
            const float f = 1.0f - age / 0.14f;
            dynamic_.push_back({im.position + im.normal * 0.15f, Vec3{0.6f, 0.9f, 1.0f} * (9.0f * f * f), 0.03f, 8.0f});
        }
    }
    // keep the brightest, closest few
    auto weight = [&](const DynamicLight& l) {
        Vec3 d = l.position - cam;
        return (l.radiance.x + l.radiance.y + l.radiance.z) / (1.0f + dot(d, d));
    };
    std::sort(dynamic_.begin(), dynamic_.end(),
              [&](const DynamicLight& a, const DynamicLight& b) { return weight(a) > weight(b); });
    if (int(dynamic_.size()) > kMaxDynamicLights) dynamic_.resize(size_t(kMaxDynamicLights));
}

void Game::addSparks(std::vector<SceneInstance>& out, Vec3 origin, Vec3 dir, float age, uint32_t seed, int count,
                     float speed, float spread) const {
    if (sparkModel_ < 0) return;
    const Vec3 g{0, -9.8f, 0};
    const Vec3 side = anyPerpendicular(dir), up = cross(dir, side);
    for (int k = 0; k < count; ++k) {
        const uint32_t h = hash2(seed, uint32_t(k));
        const float life = 0.3f + 0.75f * unit(h);
        if (age > life) continue;
        const float a = unit(mix32(h)) * 2.0f * kPi, r = spread * std::sqrt(unit(mix32(h + 1u)));
        Vec3 v = normalize(dir + side * (std::cos(a) * r) + up * (std::sin(a) * r)) *
                 (speed * (0.35f + 0.9f * unit(mix32(h + 2u))));
        Vec3 p0 = origin;
        float t = age;
        // one bounce off the floor
        const float disc = v.y * v.y + 19.6f * p0.y;
        const float tHit = p0.y > 0.0f ? (v.y + std::sqrt(std::max(0.0f, disc))) / 9.8f : 1e9f;
        if (t > tHit) {
            p0 = p0 + v * tHit + g * (0.5f * tHit * tHit);
            p0.y = 0.002f;
            Vec3 vh = v + g * tHit;
            v = {vh.x * 0.45f, -vh.y * 0.28f, vh.z * 0.45f};
            t -= tHit;
        }
        Vec3 p = p0 + v * t + g * (0.5f * t * t);
        p.y = std::max(p.y, 0.004f);
        Vec3 vel = v + g * t;
        float sp = length(vel);
        Vec3 fwd = sp > 1e-3f ? vel * (1.0f / sp) : dir;
        // a streak along its motion (the model is 7 cm along -Z)
        const float stretch = std::clamp(sp * 0.03f / 0.07f, 0.25f, 2.5f);
        // (x, y, -fwd) right-handed, so the winding (and back-face culling) is kept
        const Vec3 x = anyPerpendicular(fwd), y = cross(x, fwd);
        Mat4 m = Mat4::basis(x, y, fwd * -stretch, p);
        const float fade = 1.0f - age / life;
        const float e = 1.4f * fade * fade;
        for (uint32_t part : kit_.model(sparkModel_).parts) out.push_back({part, m, kInstanceFx, {e, e * 0.95f, e * 0.9f}});
    }
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
        Vec3 k = cellLight_[size_t(l.cell)];
        if (l.beacon) {
            // emergency beacons run on their own battery: a slow red pulse
            CellPower p{LightMode::Emergency, 1.0f, {1, 1, 1}, sec.power[size_t(l.cell)].phase};
            k = lightMultiplier(p, time_);
        }
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
    const Vec3 U = camera_.up(), F = camera_.forward();
    // under the barrel, just past the muzzle (it kicks with the gun), aimed where the
    // crosshair looks at ~8 m
    f.position = muzzlePosition() + F * 0.03f - U * 0.035f;
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

    // crouch: held key; and once under something low you cannot stand up until clear of it
    // (walking into a pipe standing up just stops you)
    Vec2 p{camera_.position.x, camera_.position.z};
    const float head = level_.collision.headroom(p, kPlayerRadius);
    const bool forced = crouch_ > 0.3f && head > 0.0f && head < kStandHeight + 0.02f;
    const float crouchTarget = (in.crouch || forced) ? 1.0f : 0.0f;
    crouch_ += (crouchTarget - crouch_) * std::min(1.0f, dt * 9.0f);
    const float height = kStandHeight + (kCrouchHeight - kStandHeight) * crouch_;

    float speed = (in.run && crouch_ < 0.5f ? 5.8f : 3.4f) * (1.0f - 0.5f * crouch_);
    Vec2 target = wl > 0 ? wish * (speed / wl) : Vec2{};
    float k = std::min(1.0f, dt * 12.0f);
    velocity_ = velocity_ + (target - velocity_) * k;

    Vec2 np = level_.collision.move(p, velocity_ * dt, kPlayerRadius, doorOpen_, height);
    float moved = length(np - p);
    // one vertical bob per footstep, at about two steps a second
    float spd = length(velocity_);
    float stepLength = std::max(0.6f, spd / stepRate(spd));
    bob_ += moved / stepLength * kPi;
    bobAmount_ += ((wl > 0 ? 1.0f : 0.0f) - bobAmount_) * std::min(1.0f, dt * 6.0f);
    float amp = (in.run ? 0.03f : 0.022f) * (1.0f - 0.5f * crouch_);
    Vec2 side = right * (0.012f * bobAmount_ * std::sin(bob_));
    const float eye = kEyeHeight + (kCrouchEye - kEyeHeight) * crouch_;
    camera_.position = {np.x + side.x, eye + amp * bobAmount_ * std::sin(2.0f * bob_), np.y + side.y};
    updateDoors(dt);
    time_ += dt;
    updateLighting();
    reveal();
    if (in.fire) fire();
    updateEffects();
}

float Game::walkDuration() const {
    return kStartPause + (walkT_.empty() ? walk_.length() / kWalkSpeed : walkT_.back()) + kEndPause;
}

void Game::setWalkTime(float t) {
    float s = walkDistanceAt(t - kStartPause);
    crouch_ = walkCrouchAt(s);
    Vec2 p = walk_.at(s);
    // average a few look-ahead directions so turns ease in
    Vec2 d{};
    // short look-aheads: turning the head too early stares into door jambs
    for (float la : {0.7f, 1.4f, 2.1f}) d += walk_.direction(s, la);
    camera_.yaw = std::atan2(-d.x, -d.y);
    camera_.pitch = -0.035f;
    // under a pipe the head comes down and looks a little up at it
    camera_.pitch = -0.035f + 0.06f * crouch_;
    float walking = (s > 0.0f && s < walk_.length()) ? 1.0f : 0.0f;
    bobAmount_ = walking;
    bob_ = s / (kWalkSpeed / stepRate(kWalkSpeed)) * kPi;
    Vec3 r = camera_.right();
    float side = 0.012f * walking * std::sin(bob_) * (1.0f - 0.5f * crouch_);
    const float eye = kEyeHeight + (kCrouchEye - kEyeHeight) * crouch_;
    camera_.position = {p.x + r.x * side, eye + 0.022f * walking * (1.0f - 0.5f * crouch_) * std::sin(2.0f * bob_),
                        p.y + r.z * side};
    time_ = t;
    updateLighting();
    reveal();
    updateEffects();
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
            if (pm.selfPowered) {
                const float ph = pm.cell >= 0 ? level_.sections.power[size_t(pm.cell)].phase : 0.0f;
                e = emissiveMultiplier(CellPower{LightMode::Emergency, 1.0f, {1, 1, 1}, ph}, time_);
            }
            out.push_back({part, t, 0, e});
        }
    }
    // scorch marks where bolts hit
    if (scorchModel_ >= 0)
        for (const Scorch& s : scorches_) {
            const Vec3 n = s.normal;
            Vec3 x = anyPerpendicular(n);
            Vec3 z = cross(x, n);
            const float c = std::cos(s.roll), sn = std::sin(s.roll);
            const Vec3 xr = x * c + z * sn, zr = z * c - x * sn;
            // the decal faces model +Y
            Mat4 m = Mat4::basis(xr * s.size, n, zr * s.size, s.position);
            for (uint32_t part : kit_.model(scorchModel_).parts) out.push_back({part, m, 0});
        }
    // sparks: from live wires and broken fixtures, and where bolts hit
    for (int i : nearEmitters_) {
        const Emitter& e = level_.emitters[size_t(i)];
        if (e.kind != Emitter::Spark) continue;
        const uint32_t m0 = slotOf(time_ + e.phase * kSparkSlot, kSparkSlot);
        for (uint32_t m = m0 >= 3 ? m0 - 3 : 0; m <= m0; ++m) {
            float t0;
            uint32_t seed;
            if (!sparkBurst(uint32_t(i), m, e.phase, t0, seed)) continue;
            const float age = time_ - t0;
            if (age < 0.0f || age > kSparkLife) continue;
            addSparks(out, e.position, e.direction, age, seed, 16, 2.4f, 0.85f);
        }
    }
    for (const Impact& im : impacts_) addSparks(out, im.position, im.normal, time_ - im.t0, im.seed, 18, 3.6f, 0.75f);
    // bolts in flight: a glowing streak, stretched along its path
    if (boltModel_ >= 0)
        for (const Bolt& b : bolts_) {
            const float travelled = std::min(b.dist, (time_ - b.t0) * kBoltSpeed);
            const float len = std::min(1.2f, std::max(0.1f, travelled)) / 0.5f;  // model is 0.5 m long
            const Vec3 c = b.origin + b.dir * std::max(0.0f, travelled - 0.25f * len);
            const Vec3 x = anyPerpendicular(b.dir), y = cross(x, b.dir);
            Mat4 m = Mat4::basis(x * 1.8f, y * 1.8f, b.dir * -len, c);  // a fatter streak than the model
            for (uint32_t part : kit_.model(boltModel_).parts) out.push_back({part, m, kInstanceFx});
        }
    if (withGun && gunModel_ >= 0) {
        const Mat4 t = gunMatrix();
        for (uint32_t part : kit_.model(gunModel_).parts) out.push_back({part, t, kInstanceGun});
        if (armsModel_ >= 0)
            for (uint32_t part : kit_.model(armsModel_).parts) out.push_back({part, t, kInstanceGun});
        const float k = time_ - lastShot_;
        if (flashModel_ >= 0 && k >= 0.0f && k < 0.05f) {
            // a different roll and size each shot
            const uint32_t h = mix32(uint32_t(shotsFired_) * 2246822519u);
            const float roll = unit(h) * 2.0f * kPi, sc = 0.85f + 0.5f * unit(mix32(h));
            Mat4 local = Mat4::translation(muzzleLocal_) * Mat4::rotationZ(roll);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) local.m[r][c] *= sc;
            local = local * Mat4::translation(-muzzleLocal_);
            const float f = 1.0f - k / 0.05f;
            for (uint32_t part : kit_.model(flashModel_).parts)
                out.push_back({part, t * local, kInstanceGun, {f, f, f}});
        }
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

    // derelict features (round 6): the first of each, preferring ones on the walk route
    std::vector<char> onRoute(size_t(m.width() * m.height()), 0);
    for (Int2 c : level_.route) onRoute[size_t(c.y * m.width() + c.x)] = 1;
    auto findPlaced = [&](std::string_view name) -> const PlacedModel* {
        const PlacedModel* any = nullptr;
        for (const PlacedModel& pm : level_.placed) {
            if (kit_.model(pm.model).name != name) continue;
            if (pm.cell >= 0 && onRoute[size_t(pm.cell)]) return &pm;
            if (!any) any = &pm;
        }
        return any;
    };
    auto lookAt = [](Vec3 eye, Vec3 target) {
        Camera c;
        c.position = eye;
        Vec3 d = target - eye;
        c.yaw = std::atan2(-d.x, -d.z);
        c.pitch = std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z));
        return c;
    };
    if (const PlacedModel* pm = findPlaced("corridor_straight_broken")) {
        // stand in the cell and look along it at the torn-open ceiling
        const Vec3 c = pm->transform.point({0, 0, 0});
        const Vec3 along = normalize(pm->transform.dir({0, 0, -1}));
        shots.push_back({"broken_ceiling", lookAt(c - along * 1.7f + Vec3{0, kEyeHeight, 0},
                                                  c + along * 1.6f + Vec3{0, 2.3f, 0})});
    }
    for (const Emitter& e : level_.emitters) {
        if (e.kind != Emitter::Steam) continue;
        // from 2.6 m down the corridor, looking at the torn pipe
        Vec3 side{e.direction.x, 0, e.direction.z};
        side = normalize(side);
        const Vec3 along{-side.z, 0, side.x};
        const Vec3 cell = cellCenter(e.cell % m.width(), e.cell / m.width());
        shots.push_back({"steam", lookAt(cell + along * 2.6f + Vec3{0, kEyeHeight, 0}, e.position + side * 0.5f)});
        break;
    }
    if (!level_.lowPipes.empty()) {
        const LowPipe& lp = level_.lowPipes.front();
        shots.push_back({"low_pipe", lookAt(lp.center - lp.along * 3.2f + Vec3{0, kEyeHeight, 0},
                                            lp.center + Vec3{0, 1.35f, 0})});
    }
    if (const PlacedModel* pm = findPlaced("beacon")) {
        const Vec3 lamp = pm->transform.point({1.1f, 2.08f, 0.0f});
        const Vec3 c = pm->transform.point({0, 0, 0});
        const Vec3 along = normalize(pm->transform.dir({0, 0, 1}));
        shots.push_back({"beacon", lookAt(c + along * 2.4f + Vec3{0, kEyeHeight, 0}, lamp)});
    }
    return shots;
}

}  // namespace bh
