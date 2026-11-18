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
};

class Creature {
public:
    static constexpr float kStride = 0.55f;  // metres the body moves per step cycle
    static constexpr float kDuty = 0.62f;    // fraction of a cycle a foot is planted

    explicit Creature(const Kit& kit);
    bool valid() const { return valid_; }

    // Poses every part on the path.
    void pose(const CrawlPath& path, const CreaturePose& p);
    void appendInstances(std::vector<SceneInstance>& out) const;

    Vec3 headPosition() const { return head_; }
    Vec3 bodyCenter() const { return center_; }
    // Where each claw is (ML, MR, HL, HR) and whether it is planted this frame.
    Vec3 foot(int leg) const { return feet_[size_t(leg)]; }
    bool planted(int leg) const { return planted_[size_t(leg)]; }
    // The claw tip as the posed tibia actually puts it (for tests: IK keeps limb lengths).
    Vec3 posedClaw(int leg) const;
    static const char* legName(int leg);

private:
    struct Bone {
        std::string name;
        Vec3 pivot;                  // rest, model space
        std::vector<uint32_t> parts;
        Mat4 world;                  // this frame
    };
    int find(const std::string& name) const;
    Mat4 bodyFrame(const CrawlPath& path, const CreaturePose& p, float sOff, float height, float lateral) const;

    bool valid_ = false;
    int model_ = -1;
    std::vector<Bone> bones_;
    std::array<Vec3, 4> restFoot_{};
    std::array<Vec3, 4> feet_{};
    std::array<bool, 4> planted_{};
    Vec3 head_, center_;
    float glow_ = 0.7f;
};

}  // namespace bh
