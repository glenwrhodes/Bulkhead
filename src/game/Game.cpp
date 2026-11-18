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
constexpr int kMaxDynamicLights = 16;          // with the light caps (before round 18)
constexpr int kMaxDynamicLightsUncapped = 128; // the renderer's buffer

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

Int2 cellOf(Vec3 p) { return {int(std::floor(p.x / kCell + 0.5f)), int(std::floor(p.z / kCell + 0.5f))}; }

Vec3 anyPerpendicular(Vec3 d) {
    Vec3 ref = std::abs(d.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    return normalize(cross(ref, d));
}

}  // namespace

Game::Game(const Kit& kit, const MazeParams& params)
    : kit_(kit), maze_(Maze::generate(params)), level_(buildLevel(maze_, kit)), walk_(level_.route), creature_(kit) {
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
    sway_ = {};
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
    if (playerDead()) return false;
    const Vec3 F = camera_.forward();
    Vec3 n;
    float d = raycastLevel(maze_, level_, camera_.position, F, 80.0f, doorOpen_, n);
    // a creature in the way?
    int target_hunter = -1;
    for (size_t i = 0; i < hunters_.size(); ++i) {
        float t;
        Vec3 hn;
        if (hunters_[i].raycast(camera_.position, F, d, t, hn) && t < d) {
            d = t;
            n = hn;
            target_hunter = int(i);
        }
    }
    const Vec3 target = camera_.position + F * d;
    lastShot_ = time_;
    ++shotsFired_;
    shotThisStep_ = true;
    const Vec3 o = muzzlePosition();
    Vec3 to = target - o;
    const float dist = std::max(0.05f, length(to));
    bolts_.push_back({o, to * (1.0f / dist), time_, dist, n, target_hunter});
    sound(Snd::GunFire, o, 0.8f, 0.96f + 0.08f * unit(mix32(uint32_t(shotsFired_) * 7919u)), false);
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
            const bool onCreature = b.hunter >= 0 && size_t(b.hunter) < hunters_.size();
            impacts_.push_back({at, b.normal, arrive, mix32(uint32_t(shotsFired_) * 2654435761u + uint32_t(i)),
                                onCreature});
            if (onCreature) {
                Hunter& h = hunters_[size_t(b.hunter)];
                const bool wasAlive = h.alive();
                h.hit(huntWorld(true));
                if (wasAlive && !h.alive()) ++kills_;
                sound(Snd::HitCreature, at, 1.0f);
                if (h.alive()) sound(Snd::CreaturePain, h.headPosition(), 1.0f, 0.95f + 0.1f * unit(mix32(uint32_t(i) + 3u)));
            } else {
                sound(Snd::HitMetal, at, 0.75f, 0.9f + 0.2f * unit(mix32(uint32_t(arrive * 997.0f))));
                if (int(scorches_.size()) >= kMaxScorches) scorches_.erase(scorches_.begin());
                const uint32_t h = mix32(uint32_t(arrive * 1000.0f) + 17u);
                scorches_.push_back(
                    {at + b.normal * 0.006f, b.normal, unit(h) * 2.0f * kPi, 0.8f + 0.5f * unit(mix32(h))});
            }
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
            // a hit on the creature bursts amber, like its glow
            const Vec3 col = im.creature ? Vec3{1.0f, 0.5f, 0.12f} * 1.6f : Vec3{0.6f, 0.9f, 1.0f};
            dynamic_.push_back({im.position + im.normal * 0.15f, col * (9.0f * f * f), 0.03f, 8.0f});
        }
    }
    // each hunter's glow lights its surroundings a little (the throat and seams). With the
    // light caps (the lighting before round 18, when every light in range cost shadow rays on
    // every pixel it reached): only the three nearest within 30 m. Without them (sampled
    // lighting: the cost no longer grows with the lights): every one within 60 m.
    std::vector<std::pair<float, size_t>> lit;
    const float litRange = lightCaps_ ? 30.0f : 60.0f;
    for (size_t i = 0; i < hunters_.size(); ++i) {
        const Hunter& h = hunters_[i];
        if (h.state() == HunterState::Dead || h.glow() < 0.05f) continue;
        const Vec3 d = h.headPosition() - cam;
        if (dot(d, d) <= litRange * litRange) lit.push_back({dot(d, d), i});
    }
    std::sort(lit.begin(), lit.end());
    if (lightCaps_ && lit.size() > 3) lit.resize(3);
    for (const auto& near : lit) {
        const Hunter& h = hunters_[near.second];
        dynamic_.push_back({h.headPosition(), Vec3{1.0f, 0.45f, 0.1f} * (0.9f * h.glow()), 0.08f, 5.0f});
        // and the seams along its back light the wall or ceiling it is on: in the dark
        // it shows as a shape against its own amber glow
        if (h.alive()) {
            const Mat4 b = h.creature().bodyMatrix();
            const Vec3 up = normalize(b.dir({0, 1, 0}));
            dynamic_.push_back({h.bodyCenter() + up * 0.45f, Vec3{1.0f, 0.42f, 0.08f} * (1.6f * h.glow()), 0.15f, 5.5f});
        }
    }
    if (creatureActive())
        dynamic_.push_back({creature_.headPosition(), Vec3{1.0f, 0.45f, 0.1f} * 1.2f, 0.08f, 5.0f});
    // keep the brightest, closest few
    auto weight = [&](const DynamicLight& l) {
        Vec3 d = l.position - cam;
        return (l.radiance.x + l.radiance.y + l.radiance.z) / (1.0f + dot(d, d));
    };
    std::sort(dynamic_.begin(), dynamic_.end(),
              [&](const DynamicLight& a, const DynamicLight& b) { return weight(a) > weight(b); });
    const int most = lightCaps_ ? kMaxDynamicLights : kMaxDynamicLightsUncapped;
    if (int(dynamic_.size()) > most) dynamic_.resize(size_t(most));
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
    if (playerDead()) {
        // no control while it has you; then the run starts over
        stepWorld(dt);
        if (deathT_ > kDeathLength) restart();
        return;
    }
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
    // where the player actually stands: the camera minus the sideways sway drawn last
    // step. (The sway is only a camera offset; feeding it back into the position made it
    // add up every step: a ~0.35 m weave at 100 fps, worse at higher frame rates, and a
    // steady slide into the wall while pressing against something.)
    Vec2 p{camera_.position.x - sway_.x, camera_.position.z - sway_.y};
    const float head = level_.collision.headroom(p, kPlayerRadius);
    const bool forced = crouch_ > 0.3f && head > 0.0f && head < kStandHeight + 0.02f;
    const float crouchTarget = (in.crouch || forced) ? 1.0f : 0.0f;
    // (easing as 1 - e^(-rate dt): the same result at any frame rate)
    crouch_ += (crouchTarget - crouch_) * (1.0f - std::exp(-9.0f * dt));
    const float height = kStandHeight + (kCrouchHeight - kStandHeight) * crouch_;

    float speed = (in.run && crouch_ < 0.5f ? 5.8f : 3.4f) * (1.0f - 0.5f * crouch_);
    // footsteps the creatures can hear: running is loud, creeping crouched is silent
    noise_ = wl <= 0.0f || crouch_ > 0.5f ? 0 : in.run ? 2 : 1;
    Vec2 target = wl > 0 ? wish * (speed / wl) : Vec2{};
    float k = 1.0f - std::exp(-12.0f * dt);
    velocity_ = velocity_ + (target - velocity_) * k;

    Vec2 np = level_.collision.move(p, velocity_ * dt, kPlayerRadius, doorOpen_, height);
    float moved = length(np - p);
    // one vertical bob per footstep, at about two steps a second
    float spd = length(velocity_);
    float stepLength = std::max(0.6f, spd / stepRate(spd));
    bob_ += moved / stepLength * kPi;
    bobAmount_ += ((wl > 0 ? 1.0f : 0.0f) - bobAmount_) * (1.0f - std::exp(-6.0f * dt));
    float amp = (in.run ? 0.03f : 0.022f) * (1.0f - 0.5f * crouch_);
    Vec2 side = right * (0.012f * bobAmount_ * std::sin(bob_));
    const float eye = kEyeHeight + (kCrouchEye - kEyeHeight) * crouch_;
    camera_.position = {np.x + side.x, eye + amp * bobAmount_ * std::sin(2.0f * bob_), np.y + side.y};
    sway_ = side;
    if (in.fire) fire();
    stepWorld(dt);
}

// ---------------------------------------------------------------------------
// Hunters and the player's death
// ---------------------------------------------------------------------------

HuntWorld Game::huntWorld(bool shot) const {
    HuntWorld w;
    w.maze = &maze_;
    w.level = &level_;
    w.doorOpen = &doorOpen_;
    w.player = camera_.position;
    w.playerForward = camera_.forward();
    w.playerAlive = !playerDead() && !huntersIgnorePlayer_;
    w.shotFired = shot;
    w.grabbable = !invulnerable_;
    w.time = time_;
    w.tuning = &tuning_;
    w.playerDist = playerDist_.empty() ? nullptr : &playerDist_;
    w.noiseRange = noise_ >= 2 ? tuning_.hearRun : noise_ == 1 ? tuning_.hearWalk : 0.0f;
    return w;
}

void Game::movePlayer(const Camera& c) {
    camera_ = c;
    sway_ = {};
}

void Game::spawnHunterAt(Int2 cell, int surface, uint64_t seed) {
    Hunter h(kit_, seed);
    if (!h.creature().valid()) return;
    h.spawn(maze_, cell, surface);
    hunters_.push_back(std::move(h));
    respawnAt_.push_back(-1.0f);
}

void Game::spawnHunters(int count, uint64_t salt) {
    huntersWanted_ = count;
    spawnSalt_ = salt;
    hunters_.clear();
    respawnAt_.clear();
    const std::vector<int> dist = maze_.distancesFrom(cellOf(camera_.position));
    Rng rng(maze_.params().seed ^ (0x51ed27u + spawnCounter_ * 7919u) ^ (salt * 0x9e3779b97f4a7c15ull), 0xc0ffeeu);
    std::vector<Int2> picked;
    for (int attempt = 0; attempt < 4000 && int(picked.size()) < count; ++attempt) {
        Int2 c{rng.range(0, maze_.width() - 1), rng.range(0, maze_.height() - 1)};
        if (maze_.kind(c) != CellKind::Corridor) continue;
        const int d = dist[size_t(c.y * maze_.width() + c.x)];
        if (d < 12) continue;  // never right on top of the player
        if (level_.sections.moodAt(c) == Mood::Lit && !rng.chance(0.1f)) continue;
        // spread out (a little closer together when there are many, so they all fit)
        const int minApart = count <= 3 ? 8 : 6;
        bool apart = true;
        for (Int2 o : picked) apart = apart && std::abs(o.x - c.x) + std::abs(o.y - c.y) >= minApart;
        if (!apart) continue;
        picked.push_back(c);
    }
    for (Int2 c : picked) spawnHunterAt(c, 2, maze_.params().seed * 31u + (spawnCounter_++) + salt * 1000003u);
}

void Game::restart() {
    deathT_ = -1.0f;
    killer_ = -1;
    sway_ = {};
    camera_ = Camera{};
    camera_.position = level_.spawn;
    camera_.yaw = level_.spawnYaw;
    velocity_ = {};
    crouch_ = 0.0f;
    bolts_.clear();
    impacts_.clear();
    std::fill(doorOpen_.begin(), doorOpen_.end(), 0.0f);
    scanner_.reset();
    scanPrev_.clear();
    if (huntersWanted_ > 0) spawnHunters(huntersWanted_, spawnSalt_);
}

float Game::redout() const {
    if (!playerDead()) {
        // a hint of red as one closes in
        float r = 0.0f;
        for (const Hunter& h : hunters_)
            if (h.state() == HunterState::Chase || h.state() == HunterState::Leap || h.state() == HunterState::Threat ||
                h.state() == HunterState::Coil)
                r = std::max(r, 0.25f * (1.0f - std::clamp(length(h.headPosition() - camera_.position) / 8.0f, 0.0f, 1.0f)));
        return r;
    }
    return std::clamp(0.12f + deathT_ / 2.6f, 0.0f, 1.0f);
}

float Game::blackout() const { return playerDead() ? std::clamp((deathT_ - 2.4f) / 1.4f, 0.0f, 1.0f) : 0.0f; }

void Game::stepCreatures(float dt) {
    stepHunters(dt);
    updateEffects();
}

void Game::stepWorld(float dt) {
    time_ += dt;
    updateDoors(dt);
    updateLighting();
    stepHunters(dt);
    if (playerDead()) {
        deathT_ += dt;
        // knocked back and down onto the floor, looking up at it
        const float fall = std::clamp(deathT_ / 0.7f, 0.0f, 1.0f);
        const float e = 1.0f - (1.0f - fall) * (1.0f - fall);
        Camera c = deathCam_;
        const Vec3 back = normalize(Vec3{-deathCam_.forward().x, 0, -deathCam_.forward().z});
        c.position = deathCam_.position + back * (0.55f * e) + Vec3{0, (0.38f - deathCam_.position.y) * e, 0};
        c.pitch = deathCam_.pitch + (0.55f - deathCam_.pitch) * e;
        // the struggle: shaking, worst at the impact
        const float shake = (deathT_ < 0.3f ? 0.06f : 0.025f) * std::exp(-deathT_ * 0.5f);
        c.yaw += shake * std::sin(time_ * 37.0f);
        c.pitch += shake * std::sin(time_ * 29.0f + 1.0f);
        camera_ = c;
        if (killer_ >= 0 && size_t(killer_) < hunters_.size()) hunters_[size_t(killer_)].poseOnFace(camera_, deathT_, time_);
    }
    reveal();
    updateEffects();
}

void Game::stepHunters(float dt) {
    // hearing needs walking distances from the player's cell
    if (!hunters_.empty() && (tuning_.hearWalk > 0.0f || tuning_.hearRun > 0.0f)) {
        const Int2 pc = cameraCell();
        if (pc != playerDistFrom_ && maze_.inBounds(pc)) {
            playerDist_ = maze_.distancesFrom(pc);
            playerDistFrom_ = pc;
        }
    }
    const HuntWorld w = huntWorld(shotThisStep_);
    shotThisStep_ = false;
    for (size_t i = 0; i < hunters_.size(); ++i) {
        Hunter& h = hunters_[i];
        h.update(dt, w);
        if (h.grabbedPlayer() && !playerDead()) {
            deathT_ = 0.0f;
            killer_ = int(i);
            deathCam_ = camera_;
        }
        // the dead are replaced after a while, somewhere far away (window play); on the
        // benchmark, so is one that reaches the player (as if it had been killed)
        const bool replace = (h.state() == HunterState::Dead && h.deadFor() > 30.0f && huntersWanted_ > 0) ||
                             (invulnerable_ && h.reachedPlayer() > 0);
        if (replace && respawnAt_[i] < 0.0f) {
            respawnAt_[i] = time_;
            const std::vector<int> dist = maze_.distancesFrom(cellOf(camera_.position));
            Rng rng(uint64_t(time_ * 1000.0f) + i, 0xbeefu);
            for (int attempt = 0; attempt < 2000; ++attempt) {
                Int2 c{rng.range(0, maze_.width() - 1), rng.range(0, maze_.height() - 1)};
                if (maze_.kind(c) != CellKind::Corridor || dist[size_t(c.y * maze_.width() + c.x)] < 14) continue;
                Hunter fresh(kit_, maze_.params().seed * 31u + (spawnCounter_++));
                fresh.spawn(maze_, c, 2);
                h = std::move(fresh);
                respawnAt_[i] = -1.0f;
                break;
            }
        }
    }
    updateScanner(dt);
}

void Game::updateScanner(float dt) {
    if (!scannerActive()) return;
    std::vector<Scanner::Contact> contacts(hunters_.size());
    if (scanPrev_.size() != hunters_.size()) scanPrev_.assign(hunters_.size(), Vec3{1e9f, 0, 0});
    for (size_t i = 0; i < hunters_.size(); ++i) {
        const Hunter& h = hunters_[i];
        const Vec3 p = h.bodyCenter();
        const float moved = length(p - scanPrev_[i]);
        // (a respawn far away is not movement)
        const float speed = dt > 0.0f && moved < 3.0f ? moved / dt : 0.0f;
        const bool live = h.alive() && h.state() != HunterState::Feed;
        contacts[i] = {p, live ? speed : 0.0f};
        scanPrev_[i] = p;
    }
    scanner_.update(time_, camera_.position, contacts);
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
    sway_ = {r.x * side, r.z * side};
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
        auto near = [&](Vec3 p) {
            float cx = p.x - c.x, cz = p.z - c.z;
            return cx * cx + cz * cz < kDoorTrigger * kDoorTrigger;
        };
        if (creatureActive())  // doors open for the creature too
            want = want || near(creature_.headPosition()) || near(creature_.bodyCenter());
        for (const Hunter& h : hunters_)
            if (h.alive()) want = want || near(h.headPosition()) || near(h.bodyCenter());
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
    for (const Impact& im : impacts_)
        addSparks(out, im.position, im.normal, time_ - im.t0, im.seed, im.creature ? 26 : 18, 3.6f, 0.75f);
    for (const Hunter& h : hunters_) h.appendInstances(out);
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
    if (creatureActive()) creature_.appendInstances(out);
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

// ---------------------------------------------------------------------------
// The crawl scene
// ---------------------------------------------------------------------------

bool Game::setupCrawlScene() {
    creatureOn_ = false;
    if (!creature_.valid()) return false;
    const Maze& m = maze_;
    auto step = [](Int2 c, int d) { return Int2{c.x + kDx[d], c.y + kDy[d]}; };
    auto straightAlong = [&](Int2 c, int d) {
        if (!m.inBounds(c) || m.kind(c) != CellKind::Corridor) return false;
        return m.passable(c.x, c.y, Dir(d)) && m.passable(c.x, c.y, Dir((d + 2) & 3)) &&
               !m.passable(c.x, c.y, Dir((d + 1) & 3)) && !m.passable(c.x, c.y, Dir((d + 3) & 3));
    };
    // the best long straight corridor: long, and not in a pitch-dark section (the
    // flashlight is the only light there), so the creature reads on film
    int bestScore = -1, bestD = 0;
    std::vector<Int2> best;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            for (int d = 0; d < 4; ++d) {
                Int2 c{x, y};
                if (!straightAlong(c, d) || straightAlong(step(c, (d + 2) & 3), d)) continue;  // start of a run
                std::vector<Int2> run;
                while (straightAlong(c, d)) {
                    run.push_back(c);
                    c = step(c, d);
                }
                if (run.size() < 5) continue;
                const Mood mood = level_.sections.moodAt(run[run.size() / 2]);
                // dim corridors suit it best (some light, deep shadows), then derelict; fully
                // lit ones flatten it, dark ones hide it
                int score = int(std::min<size_t>(run.size(), 9)) * 2 +
                            (mood == Mood::Dim ? 30 : mood == Mood::Derelict ? 22 : mood == Mood::Dark ? 8 : 0);
                // no low pipes in it (the creature would crawl through them); broken ceilings,
                // burst pipes and debris it would crawl through count against it
                for (Int2 r : run) {
                    for (const LowPipe& lp : level_.lowPipes)
                        if (std::abs(lp.center.x - cellCenter(r).x) < 2.1f && std::abs(lp.center.z - cellCenter(r).z) < 2.1f)
                            score = -1000;
                    const int cell = r.y * m.width() + r.x;
                    for (const PlacedModel& pm : level_.placed) {
                        if (pm.cell != cell) continue;
                        const std::string& nm = kit_.model(pm.model).name;
                        if (nm == "corridor_straight_broken") score -= 14;
                        else if (nm == "pipe_burst" || nm == "debris" || nm == "beacon") score -= 5;
                    }
                }
                if (score > bestScore) {
                    bestScore = score;
                    best = run;
                    bestD = d;
                }
            }
    if (best.empty()) return false;
    // the camera stands near run[0] looking along d; the creature comes from beyond the far end
    const int d = bestD;
    const Vec3 dv = dirVector(Dir(d));
    const Int2 farCell = best.back();
    const Int2 beyond = step(farCell, d);
    // an approach from beyond the far end: prefer a room (through its door), else a few cells round a corner
    std::vector<int> parent(size_t(m.width() * m.height()), -2);
    std::vector<int> depth(parent.size(), 0);
    auto idx = [&](Int2 c) { return c.y * m.width() + c.x; };
    std::vector<char> inRun(parent.size(), 0);
    for (Int2 r : best) inRun[size_t(idx(r))] = 1;
    std::vector<Int2> queue{beyond};
    parent[size_t(idx(beyond))] = -1;
    Int2 start = beyond;
    bool foundRoom = false;
    for (size_t qi = 0; qi < queue.size() && !foundRoom; ++qi) {
        const Int2 c = queue[qi];
        if (m.kind(c) == CellKind::Room && depth[size_t(idx(c))] >= 1) {
            start = c;
            foundRoom = true;
            break;
        }
        if (depth[size_t(idx(c))] >= 5) continue;
        if (depth[size_t(idx(c))] >= 3 && !foundRoom && start == beyond) start = c;
        for (int k = 0; k < 4; ++k) {
            if (!m.passable(c.x, c.y, Dir(k))) continue;
            const Int2 n = step(c, k);
            if (inRun[size_t(idx(n))] || parent[size_t(idx(n))] != -2) continue;
            parent[size_t(idx(n))] = idx(c);
            depth[size_t(idx(n))] = depth[size_t(idx(c))] + 1;
            queue.push_back(n);
        }
    }
    std::vector<Int2> cells;
    for (int i = idx(start); i >= 0; i = parent[size_t(i)]) cells.push_back({i % m.width(), i / m.width()});
    // (start .. beyond), then the run from the far end to the camera's end, then behind it
    for (auto it = best.rbegin(); it != best.rend(); ++it) cells.push_back(*it);
    Int2 behind = best.front();
    for (int k = 0; k < 2; ++k) {
        if (!m.passable(behind.x, behind.y, Dir((d + 2) & 3))) break;
        behind = step(behind, (d + 2) & 3);
        cells.push_back(behind);
    }
    crawlPath_ = CrawlPath(cells);
    crawl_ = CrawlScene{};
    crawl_.run = best;
    // stand about four cells from the far end, so the whole climb happens close enough to read
    const size_t camCell = best.size() > 5 ? best.size() - 4 : 1;
    crawl_.cameraPos = cellCenter(best[camCell]) - dv * 1.4f + Vec3{0, kEyeHeight, 0};
    crawl_.farPoint = cellCenter(farCell) + Vec3{0, 1.2f, 0};
    crawl_.sRunStart = crawlPath_.project(cellCenter(farCell) + dv * 2.0f);
    crawl_.sCam = crawlPath_.project(crawl_.cameraPos);
    // the run splits into floor, wall (with a pause) and ceiling, ending overhead
    const float D = crawl_.sCam - crawl_.sRunStart;
    crawl_.sWall0 = crawl_.sRunStart + 0.28f * D;
    crawl_.sCeil0 = crawl_.sRunStart + 0.66f * D - 1.3f;
    crawl_.sPause = 0.5f * (crawl_.sWall0 + 2.0f + crawl_.sCeil0);
    // it starts just round the far end, so it's on screen within a second or two
    crawl_.s0 = std::max(0.0f, crawl_.sRunStart - 1.5f);
    // it ends with the creature right overhead, its tail sweeping past (the camera can't
    // follow it behind without flipping)
    crawl_.sEnd = std::min(crawlPath_.length() + 1.0f, crawl_.sCam + 0.6f);
    // distance -> time: quick on the floor, slower climbing, a near-stop on the wall
    // (it has noticed you), fast across the ceiling
    float t = 0.0f;
    for (float s = crawl_.s0; s <= crawl_.sEnd + 1e-3f; s += 0.02f) {
        float v = 1.3f;
        if (s > crawl_.sWall0) v = 1.0f;
        if (s > crawl_.sWall0 + 2.0f) v = 1.3f;
        const float dp = std::abs(s - crawl_.sPause);
        if (dp < 0.7f) v = 0.25f + (1.3f - 0.25f) * (dp / 0.7f) * (dp / 0.7f);
        if (s > crawl_.sCeil0) v = 1.5f;
        if (s > crawl_.sCeil0 + 2.6f) v = 2.1f;
        crawl_.tableS.push_back(s);
        crawl_.tableT.push_back(t);
        t += 0.02f / v;
    }
    crawl_.duration = crawl_.tableT.back() + 0.6f;
    creatureOn_ = true;
    return true;
}

float Game::crawlU(float s) const {
    const CrawlProfile& pr = crawlPath_.profile();
    const float wall = pr.rightWallU(), ceil = pr.ceilingU();
    auto ease = [](float a, float b, float x) { return smooth01(a, b, x); };
    float u = wall * ease(crawl_.sWall0, crawl_.sWall0 + 2.0f, s);
    u += (ceil - wall) * ease(crawl_.sCeil0, crawl_.sCeil0 + 2.6f, s);
    return u;
}

float Game::crawlS(float t) const {
    const auto& T = crawl_.tableT;
    const auto& S = crawl_.tableS;
    if (T.empty()) return 0.0f;
    if (t <= 0.0f) return S.front();
    if (t >= T.back()) return S.back();
    size_t i = size_t(std::upper_bound(T.begin(), T.end(), t) - T.begin());
    float f = (t - T[i - 1]) / std::max(1e-6f, T[i] - T[i - 1]);
    return S[i - 1] + (S[i] - S[i - 1]) * f;
}

CreaturePose Game::crawlPose(float t) const {
    CreaturePose p;
    p.s = crawlS(t);
    p.uOf = [this](float s) { return crawlU(s); };
    p.time = t;
    // it hisses at you from the wall, and again right overhead
    const float pause = 1.0f - smooth01(0.3f, 1.2f, std::abs(p.s - crawl_.sPause));
    const float head = p.s + 1.0f;
    const float overhead = smooth01(crawl_.sCam - 4.0f, crawl_.sCam - 1.5f, head) *
                           (1.0f - smooth01(crawl_.sCam + 1.0f, crawl_.sCam + 2.5f, head));
    const float flutter = 0.85f + 0.15f * std::sin(t * 23.0f);
    p.jaw = std::max(0.55f * pause, 0.85f * overhead) * flutter;
    p.look = pause > 0.05f || overhead > 0.05f;
    p.lookAt = crawl_.cameraPos;
    // it is hunting: the seams burn bright, brighter still when it stops to look
    p.glow = 0.85f + 0.6f * std::max(pause, overhead) + 0.1f * std::sin(t * 2.3f);
    return p;
}

void Game::setCrawlTime(float t) {
    if (!creatureOn_) return;
    creature_.pose(crawlPath_, crawlPose(t));
    // the camera: stands still and follows the creature's head with a short lag; until
    // the creature comes into the corridor it watches the far end
    const float tl = std::max(0.0f, t - 0.15f);
    const float sl = crawlS(tl);
    const float sh = sl + 1.0f;  // roughly where the head is
    const float uh = crawlU(sh);
    Vec3 head = crawlPath_.point(sh, uh) + crawlPath_.normal(sh, uh) * 0.55f;
    const float w = smooth01(crawl_.sRunStart - 1.0f, crawl_.sRunStart + 2.5f, sh);
    Vec3 aim = crawl_.farPoint + (head - crawl_.farPoint) * w;
    // once it is overhead, keep looking up rather than whipping round behind
    const Vec3 dv = normalize(crawl_.farPoint - crawl_.cameraPos);
    Vec3 rel = aim - crawl_.cameraPos;
    const Vec3 flat = normalize(Vec3{dv.x, 0, dv.z});
    float along = dot(rel, flat);
    if (along < 0.7f) rel = rel + flat * (0.7f - along);
    Camera c;
    c.position = crawl_.cameraPos;
    c.yaw = std::atan2(-rel.x, -rel.z);
    c.pitch = std::clamp(std::atan2(rel.y, std::sqrt(rel.x * rel.x + rel.z * rel.z)), -0.4f, 1.42f);
    camera_ = c;
    sway_ = {};
    time_ = t;
    updateLighting();
    reveal();
    updateEffects();
}

// ---------------------------------------------------------------------------
// The hunt and fight scenes: the real AI, a scripted player
// ---------------------------------------------------------------------------

bool Game::setupHuntScene(bool fight, bool approach, bool threatShot) {
    if (threatShot) approach = true;
    if (approach) fight = true;  // (the same corridor choice: enough light to see it come)
    const Maze& m = maze_;
    auto step = [](Int2 c, int d) { return Int2{c.x + kDx[d], c.y + kDy[d]}; };
    auto straightAlong = [&](Int2 c, int d) {
        if (!m.inBounds(c) || m.kind(c) != CellKind::Corridor) return false;
        return m.passable(c.x, c.y, Dir(d)) && m.passable(c.x, c.y, Dir((d + 2) & 3)) &&
               !m.passable(c.x, c.y, Dir((d + 1) & 3)) && !m.passable(c.x, c.y, Dir((d + 3) & 3));
    };
    int bestScore = -100000, bestD = 0;
    std::vector<Int2> best;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            for (int d = 0; d < 4; ++d) {
                Int2 c{x, y};
                if (!straightAlong(c, d) || straightAlong(step(c, (d + 2) & 3), d)) continue;
                std::vector<Int2> run;
                while (straightAlong(c, d)) {
                    run.push_back(c);
                    c = step(c, d);
                }
                if (run.size() < 5) continue;
                const Mood mood = level_.sections.moodAt(run[run.size() / 2]);
                int score = int(std::min<size_t>(run.size(), 8)) * 3;
                // hunt: the dark, the flashlight and its glow; fight: enough light to see it die
                if (threatShot) score += mood == Mood::Lit ? 30 : mood == Mood::Dim ? 15 : 0;  // (no sparks)
                else if (fight) score += mood == Mood::Dim ? 30 : mood == Mood::Derelict ? 24 : mood == Mood::Dark ? 10 : 0;
                else score += mood == Mood::Derelict ? 30 : mood == Mood::Dark ? 26 : mood == Mood::Dim ? 12 : 0;
                for (Int2 r : run) {
                    for (const LowPipe& lp : level_.lowPipes)
                        if (std::abs(lp.center.x - cellCenter(r).x) < 2.1f && std::abs(lp.center.z - cellCenter(r).z) < 2.1f)
                            score -= 1000;
                    const int cellIdx = r.y * m.width() + r.x;
                    for (const PlacedModel& pm : level_.placed)
                        if (pm.cell == cellIdx && kit_.model(pm.model).name == "debris") score -= 6;
                }
                if (score > bestScore) {
                    bestScore = score;
                    best = run;
                    bestD = d;
                }
            }
    if (best.empty()) return false;
    scene_ = HuntScene{};
    scene_.fight = fight && !approach;
    scene_.approach = approach;
    scene_.threatShot = threatShot;
    scene_.run = best;
    scene_.dir = dirVector(Dir(bestD));
    scene_.start = cellCenter(best[0]) + scene_.dir * 0.5f + Vec3{0, kEyeHeight, 0};
    scene_.yaw = std::atan2(-scene_.dir.x, -scene_.dir.z);
    scene_.pitch = -0.04f;
    scene_.length = approach ? 18.0f : fight ? 16.0f : 24.0f;
    camera_ = Camera{};
    sway_ = {};
    camera_.position = scene_.start;
    camera_.yaw = scene_.yaw;
    camera_.pitch = scene_.pitch;
    flashlightOn_ = true;
    // the creature: just beyond the far end, on the ceiling
    Int2 far = best.back();
    const Int2 beyond = step(far, bestD);
    if (m.passable(far.x, far.y, Dir(bestD)) && m.kind(beyond) == CellKind::Corridor) far = beyond;
    hunters_.clear();
    respawnAt_.clear();
    huntersWanted_ = 0;
    spawnHunterAt(far, fight && !approach ? 0 : 2, maze_.params().seed * 977u + (approach ? 3u : fight ? 1u : 2u));
    if (threatShot && !hunters_.empty()) hunters_[0].setHuntSurface(2);
    return !hunters_.empty();
}

void Game::stepHuntScene(float t, float dt) {
    if (hunters_.empty()) return;
    Hunter& h = hunters_[0];
    auto aimAt = [&](Vec3 target, float rate) {
        const Vec3 d = target - camera_.position;
        const float wantYaw = std::atan2(-d.x, -d.z);
        const float wantPitch = std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z));
        float dy = wantYaw - scene_.yaw;
        while (dy > kPi) dy -= 2.0f * kPi;
        while (dy < -kPi) dy += 2.0f * kPi;
        const float k = std::min(1.0f, dt * rate);
        scene_.yaw += dy * k;
        scene_.pitch += (wantPitch - scene_.pitch) * k;
    };
    if (!playerDead()) {
        const Vec3 head = h.headPosition();
        const float dist = length(head - camera_.position);
        const bool hunting = h.state() == HunterState::Stalk || h.state() == HunterState::Chase ||
                             h.state() == HunterState::Leap || h.state() == HunterState::Threat ||
                             h.state() == HunterState::Coil;
        if (scene_.approach) {
            // standing your ground, not firing: it has spotted you and comes; you watch it
            if (t >= 0.5f && h.state() == HunterState::Roam) h.alert(huntWorld(false), scene_.threatShot);
            // (the threat shot keeps its head in the middle of the frame)
            if (h.alive()) aimAt(scene_.threatShot ? head : h.bodyCenter() * 0.4f + head * 0.6f, scene_.threatShot ? 4.0f : 3.0f);
            camera_.yaw = scene_.yaw;
            camera_.pitch = scene_.pitch;
        } else if (scene_.fight) {
            if (t >= 0.8f && h.state() == HunterState::Roam) h.alert(huntWorld(false), true);
            if (h.alive() && hunting) aimAt(h.bodyCenter() * 0.6f + head * 0.4f, 7.0f);
            else if (!h.alive()) aimAt(h.bodyCenter(), 2.0f);
            camera_.yaw = scene_.yaw;
            camera_.pitch = scene_.pitch;
            if (h.alive() && hunting && dist < 9.5f && t >= scene_.nextShot) {
                if (fire()) scene_.nextShot = t + 0.5f;
            }
            if (!h.alive() && scene_.endAt < 0.0f) scene_.endAt = t + 3.8f;
        } else {
            if (t >= 1.6f && h.state() == HunterState::Roam) h.alert(huntWorld(false), false);
            // walk in slowly; once it is close, back away from it
            const float lenRun = float(scene_.run.size() - 1) * kCell;
            float along = dot(camera_.position - scene_.start, scene_.dir);
            if (dist > 10.0f && h.state() != HunterState::Chase) along = std::min(along + 1.0f * dt, lenRun - 6.0f);
            else along = std::max(along - 1.1f * dt, -0.3f);
            const float bob = 0.012f * std::sin(along / 0.6f * kPi);
            camera_.position = scene_.start + scene_.dir * along + Vec3{0, bob, 0};
            if (hunting && t > 1.8f) aimAt(head, 3.5f);
            else aimAt(scene_.start + scene_.dir * 12.0f + Vec3{0, -0.3f, 0}, 2.0f);
            camera_.yaw = scene_.yaw;
            camera_.pitch = scene_.pitch;
            // two panicked hits as it charges: it flinches, and keeps coming
            if (h.state() == HunterState::Chase && dist < 8.5f && scene_.shots < 2 && t >= scene_.nextShot) {
                if (fire()) {
                    ++scene_.shots;
                    scene_.nextShot = t + 0.45f;
                }
            }
        }
    }
    stepWorld(dt);
    // ends most of the way into the fade to black (an all-black frame says nothing)
    if (playerDead() && scene_.endAt < 0.0f) scene_.endAt = t + 3.0f;
}

// ---------------------------------------------------------------------------
// The scanner scene (round 17): hold a junction while the scanner shows them coming
// ---------------------------------------------------------------------------

bool Game::setupScannerScene() {
    const Maze& m = maze_;
    const int W = m.width(), H = m.height();
    auto step = [](Int2 c, int d) { return Int2{c.x + kDx[d], c.y + kDy[d]}; };
    auto open = [&](Int2 c, int d) { return m.inBounds(c) && m.passable(c.x, c.y, Dir(d)) && m.inBounds(step(c, d)); };
    auto straightAlong = [&](Int2 c, int d) {
        if (!m.inBounds(c) || m.kind(c) != CellKind::Corridor) return false;
        return m.passable(c.x, c.y, Dir(d)) && m.passable(c.x, c.y, Dir((d + 2) & 3)) &&
               !m.passable(c.x, c.y, Dir((d + 1) & 3)) && !m.passable(c.x, c.y, Dir((d + 3) & 3));
    };
    struct Pick {
        Int2 j{};
        int ahead = 0, score = -100000;
        std::vector<Int2> spawns;
    } best;
    // walking distance from the junction, and which of its ways each cell is reached by
    std::vector<int> dist(size_t(W * H)), via(size_t(W * H));
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const Int2 j{x, y};
            if (m.kind(j) != CellKind::Corridor) continue;
            int ways = 0;
            for (int d = 0; d < 4; ++d) ways += open(j, d) ? 1 : 0;
            if (ways < 3) continue;
            std::fill(dist.begin(), dist.end(), -1);
            std::fill(via.begin(), via.end(), -1);
            std::vector<Int2> q;
            dist[size_t(y * W + x)] = 0;
            for (int d = 0; d < 4; ++d)
                if (open(j, d)) {
                    const Int2 n = step(j, d);
                    dist[size_t(n.y * W + n.x)] = 1;
                    via[size_t(n.y * W + n.x)] = d;
                    q.push_back(n);
                }
            for (size_t i = 0; i < q.size(); ++i) {
                const Int2 c = q[i];
                for (int d = 0; d < 4; ++d) {
                    if (!open(c, d)) continue;
                    const Int2 n = step(c, d);
                    if (dist[size_t(n.y * W + n.x)] >= 0) continue;
                    dist[size_t(n.y * W + n.x)] = dist[size_t(c.y * W + c.x)] + 1;
                    via[size_t(n.y * W + n.x)] = via[size_t(c.y * W + c.x)];
                    q.push_back(n);
                }
            }
            // per way: a corridor cell 9-11 cells off (prefer 10) reached through it
            auto spawnFor = [&](int d) {
                Int2 bestC{-1, -1};
                int bestErr = 99;
                for (const Int2& c : q) {
                    const size_t i = size_t(c.y * W + c.x);
                    if (via[i] != d || m.kind(c) != CellKind::Corridor) continue;
                    const int e = std::abs(dist[i] - 10);
                    if (e <= 1 && e < bestErr) bestErr = e, bestC = c;
                }
                return bestC;
            };
            for (int a = 0; a < 4; ++a) {
                if (!open(j, a)) continue;
                // the corridor ahead: straight, a few cells long
                int run = 0;
                Int2 c = step(j, a);
                std::vector<Int2> cells;
                while (straightAlong(c, a) && run < 8) cells.push_back(c), c = step(c, a), ++run;
                if (run < 3) continue;
                Pick p;
                p.j = j;
                p.ahead = a;
                const Int2 sa = spawnFor(a);
                if (sa.x < 0) continue;
                p.spawns.push_back(sa);
                for (int d = 0; d < 4; ++d)
                    if (d != a && open(j, d)) {
                        const Int2 s = spawnFor(d);
                        if (s.x >= 0) p.spawns.push_back(s);
                    }
                if (p.spawns.size() < 3) continue;  // ahead and two more ways
                const Mood mood = level_.sections.moodAt(cells[cells.size() / 2]);
                p.score = std::min(run, 6) * 4 + (mood == Mood::Lit ? 30 : mood == Mood::Dim ? 24 : mood == Mood::Derelict ? 8 : 0);
                for (Int2 r : cells) {
                    for (const LowPipe& lp : level_.lowPipes)
                        if (std::abs(lp.center.x - cellCenter(r).x) < 2.1f && std::abs(lp.center.z - cellCenter(r).z) < 2.1f)
                            p.score -= 1000;
                    for (const Emitter& e : level_.emitters)
                        if (e.kind == Emitter::Spark && std::abs(e.position.x - cellCenter(r).x) < 2.5f &&
                            std::abs(e.position.z - cellCenter(r).z) < 2.5f)
                            p.score -= 10;
                }
                if (p.score > best.score) best = p;
            }
        }
    if (best.spawns.empty()) return false;
    scan_ = ScanScene{};
    scan_.junction = best.j;
    scan_.ahead = dirVector(Dir(best.ahead));
    scan_.start = cellCenter(best.j) + Vec3{0, kEyeHeight, 0};
    scan_.yaw = std::atan2(-scan_.ahead.x, -scan_.ahead.z);
    scan_.pitch = -0.03f;
    scan_.spawnCells = best.spawns;
    if (scan_.spawnCells.size() > 3) scan_.spawnCells.resize(3);
    camera_ = Camera{};
    sway_ = {};
    camera_.position = scan_.start;
    camera_.yaw = scan_.yaw;
    camera_.pitch = scan_.pitch;
    flashlightOn_ = true;
    hunters_.clear();
    respawnAt_.clear();
    huntersWanted_ = 0;
    scanner_.reset();
    scanPrev_.clear();
    // the one that comes down the corridor ahead starts first; the others a little behind
    const float goAt[3] = {0.5f, 2.5f, 4.0f};
    for (size_t i = 0; i < scan_.spawnCells.size(); ++i) {
        spawnHunterAt(scan_.spawnCells[i], 2, maze_.params().seed * 613u + uint64_t(i) * 7u + 5u);
        scan_.goAt.push_back(goAt[i]);
    }
    return hunters_.size() == scan_.spawnCells.size();
}

void Game::stepScannerScene(float t, float dt) {
    for (size_t i = 0; i < hunters_.size() && i < scan_.goAt.size(); ++i) {
        if (scan_.goAt[i] >= 0.0f && t >= scan_.goAt[i]) {
            hunters_[i].headFor(maze_, scan_.junction, i == 0 ? 2 : 1);
            scan_.goAt[i] = -1.0f;  // (once)
        }
    }
    // standing still, breathing; the look drifts a little
    camera_.position = scan_.start + Vec3{0, 0.006f * std::sin(t * 1.7f), 0};
    camera_.yaw = scan_.yaw + 0.03f * std::sin(t * 0.37f);
    camera_.pitch = scan_.pitch + 0.012f * std::sin(t * 0.53f + 1.0f);
    sway_ = {};
    stepWorld(dt);
    // the end: once the first of them is in plain view ahead, when it has come within 9 m
    // (a second later), or 9 s after it showed
    for (size_t i = 0; i < hunters_.size(); ++i) {
        const Hunter& h = hunters_[i];
        if (!h.alive()) continue;
        const Vec3 d = h.headPosition() - camera_.position;
        const float dl = length(d);
        if (scan_.seenAt < 0.0f) {
            if (dl > 24.0f || dot(d, camera_.forward()) < 0.82f * dl) continue;
            if (occlusionTo(h.headPosition()) > 0.5f) continue;
            scan_.seenAt = t;
            scan_.seenBy = int(i);
            scan_.endAt = t + 9.0f;
        } else if (int(i) == scan_.seenBy && dl < 9.0f) {
            scan_.endAt = std::min(scan_.endAt, t + 1.0f);
        }
    }
    if (playerDead() && scan_.endAt < 0.0f) scan_.endAt = t + 3.0f;
}

// ---------------------------------------------------------------------------
// The glow scene (round 18): ten creatures lighting a dark corridor
// ---------------------------------------------------------------------------

bool Game::setupGlowScene() {
    const Maze& m = maze_;
    const int W = m.width();
    auto step = [](Int2 c, int d) { return Int2{c.x + kDx[d], c.y + kDy[d]}; };
    auto straightAlong = [&](Int2 c, int d) {
        if (!m.inBounds(c) || m.kind(c) != CellKind::Corridor) return false;
        return m.passable(c.x, c.y, Dir(d)) && m.passable(c.x, c.y, Dir((d + 2) & 3)) &&
               !m.passable(c.x, c.y, Dir((d + 1) & 3)) && !m.passable(c.x, c.y, Dir((d + 3) & 3));
    };
    // a long straight corridor where the fixtures give little light
    int bestScore = -100000, bestD = 0;
    std::vector<Int2> best;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < W; ++x)
            for (int d = 0; d < 4; ++d) {
                Int2 c{x, y};
                if (!straightAlong(c, d) || straightAlong(step(c, (d + 2) & 3), d)) continue;
                std::vector<Int2> run;
                while (straightAlong(c, d)) run.push_back(c), c = step(c, d);
                if (run.size() < 5) continue;
                const Mood mood = level_.sections.moodAt(run[run.size() / 2]);
                int score = int(std::min<size_t>(run.size(), 7)) * 4 +
                            (mood == Mood::Dark ? 60 : mood == Mood::Derelict ? 30 : mood == Mood::Dim ? 15 : -40);
                for (size_t k = 0; k < std::min<size_t>(run.size(), 7); ++k) {
                    const Int2 r = run[k];
                    for (const LowPipe& lp : level_.lowPipes)
                        if (std::abs(lp.center.x - cellCenter(r).x) < 2.1f && std::abs(lp.center.z - cellCenter(r).z) < 2.1f)
                            score -= 1000;
                    // fixtures still burning in it or beside it (and beacons) light it over the glow
                    for (int ny = -1; ny <= 1; ++ny)
                        for (int nx = -1; nx <= 1; ++nx) {
                            const int x2 = r.x + nx, y2 = r.y + ny;
                            if (x2 < 0 || y2 < 0 || x2 >= level_.gridW || y2 >= level_.gridH) continue;
                            const size_t ci = size_t(y2 * level_.gridW + x2);
                            for (uint32_t q = 0; q < level_.cellCount[ci]; ++q) {
                                const uint32_t li = level_.cellLights[level_.cellStart[ci] + q];
                                const Vec3 now = lightRadiance_[li], full = level_.lights[li].radiance;
                                if (level_.lights[li].beacon || now.x + now.y + now.z > 0.1f * (full.x + full.y + full.z))
                                    score -= (nx == 0 && ny == 0) ? 25 : 8;
                            }
                        }
                }
                if (score > bestScore) bestScore = score, best = run, bestD = d;
            }
    if (best.empty()) return false;
    if (best.size() > 7) best.resize(7);
    glow_ = GlowScene{};
    glow_.run = best;
    glow_.dir = dirVector(Dir(bestD));
    glow_.start = cellCenter(best[0]) + Vec3{0, kEyeHeight, 0} - glow_.dir * 1.5f;
    glow_.yaw = std::atan2(-glow_.dir.x, -glow_.dir.z);
    camera_ = Camera{};
    sway_ = {};
    camera_.position = glow_.start;
    camera_.yaw = glow_.yaw;
    camera_.pitch = 0.04f;
    flashlightOn_ = false;  // (their glow is the light)
    hunters_.clear();
    respawnAt_.clear();
    huntersWanted_ = 0;
    huntersIgnorePlayer_ = true;
    // ten of them in the corridor beyond the first two cells: two to a cell, one on the
    // ceiling and one on a wall or the floor (round the run again if it is short)
    const int lower[3] = {1, 0, 3};
    for (size_t i = 0; hunters_.size() < 10 && i < 40; ++i) {
        const size_t cellIdx = 1 + (i / 2) % std::min<size_t>(best.size() - 1, 4);
        const int surf = i % 2 == 0 ? 2 : lower[(i / 2) % 3];
        spawnHunterAt(best[cellIdx], surf, maze_.params().seed * 389u + uint64_t(i) * 11u + 3u);
    }
    glow_.retarget.assign(hunters_.size(), 0.0f);
    for (size_t i = 0; i < hunters_.size(); ++i) glow_.retarget[i] = 0.2f + 0.37f * float(i);
    return hunters_.size() == 10;
}

void Game::stepGlowScene(float t, float dt) {
    // each creature heads for another cell of the corridor now and then, over a wall or
    // the ceiling, so they keep crossing the view
    for (size_t i = 0; i < hunters_.size(); ++i) {
        if (t < glow_.retarget[i]) continue;
        const uint32_t h = mix32(uint32_t(i) * 7919u + uint32_t(t * 10.0f) * 104729u + 99u);
        const Int2 goal = glow_.run[1 + h % uint32_t(glow_.run.size() - 1)];
        const int surf[4] = {2, 1, 3, 2};
        hunters_[i].headFor(maze_, goal, surf[(h >> 8) % 4]);
        glow_.retarget[i] = t + 3.5f + 2.0f * unit(mix32(h));
    }
    // still for 3 s, then walking slowly up the corridor, looking from side to side
    const float walk = std::max(0.0f, t - 3.0f);
    const float along = 0.45f * walk;
    const float look = walk > 0.0f ? 0.42f * std::sin(walk * 0.75f) : 0.0f;
    camera_.position = glow_.start + glow_.dir * along + Vec3{0, 0.012f * std::sin(along / 0.6f * kPi), 0};
    camera_.yaw = glow_.yaw + look;
    camera_.pitch = 0.04f + 0.05f * std::sin(t * 0.4f);
    sway_ = {};
    stepWorld(dt);
}

// ---------------------------------------------------------------------------
// Sound (part 3): what the game hands the mixer each frame
// ---------------------------------------------------------------------------

void Game::sound(Snd s, Vec3 pos, float gain, float pitch, bool positional) {
    SoundEvent e;
    e.sound = s;
    e.time = time_;
    e.pos = pos;
    e.positional = positional;
    e.gain = gain;
    e.pitch = pitch;
    pendingSounds_.push_back(e);
}

Listener Game::listener() const {
    Listener l;
    l.pos = camera_.position;
    l.right = camera_.right();
    l.forward = camera_.forward();
    return l;
}

// 0 in plain view .. 1 behind a wall (or too far to tell: 45 m and beyond)
float Game::occlusionTo(Vec3 p) const {
    Vec3 d = p - camera_.position;
    const float dist = length(d);
    if (dist < 0.6f) return 0.0f;
    if (dist > 45.0f) return 1.0f;
    d = d * (1.0f / dist);
    Vec3 n;
    const float t = raycastLevel(maze_, level_, camera_.position, d, dist, doorOpen_, n);
    return t >= dist - 0.5f ? 0.0f : 1.0f;
}

// Claws landing (a foot going from lifted to planted), and for the scripted crawl creature
// a hiss whenever its jaws open wide.
void Game::listenToCreature(const Creature& c, CreatureEars& ears, bool scripted, float dt) {
    ears.hissAgain = std::max(0.0f, ears.hissAgain - dt);
    const bool near = length(c.bodyCenter() - camera_.position) < 32.0f;
    for (int leg = 0; leg < 4; ++leg) {
        const bool p = c.planted(leg);
        if (p && !ears.planted[size_t(leg)] && near) {
            const uint32_t h = mix32(uint32_t(time_ * 1000.0f) * 31u + uint32_t(leg) + uint32_t(ears.claw) * 7u);
            sound(ears.claw++ % 2 ? Snd::Claw2 : Snd::Claw1, c.foot(leg), 0.4f, 0.85f + 0.3f * unit(h));
        }
        ears.planted[size_t(leg)] = p;
    }
    if (scripted) {
        if (c.jaw() > 0.45f && ears.jaw <= 0.45f && ears.hissAgain <= 0.0f) {
            sound(Snd::CreatureHiss, c.headPosition(), 1.0f);
            ears.hissAgain = 2.5f;
        }
    }
    ears.jaw = c.jaw();
}

void Game::collectSounds(std::vector<SoundEvent>& events, std::vector<SoundLoop>& loops) {
    events.clear();
    loops.clear();
    const float dt = std::max(0.0f, time_ - soundTime_);
    const bool firstCall = !soundStarted_;
    // the scanner's pulses: one ping each, higher and louder the nearer the last contact
    for (const Scanner::Pulse& p : scanner_.takePulses()) {
        if (firstCall || !scannerActive() || playerDead()) continue;
        SoundEvent e;
        e.sound = Snd::ScannerPing;
        e.time = p.time;
        e.pos = camera_.position;
        e.positional = false;  // (the set in your hand)
        e.gain = Scanner::pingGain(p);
        e.pitch = Scanner::pingPitch(p);
        pendingSounds_.push_back(e);
    }
    if (!soundStarted_) {
        // first call: take stock, no one-shots (a capture starting late mustn't open with a crash)
        soundStarted_ = true;
        lastCam_ = camera_.position;
        lastBob_ = bob_;
        lastDoor_ = doorOpen_;
        hunterEars_.assign(hunters_.size(), {});
        for (size_t i = 0; i < hunters_.size(); ++i) hunterEars_[i].state = hunters_[i].state();
        pendingSounds_.clear();
    }

    // footsteps: one per head bob when the bob moved (the window, the scripted walk), else one
    // every 0.8 m the camera travels (scenes that place the camera themselves)
    const Vec3 move{camera_.position.x - lastCam_.x, 0.0f, camera_.position.z - lastCam_.z};
    const float travelled = length(move);
    if (!playerDead() && travelled < 3.0f && travelled > 0.0f) {
        int n = 0;
        if (bob_ != lastBob_) {
            n = int(std::floor(bob_ / kPi)) - int(std::floor(lastBob_ / kPi));
            n = std::clamp(n, 0, 3);
        } else {
            stepAcc_ += travelled;
            while (stepAcc_ >= 0.8f) stepAcc_ -= 0.8f, ++n;
        }
        for (int k = 0; k < n; ++k) {
            const uint32_t h = mix32(uint32_t(++steps_) * 2654435761u);
            const Snd s = steps_ % 3 == 0 ? Snd::Footstep1 : steps_ % 3 == 1 ? Snd::Footstep2 : Snd::Footstep3;
            sound(s, camera_.position - Vec3{0, 1.5f, 0}, 0.5f * (1.0f - 0.5f * crouch_), 0.92f + 0.16f * unit(h), false);
        }
    }
    lastCam_ = camera_.position;
    lastBob_ = bob_;

    // hunters: claws, and what each state change sounds like
    if (hunterEars_.size() != hunters_.size()) {
        hunterEars_.assign(hunters_.size(), {});
        for (size_t i = 0; i < hunters_.size(); ++i) hunterEars_[i].state = hunters_[i].state();
    }
    for (size_t i = 0; i < hunters_.size(); ++i) {
        const Hunter& h = hunters_[i];
        CreatureEars& ears = hunterEars_[i];
        const HunterState s = h.state();
        if (s != ears.state) {
            if (s == HunterState::Stalk) sound(Snd::CreatureHiss, h.headPosition(), 1.0f);
            if (s == HunterState::Threat) sound(Snd::CreatureHiss, h.headPosition(), 1.3f, 0.92f);
            if (s == HunterState::Leap) sound(Snd::CreatureScreech, h.headPosition(), 1.1f);
            if (s == HunterState::Dying) sound(Snd::CreatureDeath, h.headPosition(), 1.0f);
            ears.state = s;
        }
        if (h.alive() && s != HunterState::Feed) listenToCreature(h.creature(), ears, false, dt);
        if (s == HunterState::Chase)
            loops.push_back({Snd::CreatureSkitter, 100u + uint32_t(i), h.bodyCenter(), true, 0.6f, 1.0f, 0.0f});
    }
    if (creatureActive()) listenToCreature(creature_, crawlEars_, true, dt);

    // being caught: the slam, then feeding until the fade
    if (playerDead() && !wasDead_) sound(Snd::PlayerThud, camera_.position, 1.0f, 1.0f, false);
    wasDead_ = playerDead();
    if (playerDead() && deathT_ > 0.15f)
        loops.push_back({Snd::Feeding, 200u, camera_.position, false, 0.9f * (1.0f - blackout()), 1.0f, 0.0f});

    // doors: when one starts to open or to close
    if (lastDoor_.size() != doorOpen_.size()) lastDoor_ = doorOpen_;
    for (size_t i = 0; i < doorOpen_.size(); ++i) {
        const float was = lastDoor_[i], now = doorOpen_[i];
        const Door& d = level_.doors[i];
        if (length(d.center - camera_.position) < 35.0f) {
            const Snd s = d.kind == DoorKind::Sliding ? Snd::DoorSliding : Snd::DoorHeavy;
            const Vec3 p = d.center + Vec3{0, 1.2f, 0};
            if (was <= 0.001f && now > 0.001f) sound(s, p, 0.8f);
            else if (was >= 0.999f && now < 0.999f) sound(s, p, 0.6f, 0.9f);
        }
        lastDoor_[i] = now;
    }

    // sparks: bursts that started since the last call; steam: loops while they blow
    for (int idx : nearEmitters_) {
        const Emitter& e = level_.emitters[size_t(idx)];
        if (e.kind == Emitter::Spark) {
            // (within 16 m: a derelict corridor has dozens of live wires in earshot)
            if (length(e.position - camera_.position) > 16.0f) continue;
            const uint32_t m0 = slotOf(time_ + e.phase * kSparkSlot, kSparkSlot);
            for (uint32_t m = m0 >= 3 ? m0 - 3 : 0; m <= m0; ++m) {
                float t0;
                uint32_t seed;
                if (sparkBurst(uint32_t(idx), m, e.phase, t0, seed) && t0 > soundTime_ && t0 <= time_ && dt < 1.0f)
                    sound(Snd::Sparks, e.position, 0.45f, 0.9f + 0.25f * unit(seed));
            }
        } else {
            const float s = steamStrength(uint32_t(idx), time_, e.phase);
            if (s > 0.05f) loops.push_back({Snd::Steam, 1000u + uint32_t(idx), e.position, true, 0.55f * s, 1.0f, 0.0f});
        }
    }

    // the ship: its hum, and now and then something groaning or knocking far off
    loops.push_back({Snd::ShipHum, 1u, camera_.position, false, 0.32f, 1.0f, 0.0f});
    const float slotLen = 23.0f;
    const int64_t slotNow = int64_t(std::floor(time_ / slotLen)), slotWas = int64_t(std::floor(soundTime_ / slotLen));
    if (slotNow != slotWas && slotNow > 0 && dt < 1.0f) {
        const uint32_t h = hash2(uint32_t(slotNow), maze_.params().seed ? uint32_t(maze_.params().seed) : 1u);
        if (unit(h) < 0.7f) {
            const float a = unit(mix32(h)) * 2.0f * kPi;
            // (placed 10 m off: the walls in between muffle it, the room carries it)
            const Vec3 p = camera_.position + Vec3{std::cos(a), 0.1f, std::sin(a)} * 10.0f;
            sound(unit(mix32(h + 9u)) < 0.6f ? Snd::HullGroan : Snd::DistantKnock, p, 1.6f, 0.9f + 0.2f * unit(h >> 3));
        }
    }

    // how hidden each one is
    for (SoundEvent& e : pendingSounds_) {
        if (e.positional) e.occlusion = occlusionTo(e.pos);
        events.push_back(e);
    }
    pendingSounds_.clear();
    for (SoundLoop& l : loops)
        if (l.positional) l.occlusion = occlusionTo(l.pos);
    soundTime_ = time_;
}

// ---------------------------------------------------------------------------
// The climb (round 15): the crawl as a showcase
// ---------------------------------------------------------------------------

namespace {
const char* kClimbStages[] = {"floor", "floor to wall", "wall", "the edge: wall to ceiling", "ceiling",
                              "ceiling to wall", "wall", "wall to floor", "floor"};
}

bool Game::setupClimbScene() {
    creatureOn_ = false;
    if (!creature_.valid()) return false;
    const Maze& m = maze_;
    auto step = [](Int2 c, int d) { return Int2{c.x + kDx[d], c.y + kDy[d]}; };
    auto straightAlong = [&](Int2 c, int d) {
        if (!m.inBounds(c) || m.kind(c) != CellKind::Corridor) return false;
        return m.passable(c.x, c.y, Dir(d)) && m.passable(c.x, c.y, Dir((d + 2) & 3)) &&
               !m.passable(c.x, c.y, Dir((d + 1) & 3)) && !m.passable(c.x, c.y, Dir((d + 3) & 3));
    };
    // a long straight corridor, lit (black armour reads by the fixtures' highlights), with
    // nothing in it the creature would crawl through
    int bestScore = -100000, bestD = 0;
    std::vector<Int2> best;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            for (int d = 0; d < 4; ++d) {
                Int2 c{x, y};
                if (!straightAlong(c, d) || straightAlong(step(c, (d + 2) & 3), d)) continue;
                std::vector<Int2> run;
                while (straightAlong(c, d)) {
                    run.push_back(c);
                    c = step(c, d);
                }
                if (run.size() < 5) continue;
                const Mood mood = level_.sections.moodAt(run[run.size() / 2]);
                int score = int(std::min<size_t>(run.size(), 7)) * 4 +
                            (mood == Mood::Lit ? 60 : mood == Mood::Dim ? 30 : mood == Mood::Dark ? -30 : 0);
                for (Int2 r : run) {
                    for (const LowPipe& lp : level_.lowPipes)
                        if (std::abs(lp.center.x - cellCenter(r).x) < 2.1f && std::abs(lp.center.z - cellCenter(r).z) < 2.1f)
                            score -= 1000;
                    const int cell = r.y * m.width() + r.x;
                    for (const PlacedModel& pm : level_.placed) {
                        if (pm.cell != cell) continue;
                        const std::string& nm = kit_.model(pm.model).name;
                        if (nm == "corridor_straight_broken" || nm == "corridor_straight_window") score -= 20;
                        else if (nm == "pipe_burst" || nm == "debris" || nm == "beacon") score -= 8;
                    }
                    // sparks falling through the shot pull the eye off the feet
                    for (const Emitter& e : level_.emitters)
                        if (e.kind == Emitter::Spark && std::abs(e.position.x - cellCenter(r).x) < 4.5f &&
                            std::abs(e.position.z - cellCenter(r).z) < 4.5f)
                            score -= 25;
                }
                if (score > bestScore) {
                    bestScore = score;
                    best = run;
                    bestD = d;
                }
            }
    if (best.empty()) return false;
    crawlPath_ = CrawlPath(best);
    climb_ = ClimbScene{};
    climb_.mood = level_.sections.moodAt(best[best.size() / 2]);
    const CrawlProfile& pr = crawlPath_.profile();
    const float R = pr.rightWallU(), C = pr.ceilingU(), Lw = pr.leftWallU(), T = pr.length();
    // stages: floor, up, wall, the edge, ceiling, down, wall, down, floor (metres along)
    float len[] = {2.4f, 2.0f, 1.8f, 2.6f, 2.0f, 2.0f, 1.4f, 2.0f, 1.2f};
    const float u[] = {0.0f, R, R, C, C, Lw, Lw, T, T};  // u at the end of each stage
    climb_.s0 = 2.0f;  // the body all on the path
    float total = 0.0f;
    for (float l : len) total += l;
    const float room = crawlPath_.length() - climb_.s0 - 1.6f;  // (its head is ~1 m ahead)
    if (room < total) {
        const float k = std::max(0.6f, room / total);
        for (float& l : len) l *= k;
    }
    float s = climb_.s0;
    climb_.keyS.push_back(s);
    climb_.keyU.push_back(0.0f);
    for (int i = 0; i < 9; ++i) {
        climb_.keyStage.push_back(i);
        s += len[i];
        climb_.keyS.push_back(s);
        climb_.keyU.push_back(u[i]);
    }
    climb_.keyStage.push_back(8);
    climb_.sEnd = s;
    // distance -> time: 0.75 m/s, slowing to 0.32 m/s through the edge
    float t = 0.0f;
    for (float x = climb_.s0; x <= climb_.sEnd + 1e-3f; x += 0.01f) {
        const float e0 = climb_.keyS[3], e1 = climb_.keyS[4];
        float v = 0.75f;
        if (x > e0 - 0.6f && x < e1 + 0.6f) {
            const float k = std::min(smooth01(e0 - 0.6f, e0 + 0.2f, x), 1.0f - smooth01(e1 - 0.2f, e1 + 0.6f, x));
            v = 0.75f + (0.32f - 0.75f) * k;
        }
        climb_.tableS.push_back(x);
        climb_.tableT.push_back(t);
        t += 0.01f / v;
    }
    climb_.duration = climb_.tableT.back() + 0.8f;
    flashlightOn_ = false;  // (the corridor's own light: the flashlight would flatten it)
    hunters_.clear();
    huntersWanted_ = 0;
    creatureOn_ = true;
    setClimbTime(0.0f);
    (void)bestD;
    return true;
}

float Game::climbS(float t) const {
    const auto& T = climb_.tableT;
    const auto& S = climb_.tableS;
    if (T.empty()) return 0.0f;
    if (t <= 0.0f) return S.front();
    if (t >= T.back()) return S.back();
    const size_t i = size_t(std::upper_bound(T.begin(), T.end(), t) - T.begin());
    const float f = (t - T[i - 1]) / std::max(1e-6f, T[i] - T[i - 1]);
    return S[i - 1] + (S[i] - S[i - 1]) * f;
}

float Game::climbU(float s) const {
    const auto& K = climb_.keyS;
    if (K.empty()) return 0.0f;
    if (s <= K.front()) return climb_.keyU.front();
    if (s >= K.back()) return climb_.keyU.back();
    const size_t i = size_t(std::upper_bound(K.begin(), K.end(), s) - K.begin());
    return climb_.keyU[i - 1] + (climb_.keyU[i] - climb_.keyU[i - 1]) * smooth01(K[i - 1], K[i], s);
}

int Game::climbStage(float s) const {
    const auto& K = climb_.keyS;
    for (size_t i = 1; i < K.size(); ++i)
        if (s < K[i]) return climb_.keyStage[i - 1];
    return 8;
}

Game::ClimbInfo Game::climbInfo() const {
    ClimbInfo ci;
    ci.s = climbS(time_);
    const float sT = ci.s - creature_.restPivot("thorax").z;
    const int st = climbStage(sT);
    ci.stage = kClimbStages[st];
    ci.shot = climb_.lastShot;
    return ci;
}

void Game::setClimbTime(float t) {
    const float s = climbS(t);
    CreaturePose p;
    p.s = s;
    p.uOf = [this](float ss) { return climbU(ss); };
    p.time = t;
    p.jaw = 0.08f + 0.06f * std::sin(t * 1.4f);
    p.glow = 0.95f + 0.1f * std::sin(t * 2.1f);
    creature_.pose(crawlPath_, p);

    // the camera: one setup per stage of the climb (thorax position), placed relative to
    // the creature along the corridor: metres ahead, to the right, up from the floor
    const float sT = s - creature_.restPivot("thorax").z;
    const int st = climbStage(sT);
    struct Setup {
        float ahead, right, up;
    };
    int shot = st == 0 ? 0 : st == 1 ? 1 : st == 2 ? 2 : st == 3 ? 3 : st == 4 ? 4 : st <= 6 ? 5 : 6;
    static const Setup kSetups[] = {
        {2.4f, -0.70f, 0.45f},  // 0 low on the floor ahead of it, coming at the camera
        {1.3f, -0.75f, 0.75f},  // 1 its feet taking hold of the right wall
        {0.6f, -0.85f, 1.35f},  // 2 across from it on the wall
        {1.8f, -0.50f, 1.30f},  // 3 the edge: a slow pass (ahead goes 1.8 -> -0.6)
        {1.6f, 0.00f, 1.00f},   // 4 under it on the ceiling, looking up
        {0.9f, 0.80f, 1.20f},   // 5 across from it, down onto the left wall
        {2.2f, 0.60f, 0.50f},   // 6 low again as it comes back down to the floor
    };
    Setup su = kSetups[shot];
    if (shot == 3) {
        const float k = smooth01(climb_.keyS[3], climb_.keyS[4], sT);
        su.ahead = 1.8f + (-0.6f - 1.8f) * k;
    }
    const float sc = std::clamp(sT + su.ahead, 0.0f, crawlPath_.length());
    const Vec3 camPos = crawlPath_.point(sc, 0.0f) + crawlPath_.right(sc) * su.right + Vec3{0, su.up, 0};
    // look at its middle, a little toward the legs; eased (a cut snaps it)
    const Vec3 want = creature_.bodyCenter();
    const float dt = climb_.lastT < 0.0f ? 0.0f : std::max(0.0f, t - climb_.lastT);
    if (shot != climb_.lastShot || climb_.lastT < 0.0f) climb_.target = want;
    else climb_.target = climb_.target + (want - climb_.target) * (1.0f - std::exp(-6.0f * dt));
    climb_.lastShot = shot;
    climb_.lastT = t;
    const Vec3 d = climb_.target - camPos;
    Camera c;
    c.position = camPos;
    c.yaw = std::atan2(-d.x, -d.z);
    c.pitch = std::clamp(std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z)), -1.3f, 1.3f);
    camera_ = c;
    sway_ = {};
    time_ = t;
    updateLighting();
    reveal();
    updateEffects();
}

// ---------------------------------------------------------------------------
// The window scene (part 3): a scripted camera and the crawl creature
// ---------------------------------------------------------------------------

bool Game::setupWindowScene() {
    if (!creature_.valid()) return false;
    const Maze& m = maze_;
    auto stepTo = [](Int2 c, Vec3 d) { return Int2{c.x + int(std::lround(d.x)), c.y + int(std::lround(d.z))}; };
    auto dirOf = [](Vec3 d) {
        for (int k = 0; k < 4; ++k)
            if (dot(dirVector(Dir(k)), d) > 0.9f) return Dir(k);
        return Dir::North;
    };
    auto open = [&](Int2 c, Vec3 d) { return m.inBounds(c) && m.passable(c.x, c.y, dirOf(d)); };
    float bestScore = -1.0f;
    for (const Level::Viewport& vp : level_.viewports) {
        const Vec3 O = dirVector(vp.out);
        for (float sgn : {1.0f, -1.0f}) {
            // A points to where the creature comes from (two corridor cells that way); you
            // walk in from the other side
            const Vec3 A = Vec3{-O.z, 0.0f, O.x} * sgn;
            const Int2 c = vp.cell, p1 = stepTo(c, A), p2 = stepTo(p1, A), a1 = stepTo(c, -A);
            if (!open(c, A) || !open(p1, A) || !open(c, -A)) continue;
            if (m.kind(p1) != CellKind::Corridor || m.kind(p2) != CellKind::Corridor || m.kind(a1) == CellKind::Void)
                continue;
            const Mood mood = level_.sections.moodAt(c);
            // enough light for the corridor to show in the glass, dark enough for the stars
            float score = mood == Mood::Dim ? 3.0f : mood == Mood::Derelict ? 2.5f : mood == Mood::Dark ? 1.5f : 1.0f;
            if (m.kind(a1) == CellKind::Corridor) score += 0.5f;  // walking in down a corridor
            if (score > bestScore) {
                bestScore = score;
                window_ = WindowScene{};
                window_.cell = c;
                window_.C = cellCenter(c);
                window_.A = A;
                window_.O = O;
                std::vector<Int2> cells{p2, p1, c, a1};
                crawlPath_ = CrawlPath(cells);
            }
        }
    }
    if (bestScore < 0.0f) return false;
    WindowScene& w = window_;
    const Vec3 up{0, 1.42f, 0};
    w.v1 = w.C + w.O * 1.5f - w.A * 0.86f + up;
    w.v2 = w.C + w.O * 1.5f + w.A * 0.86f + up;
    // the creature keeps to the wall opposite the windows (it shows in the glass there)
    const float sC = crawlPath_.project(w.C);
    const CrawlProfile& pr = crawlPath_.profile();
    w.uWall = dot(crawlPath_.right(sC), w.O) > 0.0f ? pr.leftWallU() : pr.rightWallU();
    // s is about a metre behind the head. Seen at an angle from where you stand, the far
    // viewport mirrors the opposite wall 4-6 m down the corridor: it comes in along the
    // ceiling there, climbs down onto that wall in the reflection, and stops ~2 m down
    // the corridor, facing you
    w.sStart = 1.6f;
    w.sDesc = crawlPath_.project(w.C + w.A * 4.6f) - 1.0f;
    w.sStop = crawlPath_.project(w.C + w.A * 1.9f) - 1.0f;
    w.creatureFrom = 8.0f;  // waiting on the ceiling out of sight (you look at the near viewport)
    w.moveFrom = 10.0f;     // and crawling as the far viewport comes round
    w.speed = 0.9f;
    w.length = 17.5f;
    creatureOn_ = false;
    hunters_.clear();
    respawnAt_.clear();
    huntersWanted_ = 0;
    flashlightOn_ = true;
    setWindowTime(0.0f);
    return true;
}

float Game::windowU(float s) const {
    const float ceil = crawlPath_.profile().ceilingU();
    return ceil + (window_.uWall - ceil) * smooth01(window_.sDesc, window_.sDesc + 2.2f, s);
}

void Game::setWindowTime(float t) {
    const WindowScene& w = window_;
    // the creature: crawls from t = creatureFrom at constant speed for 85% of the way,
    // then eases into a stop behind you (same speed where the ease begins)
    const float run = std::max(0.5f, w.sStop - w.sStart);
    const float u = std::max(0.0f, t - w.moveFrom) * w.speed / run;
    float f = u;
    if (u > 0.85f) {
        const float e = std::min(1.0f, (u - 0.85f) / 0.3f);
        f = 0.85f + 0.15f * (1.0f - (1.0f - e) * (1.0f - e));
    }
    const float s = w.sStart + run * f;
    const float stopAt = w.moveFrom + 1.15f * run / w.speed;
    creatureOn_ = t >= w.creatureFrom;

    // the camera
    const Vec3 P0 = w.C - w.A * 4.6f - w.O * 0.15f, S = w.C - w.A * 0.86f + w.O * 0.15f, S2 = S + w.O * 0.25f;
    const float walk = smooth01(0.0f, 4.2f, t);
    Vec3 pos = t < 4.2f ? P0 + (S - P0) * walk : S + (S2 - S) * smooth01(4.2f, 9.0f, t);
    const float walking = t < 4.2f ? 1.0f - smooth01(3.4f, 4.2f, t) : 0.0f;
    const float dist = length(S - P0) * walk;
    pos.y = kEyeHeight - 0.08f + 0.02f * walking * std::sin(dist / 0.62f * kPi);
    auto yawPitch = [&](Vec3 target, float& yaw, float& pitch) {
        const Vec3 d = target - pos;
        yaw = std::atan2(-d.x, -d.z);
        pitch = std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z));
    };
    auto blend = [](float a, float b, float k) {
        float d = b - a;
        while (d > kPi) d -= 2.0f * kPi;
        while (d < -kPi) d += 2.0f * kPi;
        return a + d * k;
    };
    float yaw = 0.0f, pitch = 0.0f, y2 = 0.0f, p2 = 0.0f;
    const Vec3 lookOut1 = w.v1 + w.O * 3.0f + Vec3{0, 0.25f, 0} + w.A * (0.15f * std::sin(t * 0.4f));
    // the far viewport, looked at steeply: it mirrors the opposite wall far down the
    // corridor (where the creature comes from), and glass reflects more at that angle
    const Vec3 lookOut2 = w.v2 + w.O * 0.4f + Vec3{0, 0.05f, 0};
    yawPitch(pos + w.A * 6.0f + Vec3{0, -0.1f, 0}, yaw, pitch);  // down the corridor
    yawPitch(lookOut1, y2, p2);
    float k = smooth01(1.6f, 4.4f, t);
    yaw = blend(yaw, y2, k), pitch += (p2 - pitch) * k;
    yawPitch(lookOut2, y2, p2);
    k = smooth01(9.6f, 11.8f, t);
    yaw = blend(yaw, y2, k), pitch += (p2 - pitch) * k;
    const float headS = s + 1.0f;
    const float uh = windowU(headS);
    const Vec3 head = crawlPath_.point(headS, uh) + crawlPath_.normal(headS, uh) * 0.45f;
    yawPitch(head, y2, p2);
    k = smooth01(13.4f, 15.2f, t);
    yaw = blend(yaw, y2, k), pitch += (p2 - pitch) * k;
    Camera c;
    c.position = pos;
    c.yaw = yaw;
    c.pitch = pitch;
    camera_ = c;
    sway_ = {};

    if (creatureOn_) {
        CreaturePose p;
        p.s = s;
        p.uOf = [this](float ss) { return windowU(ss); };
        p.time = t;
        const float flutter = 0.85f + 0.15f * std::sin(t * 23.0f);
        const float hiss = smooth01(stopAt + 0.6f, stopAt + 1.4f, t);
        const float faced = smooth01(13.8f, 15.0f, t);
        p.jaw = std::max(0.06f + 0.05f * std::sin(t * 1.3f), std::max(0.6f * hiss, 0.95f * faced) * flutter);
        p.look = t > stopAt;
        p.lookAt = pos;
        p.glow = 0.9f + 0.5f * std::max(hiss, faced) + 0.1f * std::sin(t * 2.3f);
        creature_.pose(crawlPath_, p);
    }
    time_ = t;
    updateLighting();
    reveal();
    updateEffects();
}

}  // namespace bh
