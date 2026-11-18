#pragma once
// One creature loose in the ship (rounds 9-10): where it goes, what it crawls on, and
// what it does to the player.
//
//   Roam    wanders to cells a few corridors away, mostly in the dark, on whatever
//           surface it fancies (ceiling, wall or floor)
//   Stalk   it has seen or heard you: it closes in along the ceiling, slowly, jaws
//           working, glow brightening
//   Chase   it rushes you, dropping to the floor for the last few metres
//   Leap    within ~3.3 m with a clear line: a 0.42 s lunge at your face
//   Feed    it has you (the game plays the death scene around it)
//   Dying   four hits: it spasms, falls on its back, curls up and goes dark
//   Dead    a husk on the floor
//
// It moves along a CrawlPath through maze cells, choosing a surface per cell (rooms,
// door crossings and junctions limit the choice). It re-routes only at cell centres, so
// the path never jumps under it. Everything is a pure function of the steps it is given
// and its seed, so captures are repeatable. No D3D12 here.

#include <cstdint>
#include <vector>

#include "assets/Kit.h"
#include "core/Math.h"
#include "game/Creature.h"
#include "world/CrawlSurface.h"
#include "world/Level.h"
#include "world/Maze.h"
#include "world/Random.h"

namespace bh {

enum class HunterState : uint8_t { Roam, Stalk, Chase, Leap, Feed, Dying, Dead };
const char* toString(HunterState s);

struct HuntWorld {
    const Maze* maze = nullptr;
    const Level* level = nullptr;
    const std::vector<float>* doorOpen = nullptr;
    Vec3 player;            // eye position
    Vec3 playerForward{0, 0, -1};
    bool playerAlive = true;
    bool shotFired = false; // the player fired this step: loud
    float time = 0.0f;
};

class Hunter {
public:
    static constexpr int kHealth = 4;
    static constexpr float kRoamSpeed = 1.1f, kStalkSpeed = 1.35f, kChaseSpeed = 4.0f;
    static constexpr float kLeapRange = 3.3f, kLeapTime = 0.42f;

    Hunter(const Kit& kit, uint64_t seed);

    // Put it in a cell (on the given surface: 0 floor, 1 wall, 2 ceiling), roaming.
    void spawn(const Maze& maze, Int2 cell, int surface);
    void update(float dt, const HuntWorld& w);
    // Knows where you are: stalks, or charges at once.
    void alert(const HuntWorld& w, bool charge);

    // Shooting
    bool raycast(Vec3 o, Vec3 d, float maxDist, float& t, Vec3& n) const;
    void hit(const HuntWorld& w);
    bool alive() const { return state_ != HunterState::Dying && state_ != HunterState::Dead; }
    int health() const { return health_; }
    float flinch() const { return flinch_; }

    HunterState state() const { return state_; }
    float stateTime() const { return stateTime_; }
    bool grabbedPlayer() const { return state_ == HunterState::Feed; }
    // While feeding, the game poses it on the player's face with this.
    void poseOnFace(const Camera& cam, float t, float time);
    float deadFor() const { return state_ == HunterState::Dead || state_ == HunterState::Dying ? stateTime_ : 0.0f; }

    const Creature& creature() const { return creature_; }
    Vec3 headPosition() const { return creature_.headPosition(); }
    Vec3 bodyCenter() const { return creature_.bodyCenter(); }
    Int2 cell() const;
    float surfaceU() const;  // round the cross-section where it is now
    void appendInstances(std::vector<SceneInstance>& out) const { creature_.appendInstances(out); }
    float glow() const { return glow_; }

private:
    void setState(HunterState s);
    // New path from the cell it is in to goal; surfacePref per cell as for the state.
    bool route(const Maze& maze, Int2 goal, bool allowReverse = true);
    void chooseRoamGoal(const Maze& maze, const Level& level);
    float uOf(float s) const;
    float uForSurface(int surface) const;
    bool canSee(const HuntWorld& w) const;
    void pose(const HuntWorld& w, float dt);

    Creature creature_;
    Rng rng_;
    HunterState state_ = HunterState::Roam;
    float stateTime_ = 0.0f;
    int health_ = kHealth;
    float flinch_ = 0.0f;
    float glow_ = 0.7f, jaw_ = 0.0f;
    // the path
    std::vector<Int2> cells_;
    std::vector<float> cellS_, cellU_;
    CrawlPath path_;
    float s_ = 0.0f;
    int surfacePref_ = 2;
    float senseTimer_ = 0.0f;
    float stalkTime_ = 0.0f;
    // leap
    Mat4 leapFrom_;
    Vec3 leapTarget_;
    Vec3 leapFwd_;
};

}  // namespace bh
