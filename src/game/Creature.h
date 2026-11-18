#pragma once
// The creature, posed procedurally from its rigid parts (round 8).
//
// The kit model "creature" has one part per bone (creature_<bone>) in the rest pose and
// markers for the rest pivots (fx_bone_<bone>) and claw tips (fx_foot_<leg>). Each
// frame the creature is placed on a CrawlPath at distance s, with u(s) saying where
// round the corridor's cross-section it is at each point along the path (floor, wall,
// ceiling). Because u is a function of s, the head climbs first and the body follows.
//
//   body     pelvis, waist, thorax, neck, head: each sits on the surface at its own s,
//            lifted by its rest height along the surface normal, facing along the
//            chain they make
//   tail     trails the pelvis, arched and swaying
//   legs     a gait in s, not time (the same distance always gives the same step):
//            diagonal pairs alternate; a planted claw stays put while the body moves,
//            then swings in an arc to the next foothold. Two-bone IK, knees up and out.
//   head     turns toward a look target (limited), jaws open by `jaw`
//
// No D3D12 here.

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "assets/Kit.h"
#include "core/Math.h"
#include "world/CrawlSurface.h"

namespace bh {

struct SceneInstance;

struct CreaturePose {
    float s = 0.0f;                         // distance along the path
    std::function<float(float)> uOf;        // u at a given s (round the cross-section)
    float time = 0.0f;                      // seconds (tail sway, breathing)
    float jaw = 0.0f;                       // 0 closed .. 1 wide open
    float glow = 0.7f;                      // amber seams, 0 dark .. 1 hunting
    bool look = false;                      // turn the head toward lookAt
    Vec3 lookAt;
    float flinch = 0.0f;                    // 0..1: jolted back by a hit
    // round 13: the threat and the coil before a leap
    float rear = 0.0f;                      // 0..1: the front lifts off the surface (up to ~49 degrees)
    float crouch = 0.0f;                    // 0..1: the body sinks toward the surface, knees bent
    float arms = 0.0f;                      // 0..1: the scythes raised (as poseRigid's arms)
};

class Creature {
public:
    static constexpr float kStride = 0.55f;  // metres the body moves per step cycle
    static constexpr float kDuty = 0.62f;    // fraction of a cycle a foot is planted

    explicit Creature(const Kit& kit);
    bool valid() const { return valid_; }

    // Poses every part on the path.
    void pose(const CrawlPath& path, const CreaturePose& p);
    // The whole creature as one rigid body: `body` maps the rest model into the world.
    // arms 0..1 raises the scythes, legs > 0 curls the legs in, < 0 throws them forward.
    void poseRigid(const Mat4& body, float jaw, float glow, float arms, float legs, float time);
    // The current body frame (rest model -> world), as poseRigid takes it.
    Mat4 bodyMatrix() const;
    // Dying: snapshot the current pose, then poseDeath(t) spasms, drops to the floor on
    // its back, curls its legs and lets its glow die.
    void captureDeath();
    void poseDeath(float t, float time);
    void appendInstances(std::vector<SceneInstance>& out) const;
    // Moves the whole posed creature rigidly.
    void transformAll(const Mat4& m);
    // Swings the posed creature round its middle (turning round on the spot).
    void rotateAboutCenter(Vec3 axis, float angle);
    // A ray against the creature's hit spheres. Returns true with the distance and normal.
    bool raycast(Vec3 o, Vec3 d, float maxDist, float& t, Vec3& n) const;

    Vec3 headPosition() const { return head_; }
    float jaw() const { return jaw_; }  // as last posed (0 shut .. 1 wide): the game hisses on it
    Vec3 bodyCenter() const { return center_; }
    // Where each claw is (ML, MR, HL, HR) and whether it is planted this frame.
    Vec3 foot(int leg) const { return feet_[size_t(leg)]; }
    bool planted(int leg) const { return planted_[size_t(leg)]; }
    // The claw tip as the posed tibia actually puts it (for tests: IK keeps limb lengths).
    Vec3 posedClaw(int leg) const;
    static const char* legName(int leg);
    Vec3 restPivot(const char* bone) const;

    // What pose() worked out for each leg, kept when setKeepLegs(true) (the climb film's
    // overlay draws it): the hip, the solved knee and claw, the foothold the claw is on or
    // left (from) and the next one (to) with their surface normals, whether it is planted,
    // and how far through its swing it is (0..1, planted: 0).
    struct LegState {
        Vec3 hip, knee, claw, from, to, nFrom, nTo;
        bool planted = false;
        float swing = 0.0f;
        float cycle = 0.0f;  // where it is in its step cycle (0..1; planted below kDuty)
    };
    void setKeepLegs(bool on) { keepLegs_ = on; }
    const std::array<LegState, 4>& legs() const { return legState_; }
    // Blend two rigid frames (positions lerped, axes lerped and re-orthonormalised).
    static Mat4 blendFrames(const Mat4& a, const Mat4& b, float t);

private:
    struct Bone {
        std::string name;
        Vec3 pivot;                  // rest, model space
        std::vector<uint32_t> parts;
        Mat4 world;                  // this frame
    };
    struct HitSphere {
        int bone;
        Vec3 center;  // rest, model space
        float radius;
    };
    int find(const std::string& name) const;
    Mat4 bodyFrame(const CrawlPath& path, const CreaturePose& p, float sOff, float height, float lateral) const;
    // Rest-pose bone matrices (model space) with the arms, legs, jaws and tail posed.
    std::vector<Mat4> localPose(float jaw, float arms, float legs, float tailCurl, float time) const;
    void updateFeet();

    bool valid_ = false;
    int model_ = -1;
    std::vector<Bone> bones_;
    std::vector<HitSphere> hits_;
    std::vector<Mat4> deathFrom_;
    Mat4 deathBody_;
    std::array<Vec3, 4> restFoot_{};
    std::array<Vec3, 4> feet_{};
    std::array<bool, 4> planted_{};
    Vec3 head_, center_;
    float glow_ = 0.7f;
    float jaw_ = 0.0f;
    bool keepLegs_ = false;
    std::array<LegState, 4> legState_{};
};

}  // namespace bh
