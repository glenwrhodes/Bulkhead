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

// How the creatures find the player (round 11). Before it, nothing drew them toward you:
// they wandered 4-9 cells at a time and only noticed you on sight (22 m) or within 5 m.
struct HuntTuning {
    int creatures = 3;           // loose in the ship (window default)
    bool drift = false;          // now and then a roaming one heads for roughly where you are
    float driftMin = 20.0f, driftMax = 40.0f;  // seconds between one creature's drifts
    int driftSpread = 4;         // cells: the goal is this far from you at most, never exact
    float hearWalk = 0.0f;       // metres of walking distance at which it hears footsteps
    float hearRun = 0.0f;        // ... running
    // The game before round 11: three creatures, no drift, no hearing.
    static HuntTuning original() { return HuntTuning{}; }
    // Round 11: tuned with the encounter timer for a first sighting after about a minute.
    static HuntTuning round11() {
        HuntTuning t;
        t.creatures = 10;
        t.drift = true;
        t.driftMin = 15.0f;
        t.driftMax = 30.0f;
        t.hearWalk = 16.0f;
        t.hearRun = 35.0f;
        return t;
    }
};

struct HuntWorld {
    const Maze* maze = nullptr;
    const Level* level = nullptr;
    const std::vector<float>* doorOpen = nullptr;
    Vec3 player;            // eye position
    Vec3 playerForward{0, 0, -1};
    bool playerAlive = true;
    bool shotFired = false; // the player fired this step: loud
    bool grabbable = true;  // false: a leap that reaches you misses (benchmarks)
    float time = 0.0f;
    // round 11: drawn toward the player
    const HuntTuning* tuning = nullptr;
    const std::vector<int>* playerDist = nullptr;  // walking distance in cells from the player
    float noiseRange = 0.0f;                       // metres of walking distance it is heard at
};

class Hunter {
public:
    static constexpr int kHealth = 4;
    static constexpr float kRoamSpeed = 1.1f, kStalkSpeed = 1.35f, kChaseSpeed = 4.0f;
    static constexpr float kDriftSpeed = 1.5f;  // round 11: heading for roughly where you are
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
    // What it is on: 0 floor, 1 a wall, 2 the ceiling, 3 in the air (leaping), -1 dead or feeding.
    int surfaceKind() const;
    void appendInstances(std::vector<SceneInstance>& out) const { creature_.appendInstances(out); }
    float glow() const { return glow_; }
    int drifts() const { return drifts_; }  // drift legs started (the encounter timer reports them)
    // Leaps that reached a player who could not be grabbed (benchmarks).
    int reachedPlayer() const { return reachedPlayer_; }
    // Times it has turned round on its path. Known issue (since round 9): the body snaps round
    // in one step (the new path starts where it stands, so the parts behind the head pile up
    // there and unfold as it moves off). It needs a real turning animation.
    int uTurns() const { return uTurns_; }
    // Why its path last changed (for tests): "spawn", "roam", "drift", "heard", "alert", "chase".
    const char* lastRoute() const { return lastRoute_; }

private:
    void setState(HunterState s);
    // New path from the cell it is in to goal; surfacePref per cell as for the state.
    bool route(const Maze& maze, Int2 goal, bool allowReverse = true);
    // Round 11 (drift, hearing): keeps the path it is on up to and including the next cell
    // and branches off from there, so the new bend starts metres ahead and nothing under it
    // moves. Never turns round. False if the goal is behind it or unreachable.
    bool routeAhead(const Maze& maze, Int2 goal);
    // avoidDeadEnds (round 11 tuning): never a corridor closet, which means turning round.
    // ahead: called one cell before the end of the leg (see Hunter.cpp); false if no goal
    // could be reached without turning round.
    bool chooseRoamGoal(const Maze& maze, const Level& level, bool avoidDeadEnds = false, bool ahead = false);
    // Round 11: a roaming leg toward roughly where the player is (never their exact cell).
    bool chooseDriftGoal(const Maze& maze, const HuntWorld& w);
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
    // round 11: drifting toward the player, and going to look where it heard footsteps
    float driftTimer_ = -1.0f;  // < 0: not armed yet
    float hearCooldown_ = 0.0f;
    bool investigate_ = false;  // heard you: heads for investigateCell_ from the next cell centre
    Int2 investigateCell_{};
    bool drifting_ = false;     // on a drift leg (a little quicker than wandering)
    int drifts_ = 0;
    int reachedPlayer_ = 0;
    const char* lastRoute_ = "spawn";
    int uTurns_ = 0;
    // leap
    Mat4 leapFrom_;
    Vec3 leapTarget_;
    Vec3 leapFwd_;
};

}  // namespace bh
