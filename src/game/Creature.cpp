#include "game/Creature.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "game/Game.h"

namespace bh {
namespace {

const char* kBodyBones[] = {"pelvis", "waist", "thorax", "neck", "head"};
const char* kLegNames[] = {"ML", "MR", "HL", "HR"};
const float kLegPhase[] = {0.0f, 0.5f, 0.5f, 0.0f};  // diagonal pairs: ML+HR, MR+HL
const char* kJaws[] = {"jaw_t", "jaw_b", "jaw_l", "jaw_r"};
const Vec3 kJawRadial[] = {{0, 1, 0}, {0, -1, 0}, {-1, 0, 0}, {1, 0, 0}};

Mat4 inverseRigid(const Mat4& m) {
    Mat4 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = m.m[j][i];
    for (int i = 0; i < 3; ++i) r.m[i][3] = -(r.m[i][0] * m.m[0][3] + r.m[i][1] * m.m[1][3] + r.m[i][2] * m.m[2][3]);
    return r;
}

Vec3 column(const Mat4& m, int c);

Mat4 axisAngle(Vec3 a, float ang) {
    a = normalize(a);
    const float c = std::cos(ang), s = std::sin(ang), t = 1.0f - c;
    Mat4 r;
    r.m[0][0] = c + a.x * a.x * t;
    r.m[0][1] = a.x * a.y * t - a.z * s;
    r.m[0][2] = a.x * a.z * t + a.y * s;
    r.m[1][0] = a.y * a.x * t + a.z * s;
    r.m[1][1] = c + a.y * a.y * t;
    r.m[1][2] = a.y * a.z * t - a.x * s;
    r.m[2][0] = a.z * a.x * t - a.y * s;
    r.m[2][1] = a.z * a.y * t + a.x * s;
    r.m[2][2] = c + a.z * a.z * t;
    return r;
}

// A frame whose Z runs along d, with X as close to n as possible.
Mat4 frameAlong(Vec3 origin, Vec3 d, Vec3 n) {
    Vec3 z = normalize(d);
    Vec3 x = n - z * dot(n, z);
    if (length(x) < 1e-5f) x = std::abs(z.y) < 0.9f ? cross(Vec3{0, 1, 0}, z) : cross(Vec3{1, 0, 0}, z);
    x = normalize(x);
    Vec3 y = cross(z, x);
    return Mat4::basis(x, y, z, origin);
}

Vec3 column(const Mat4& m, int c) { return {m.m[0][c], m.m[1][c], m.m[2][c]}; }

}  // namespace

const char* Creature::legName(int leg) { return kLegNames[leg]; }

int Creature::find(const std::string& name) const {
    for (size_t i = 0; i < bones_.size(); ++i)
        if (bones_[i].name == name) return int(i);
    return -1;
}

Creature::Creature(const Kit& kit) {
    model_ = kit.find("creature");
    if (model_ < 0) return;
    const KitModel& m = kit.model(model_);
    std::vector<std::string> names(std::begin(kBodyBones), std::end(kBodyBones));
    for (const char* j : kJaws) names.push_back(j);
    for (int i = 0; i < 6; ++i) names.push_back("tail" + std::to_string(i));
    for (const char* l : kLegNames) {
        names.push_back(std::string("femur_") + l);
        names.push_back(std::string("tibia_") + l);
    }
    for (const char* a : {"arm_L", "arm_R", "fore_L", "fore_R"}) names.push_back(a);
    for (const std::string& n : names) {
        Bone b;
        b.name = n;
        bool havePivot = false;
        for (const KitMarker& mk : m.markers)
            if (mk.name == "bone_" + n) {
                b.pivot = mk.position;
                havePivot = true;
            }
        for (uint32_t part : m.parts) {
            const std::string& pn = kit.parts()[part].name;
            if (pn == "creature_" + n || pn.starts_with("creature_" + n + ".")) b.parts.push_back(part);
        }
        if (!havePivot || b.parts.empty()) return;  // incomplete kit: no creature
        bones_.push_back(b);
    }
    for (int l = 0; l < 4; ++l) {
        bool found = false;
        for (const KitMarker& mk : m.markers)
            if (mk.name == std::string("foot_") + kLegNames[l]) {
                restFoot_[size_t(l)] = mk.position;
                found = true;
            }
        if (!found) return;
    }
    // hit spheres: along the body chain, the head and jaws, and each leg segment
    auto piv = [&](const char* n) { return bones_[size_t(find(n))].pivot; };
    auto add = [&](const char* bone, Vec3 c, float r) { hits_.push_back({find(bone), c, r}); };
    const char* chain[] = {"pelvis", "waist", "thorax", "neck", "head"};
    const float radius[] = {0.19f, 0.12f, 0.19f, 0.1f, 0.13f};
    for (int i = 0; i < 5; ++i) {
        add(chain[i], piv(chain[i]), radius[i]);
        if (i < 4) add(chain[i], (piv(chain[i]) + piv(chain[i + 1])) * 0.5f, radius[i]);
    }
    add("pelvis", piv("pelvis") + Vec3{0, 0, 0.18f}, 0.17f);
    add("head", piv("head") + Vec3{0, -0.02f, -0.22f}, 0.1f);
    add("head", piv("head") + Vec3{0, -0.03f, -0.38f}, 0.08f);
    for (int k = 0; k < 3; ++k) add(("tail" + std::to_string(k)).c_str(), piv(("tail" + std::to_string(k)).c_str()), 0.08f);
    for (int l = 0; l < 4; ++l) {
        const std::string f = std::string("femur_") + kLegNames[l], t = std::string("tibia_") + kLegNames[l];
        add(f.c_str(), (piv(f.c_str()) + piv(t.c_str())) * 0.5f, 0.07f);
        add(t.c_str(), piv(t.c_str()) * 0.6f + restFoot_[size_t(l)] * 0.4f, 0.06f);
    }
    valid_ = true;
}

namespace {
// Blend two rigid frames (position lerp, axes lerp re-orthonormalised).
Mat4 lerpFrame(const Mat4& a, const Mat4& b, float t) {
    Vec3 x = normalize(lerp(column(a, 0), column(b, 0), t));
    Vec3 y = lerp(column(a, 1), column(b, 1), t);
    y = normalize(y - x * dot(y, x));
    Vec3 z = cross(x, y);
    return Mat4::basis(x, y, z, lerp(column(a, 3), column(b, 3), t));
}
}  // namespace

std::vector<Mat4> Creature::localPose(float jaw, float arms, float legs, float tailCurl, float time) const {
    std::vector<Mat4> local(bones_.size());
    for (size_t i = 0; i < bones_.size(); ++i) local[i] = Mat4::identity();
    auto about = [](Vec3 pivot, const Mat4& r) { return Mat4::translation(pivot) * r * Mat4::translation(-pivot); };
    auto idx = [&](const std::string& n) { return size_t(find(n)); };
    for (int j = 0; j < 4; ++j) {
        Vec3 axis = cross(Vec3{0, 0, -1}, kJawRadial[j]);
        local[idx(kJaws[j])] = about(bones_[idx(kJaws[j])].pivot, axisAngle(axis, 0.6f * jaw));
    }
    for (const char* side : {"L", "R"}) {
        const float sgn = side[0] == 'L' ? -1.0f : 1.0f;
        const size_t a = idx(std::string("arm_") + side);
        // raise the scythes: up and outward
        Mat4 r = about(bones_[a].pivot, Mat4::rotationX(0.9f * arms) * Mat4::rotationZ(-0.35f * arms * sgn));
        local[a] = r;
        local[idx(std::string("fore_") + side)] = r;
    }
    for (int l = 0; l < 4; ++l) {
        const float sgn = kLegNames[l][1] == 'R' ? 1.0f : -1.0f;
        const size_t f = idx(std::string("femur_") + kLegNames[l]), t = idx(std::string("tibia_") + kLegNames[l]);
        const float tw = 0.12f * std::sin(time * 13.0f + float(l) * 1.7f) * std::max(0.0f, legs);
        Mat4 rf, rt;
        if (legs >= 0.0f) {  // curl in under the belly
            // femurs swing down past the horizontal; the lower legs then point in under the
            // belly (on its back: legs folded up over it, like a dead insect)
            rf = about(bones_[f].pivot, Mat4::rotationZ(-sgn * (1.85f * legs + tw)));
            rt = about(bones_[t].pivot, Mat4::rotationZ(-sgn * 0.45f * legs));
        } else {             // thrown forward, claws reaching
            const float k = -legs;
            const float fwdSwing = kLegNames[l][0] == 'M' ? 0.7f : 0.35f;
            rf = about(bones_[f].pivot, Mat4::rotationX(-fwdSwing * k) * Mat4::rotationZ(sgn * 0.25f * k));
            rt = about(bones_[t].pivot, Mat4::rotationZ(sgn * 0.35f * k));
        }
        local[f] = rf;
        local[t] = rf * rt;
    }
    Mat4 acc = Mat4::identity();
    for (int k = 0; k < 6; ++k) {
        const size_t i = idx("tail" + std::to_string(k));
        acc = acc * about(bones_[i].pivot, Mat4::rotationX(tailCurl * 0.22f) *
                                               Mat4::rotationY(0.1f * std::sin(time * 2.0f - float(k))));
        local[i] = acc;
    }
    return local;
}

Vec3 Creature::restPivot(const char* bone) const {
    const int i = find(bone);
    return i >= 0 ? bones_[size_t(i)].pivot : Vec3{};
}

Mat4 Creature::blendFrames(const Mat4& a, const Mat4& b, float t) { return lerpFrame(a, b, t); }

Mat4 Creature::bodyMatrix() const {
    const Bone& t = bones_[size_t(find("thorax"))];
    return t.world;
}

void Creature::updateFeet() {
    for (int l = 0; l < 4; ++l) {
        feet_[size_t(l)] = posedClaw(l);
        planted_[size_t(l)] = false;
    }
    const Bone& h = bones_[size_t(find("head"))];
    head_ = h.world.point(h.pivot + Vec3{0, 0, -0.42f});
    const Bone& t = bones_[size_t(find("thorax"))];
    center_ = t.world.point(t.pivot);
}

void Creature::poseRigid(const Mat4& body, float jaw, float glow, float arms, float legs, float time) {
    if (!valid_) return;
    glow_ = glow;
    jaw_ = jaw;
    const std::vector<Mat4> local = localPose(jaw, arms, legs, -0.6f * std::abs(legs), time);
    for (size_t i = 0; i < bones_.size(); ++i) bones_[i].world = body * local[i];
    updateFeet();
}

void Creature::captureDeath() {
    deathFrom_.clear();
    for (const Bone& b : bones_) deathFrom_.push_back(b.world);
    deathBody_ = bodyMatrix();
}

void Creature::poseDeath(float t, float time) {
    if (!valid_ || deathFrom_.size() != bones_.size()) return;
    const Vec3 c0 = deathBody_.point(bones_[size_t(find("thorax"))].pivot);
    // the frame it lands in: on its back on the floor, forward kept horizontal
    const Vec3 up0 = normalize(column(deathBody_, 1));
    Vec3 fwd = -column(deathBody_, 2);
    fwd = normalize(Vec3{fwd.x, 0, fwd.z});
    // roll about forward until its up points down
    const Vec3 side = cross(fwd, Vec3{0, 1, 0});
    float roll = std::atan2(dot(up0, side), dot(up0, Vec3{0, 1, 0}));  // 0 when upright
    roll = (roll >= 0.0f ? kPi : -kPi) - roll;
    const float spasm = std::clamp(t / 0.35f, 0.0f, 1.0f);
    const float fall = std::clamp((t - 0.3f) / 0.65f, 0.0f, 1.0f);
    const float fallE = fall * fall;  // accelerating, like a drop
    const float settle = std::clamp((t - 0.95f) / 0.25f, 0.0f, 1.0f);
    const float bounce = 0.06f * std::sin(settle * kPi) * (1.0f - settle);
    const Vec3 c1{c0.x, 0.42f, c0.z};
    const Vec3 c = lerp(c0, c1, fallE) + Vec3{0, bounce, 0};
    const Mat4 rot = axisAngle(fwd, roll * std::min(1.0f, fall * 1.2f));
    // body frame now: rotate the death frame about its centre, then move the centre
    Mat4 frame0 = deathBody_;
    Mat4 frame = Mat4::translation(c) * rot * Mat4::translation(-c0) * frame0;
    // shaking while it spasms
    const float shake = (1.0f - fall) * 0.03f * std::sin(time * 47.0f);
    frame = Mat4::translation(Vec3{0, shake, 0}) * frame;
    const float legs = 0.2f + 0.8f * std::clamp((t - 0.2f) / 1.4f, 0.0f, 1.0f);
    const float jaw = t < 0.4f ? 1.0f : std::max(0.25f, 1.0f - (t - 0.4f) * 0.6f);
    const float twitch = t > 1.2f ? std::max(0.0f, 0.25f * std::sin(time * 9.0f) * std::exp(-(t - 1.2f) * 0.8f)) : 0.0f;
    const std::vector<Mat4> local = localPose(jaw, 0.4f, legs + twitch, 1.0f * legs, time);
    const float blend = std::clamp((t - 0.05f) / 0.3f, 0.0f, 1.0f);
    for (size_t i = 0; i < bones_.size(); ++i) {
        const Mat4 rigid = frame * inverseRigid(deathBody_) * deathBody_ * local[i];
        bones_[i].world = blend < 1.0f ? lerpFrame(deathFrom_[i], rigid, blend * blend * (3.0f - 2.0f * blend)) : rigid;
    }
    // the glow flares as it dies, then fades out
    glow_ = std::max(0.0f, (spasm < 1.0f ? 2.2f : 2.2f * std::exp(-(t - 0.35f) * 1.6f)));
    updateFeet();
}

bool Creature::raycast(Vec3 o, Vec3 d, float maxDist, float& tHit, Vec3& nHit) const {
    if (!valid_) return false;
    bool hit = false;
    tHit = maxDist;
    for (const HitSphere& h : hits_) {
        const Vec3 c = bones_[size_t(h.bone)].world.point(h.center);
        const Vec3 oc = o - c;
        const float b = dot(oc, d);
        const float disc = b * b - (dot(oc, oc) - h.radius * h.radius);
        if (disc < 0.0f) continue;
        const float t = -b - std::sqrt(disc);
        if (t > 0.0f && t < tHit) {
            tHit = t;
            nHit = normalize(o + d * t - c);
            hit = true;
        }
    }
    return hit;
}

// The frame of a body point: sOff metres ahead of the creature's origin along the path,
// lifted `height` off the surface, `lateral` metres round the cross-section. X right,
// Y up (away from the surface), -Z forward along the chain.
Mat4 Creature::bodyFrame(const CrawlPath& path, const CreaturePose& p, float sOff, float height, float lateral) const {
    auto at = [&](float sb) {
        const float u = p.uOf(sb) + lateral;
        return path.point(sb, u) + path.normal(sb, u) * height;
    };
    const float sb = p.s + sOff;
    const Vec3 o = at(sb);
    Vec3 fwd = at(sb + 0.08f) - at(sb - 0.08f);
    fwd = length(fwd) > 1e-6f ? normalize(fwd) : path.tangent(sb);
    Vec3 n = path.normal(sb, p.uOf(sb) + lateral);
    Vec3 up = normalize(n - fwd * dot(n, fwd));
    Vec3 right = cross(fwd, up);
    return Mat4::basis(right, up, -fwd, o);
}

void Creature::pose(const CrawlPath& path, const CreaturePose& p) {
    if (!valid_) return;
    glow_ = p.glow;
    jaw_ = p.jaw;
    auto bone =[&](const char* n) -> Bone& { return bones_[size_t(find(n))]; };
    // a slight bob with each step
    const float bob = 0.014f * std::sin(4.0f * kPi * p.s / kStride);

    // ---- body chain ----
    // (crouching sinks it toward the surface; the head and neck go lowest)
    const float sink = 1.0f - 0.42f * p.crouch;
    for (const char* n : kBodyBones) {
        Bone& b = bone(n);
        const bool front = std::string_view(n) == "head" || std::string_view(n) == "neck";
        const float height = (b.pivot.y + bob) * sink - (front ? 0.06f * p.crouch : 0.0f);
        Mat4 f = bodyFrame(path, p, -b.pivot.z, height, b.pivot.x);
        b.world = f * Mat4::translation(-b.pivot);
    }
    // rearing: the thorax, neck and head pivot up about the waist (the middle legs, on the
    // thorax, then reach down to their planted claws)
    if (p.rear > 0.0f) {
        const Bone& w = bone("waist");
        const Vec3 pivot = w.world.point(w.pivot);
        const Vec3 right = normalize(w.world.dir({1, 0, 0}));
        const Mat4 lift = Mat4::translation(pivot) * axisAngle(right, 0.85f * p.rear) * Mat4::translation(-pivot);
        for (const char* n : {"thorax", "neck", "head"}) bone(n).world = lift * bone(n).world;
    }
    // the head turns toward what it is looking at, within limits
    {
        Bone& h = bone("head");
        Mat4 f = h.world * Mat4::translation(h.pivot);
        float yaw = 0.18f * std::sin(p.time * 0.9f), pitch = 0.05f * std::sin(p.time * 1.3f);
        if (p.look) {
            Vec3 d = normalize(p.lookAt - column(f, 3));
            Vec3 x = column(f, 0), y = column(f, 1), z = column(f, 2);
            float lx = dot(d, x), ly = dot(d, y), lz = -dot(d, z);
            yaw = std::clamp(std::atan2(lx, std::max(lz, 0.05f)), -0.75f, 0.75f);
            pitch = std::clamp(std::atan2(ly, std::sqrt(lx * lx + lz * lz)), -0.7f, 0.7f);
        }
        f = f * Mat4::rotationY(-yaw) * Mat4::rotationX(pitch);
        h.world = f * Mat4::translation(-h.pivot);
        head_ = f.point({0, 0, -0.42f});
        // jaws open in a cross
        for (int j = 0; j < 4; ++j) {
            Bone& jb = bone(kJaws[j]);
            Vec3 axis = cross(Vec3{0, 0, -1}, kJawRadial[j]);
            jb.world = h.world * Mat4::translation(jb.pivot) * axisAngle(axis, 0.6f * p.jaw) *
                       Mat4::translation(-jb.pivot);
        }
    }
    center_ = bone("thorax").world.point(bone("thorax").pivot);

    // ---- tail: trails behind, arched, swaying side to side ----
    {
        Vec3 prevWorld = bone("pelvis").world.point(bone("pelvis").pivot);
        Vec3 prevRest = bone("pelvis").pivot;
        for (int k = 0; k < 6; ++k) {
            Bone& b = bone(("tail" + std::to_string(k)).c_str());
            const float w = float(k + 1) / 6.0f;
            const float sway = 0.09f * w * std::sin(p.time * 2.4f - float(k) * 0.75f) +
                               0.05f * w * std::sin(p.s * 2.1f - float(k) * 0.6f);
            Mat4 f = bodyFrame(path, p, -b.pivot.z, (b.pivot.y + 0.025f * float(k) + bob * 0.5f) * sink, b.pivot.x + sway);
            // chained at its rest spacing: where the route also moves round the cross-section
            // (climbing a wall), placing each segment at its own s alone pulled them apart
            Vec3 o = column(f, 3);
            {
                const Vec3 d = o - prevWorld;
                const float dl = length(d), rest = length(b.pivot - prevRest);
                if (dl > 1e-5f) o = prevWorld + d * (rest / dl);
            }
            const Vec3 up = column(f, 1);
            const Vec3 fwdW = normalize(prevWorld - o);
            const Vec3 fwdR = normalize(prevRest - b.pivot);
            const Vec3 rightW = normalize(cross(fwdW, up));
            const Vec3 upW = cross(rightW, fwdW);
            const Vec3 rightR = normalize(cross(fwdR, Vec3{0, 1, 0}));
            const Vec3 upR = cross(rightR, fwdR);
            b.world = Mat4::basis(rightW, upW, -fwdW, o) * inverseRigid(Mat4::basis(rightR, upR, -fwdR, b.pivot));
            prevWorld = o;
            prevRest = b.pivot;
        }
    }

    // ---- forelimbs: folded against the thorax, twitching; raised for the threat ----
    for (const char* side : {"L", "R"}) {
        Bone& arm = bone((std::string("arm_") + side).c_str());
        Bone& fore = bone((std::string("fore_") + side).c_str());
        const Mat4& thorax = bone("thorax").world;
        const float sgn = side[0] == 'L' ? -1.0f : 1.0f;
        const float tw = 0.06f * std::sin(p.time * 1.7f + (side[0] == 'L' ? 0.0f : 2.0f)) + 0.04f * p.jaw;
        Mat4 local = Mat4::translation(arm.pivot) * Mat4::rotationX(tw + 0.9f * p.arms) *
                     Mat4::rotationZ(-0.35f * p.arms * sgn) * Mat4::translation(-arm.pivot);
        arm.world = thorax * local;
        fore.world = arm.world;
    }

    // ---- legs: the gait in s, and two-bone IK ----
    for (int l = 0; l < 4; ++l) {
        const char* ln = kLegNames[l];
        Bone& femur = bone((std::string("femur_") + ln).c_str());
        Bone& tibia = bone((std::string("tibia_") + ln).c_str());
        const Bone& parent = bone(ln[0] == 'M' ? "thorax" : "pelvis");
        const Vec3 H0 = femur.pivot, K0 = tibia.pivot, F0 = restFoot_[size_t(l)];
        const float L1 = length(K0 - H0), L2 = length(F0 - K0);
        const float fwdOff = -F0.z, lat = F0.x;

        // the foothold for step cycle k: where the rest foot lands when the body is
        // half way through that step's stance, pulled in toward the body (round the
        // cross-section) until the hip, as it will be then, can reach it. Depends only on
        // k, so a planted claw never slides.
        const Vec3 parentPivot = parent.pivot;
        auto foothold = [&](float k) {
            const float sMid = (k - kLegPhase[l] + kDuty * 0.5f) * kStride;
            const float sf = sMid + fwdOff;
            // the hip at the start, middle and end of the stance (the body rolls as it climbs)
            Vec3 hips[3];
            const float half = 0.5f * kDuty * kStride + 0.08f;
            for (int i = 0; i < 3; ++i) {
                CreaturePose pm = p;
                pm.s = sMid + half * float(i - 1);
                const Mat4 pf = bodyFrame(path, pm, -parentPivot.z, parentPivot.y, parentPivot.x) *
                                Mat4::translation(-parentPivot);
                hips[i] = pf.point(H0);
            }
            // the claw must be neither out of reach nor folded in under the hip
            const float far = 0.9f * (L1 + L2), near = std::abs(L1 - L2) + 0.12f;
            float u = p.uOf(sf) + lat;
            for (float scale : {1.0f, 0.85f, 1.15f, 0.7f, 1.3f, 0.55f, 1.5f, 0.4f, 1.7f, 0.25f, 1.9f}) {
                u = p.uOf(sf) + lat * scale;
                const Vec3 f = path.point(sf, u);
                float worst = 0.0f, closest = 1e9f;
                for (const Vec3& h : hips) {
                    worst = std::max(worst, length(f - h));
                    closest = std::min(closest, length(f - h));
                }
                if (worst < far && closest > near) break;
            }
            return std::pair<Vec3, Vec3>{path.point(sf, u), path.normal(sf, u)};
        };
        const float c = p.s / kStride + kLegPhase[l];
        const float k = std::floor(c), ph = c - k;
        Vec3 F;
        if (ph < kDuty) {
            F = foothold(k).first;
            planted_[size_t(l)] = true;
        } else {
            const float w = (ph - kDuty) / (1.0f - kDuty);
            auto [a, na] = foothold(k);
            auto [b, nb] = foothold(k + 1.0f);
            const float ws = w * w * (3.0f - 2.0f * w);
            F = a + (b - a) * ws + normalize(na + nb) * (0.13f * std::sin(kPi * w));
            planted_[size_t(l)] = false;
        }
        const Vec3 H = parent.world.point(H0);
        if (!planted_[size_t(l)]) {
            // in the air the claw's arc stays within the leg's reach
            const Vec3 hf = F - H;
            const float dl = length(hf);
            const float clamped = std::clamp(dl, std::abs(L1 - L2) + 0.08f, 0.97f * (L1 + L2));
            if (dl > 1e-5f) F = H + hf * (clamped / dl);
        }
        feet_[size_t(l)] = F;
        const Vec3 bodyUp = normalize(parent.world.dir({0, 1, 0}));
        const Vec3 out = normalize(parent.world.dir({lat > 0 ? 1.0f : -1.0f, 0, 0}));
        Vec3 d = F - H;
        float dist = length(d);
        const Vec3 dn = dist > 1e-5f ? d * (1.0f / dist) : bodyUp * -1.0f;
        dist = std::clamp(dist, std::abs(L1 - L2) + 1e-3f, L1 + L2 - 1e-3f);
        const float a = (L1 * L1 - L2 * L2 + dist * dist) / (2.0f * dist);
        const float h = std::sqrt(std::max(0.0f, L1 * L1 - a * a));
        Vec3 pole = bodyUp + out * 0.45f;
        pole = normalize(pole - dn * dot(pole, dn));
        const Vec3 K = H + dn * a + pole * h;
        const Vec3 Fr = H + dn * dist;  // reachable foot

        const Vec3 n0 = normalize(cross(K0 - H0, F0 - H0));
        const Vec3 n = normalize(cross(K - H, Fr - H));
        femur.world = frameAlong(H, K - H, n) * inverseRigid(frameAlong(H0, K0 - H0, n0));
        tibia.world = frameAlong(K, Fr - K, n) * inverseRigid(frameAlong(K0, F0 - K0, n0));
        if (keepLegs_) {  // (for the climb film's overlay)
            LegState& ls = legState_[size_t(l)];
            const auto [fromP, fromN] = foothold(k);
            const auto [toP, toN] = foothold(k + 1.0f);
            ls = LegState{H, K, Fr, fromP, toP, fromN, toN, planted_[size_t(l)],
                          planted_[size_t(l)] ? 0.0f : (ph - kDuty) / (1.0f - kDuty), ph};
        }
    }

    // a hit jolts the whole body back and the head up; the jaws snap wide
    if (p.flinch > 0.0f) {
        const Bone& th = bone("thorax");
        const Vec3 c = th.world.point(th.pivot);
        const Vec3 fwd = normalize(-th.world.dir({0, 0, 1}));
        const Vec3 right = normalize(th.world.dir({1, 0, 0}));
        const float f = p.flinch;
        const Mat4 jolt = Mat4::translation(c - fwd * (0.16f * f)) * axisAngle(right, 0.3f * f) * Mat4::translation(-c);
        for (Bone& b : bones_) b.world = jolt * b.world;
        Bone& h = bone("head");
        for (int j = 0; j < 4; ++j) {
            Bone& jb = bone(kJaws[j]);
            Vec3 axis = cross(Vec3{0, 0, -1}, kJawRadial[j]);
            jb.world = h.world * Mat4::translation(jb.pivot) * axisAngle(axis, 0.6f * std::min(1.0f, p.jaw + f)) *
                       Mat4::translation(-jb.pivot);
        }
        glow_ = p.glow + 1.2f * f;
        head_ = h.world.point(h.pivot + Vec3{0, 0, -0.42f});
        center_ = c;
    }
}

Vec3 Creature::posedClaw(int leg) const {
    const int i = find(std::string("tibia_") + kLegNames[leg]);
    return bones_[size_t(i)].world.point(restFoot_[size_t(leg)]);
}

void Creature::transformAll(const Mat4& m) {
    if (!valid_) return;
    for (Bone& b : bones_) b.world = m * b.world;
    for (Vec3& f : feet_) f = m.point(f);
    head_ = m.point(head_);
    center_ = m.point(center_);
}

void Creature::rotateAboutCenter(Vec3 axis, float angle) {
    const Vec3 c = center_;
    transformAll(Mat4::translation(c) * axisAngle(normalize(axis), angle) * Mat4::translation(-c));
}

void Creature::appendInstances(std::vector<SceneInstance>& out) const {
    if (!valid_) return;
    const float g = glow_;
    for (const Bone& b : bones_)
        for (uint32_t part : b.parts) out.push_back({part, b.world, 0, {g, g, g}});
}

}  // namespace bh
