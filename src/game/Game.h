#pragma once
// Game state shared by the window and the capture modes: the generated level, the
// player (keyboard + mouse, collision, crouching), doors that open as you approach, the
// lights of every section changing over time, the flashlight, the cells the player has
// seen (for the map), spark bursts and steam jets, the gun (firing bolts, recoil, the
// muzzle flash, impacts and scorch marks), the scripted walk from start to exit, and the
// per-frame list of instances to render. No D3D12 here.

#include <string>
#include <vector>

#include "assets/Kit.h"
#include "core/Math.h"
#include "game/Creature.h"
#include "game/Effects.h"
#include "world/CrawlSurface.h"
#include "world/Level.h"
#include "world/Maze.h"
#include "world/WalkPath.h"

namespace bh {

enum InstanceFlags : uint32_t {
    kInstanceGun = 1u,  // drawn with the camera (gun, arms, muzzle flash): no shadows, fixed history
    kInstanceFx = 2u,   // sparks and bolts: seen directly, ignored by shadow and bounce rays
};

struct SceneInstance {
    uint32_t part = 0;   // Kit::parts index
    Mat4 transform;
    uint32_t flags = 0;
    Vec3 emissive{1, 1, 1};  // multiplies the part's emissive materials (power of its cell)
};

struct InputState {
    bool forward = false, back = false, left = false, right = false, run = false;
    bool crouch = false;
    bool fire = false;   // held: fires whenever the gun is ready
    float mouseDx = 0.0f, mouseDy = 0.0f;  // pixels since last frame
};

struct Shot {
    std::string name;
    Camera camera;
};

// A spot light carried under the gun barrel.
struct Flashlight {
    bool on = false;
    Vec3 position;
    Vec3 direction{0, 0, -1};
    float intensity = 0.0f;  // W/sr on the axis
    float cosInner = 1.0f;   // full brightness inside this cone
    float cosOuter = 0.9f;   // beam edge
};

class Game {
public:
    Game(const Kit& kit, const MazeParams& params);

    const Maze& maze() const { return maze_; }
    const Level& level() const { return level_; }
    const WalkPath& walk() const { return walk_; }

    // Window mode
    void update(float dt, const InputState& input);
    // Capture mode: put the camera at time t (seconds) on the scripted walk. The walk
    // slows down and crouches under low pipes.
    void setWalkTime(float t);
    float walkDuration() const;
    void setCamera(const Camera& c);
    // Sets the clock that drives flickering lights and effects (setWalkTime and update also do).
    void setTime(float t);
    float time() const { return time_; }

    // Advances the doors toward open/closed given the camera position.
    void updateDoors(float dt);
    void openDoorsNear(Vec3 p, float radius);  // snaps doors open (for still shots)

    const Camera& camera() const { return camera_; }
    bool reachedExit() const;
    float bobPhase() const { return bob_; }
    Int2 cameraCell() const;
    Mood moodAtCamera() const;
    float crouch() const { return crouch_; }  // 0 standing .. 1 crouched
    // How crouched the scripted walk is at distance s along it (1 under low pipes).
    float walkCrouchAt(float s) const;

    // Current radiance of every Level light (flicker, dead, emergency pulse applied).
    const std::vector<Vec3>& lightRadiance() const { return lightRadiance_; }
    // How much the lighting near the camera changed in the last update (0 = steady).
    // The window drops temporal history when this is large, so flicker stays crisp.
    float lightingChange() const { return lightingChange_; }

    void setFlashlight(bool on) { flashlightOn_ = on; }
    bool flashlightOn() const { return flashlightOn_; }
    Flashlight flashlight() const;

    // Fires a bolt at whatever the crosshair is on, if the gun is ready. Returns true if it fired.
    bool fire();
    int shotsFired() const { return shotsFired_; }
    int scorchCount() const { return int(scorches_.size()); }
    float sinceLastShot() const { return time_ - lastShot_; }
    Vec3 muzzlePosition() const;

    // Effects for the renderer, as of the last update / setTime / setWalkTime.
    const std::vector<DynamicLight>& dynamicLights() const { return dynamic_; }
    const std::vector<SteamPlume>& steamPlumes() const { return steam_; }
    int sparksVisible() const { return sparkCount_; }

    // Cells the player has seen (1 = seen), y * width + x. Feeds the minimap and map.
    const std::vector<uint8_t>& explored() const { return explored_; }

    void buildInstances(std::vector<SceneInstance>& out, bool withGun = true) const;
    std::vector<Shot> findShots() const;

    // ---- the creature (round 8) ----
    // The crawl scene (capture --scene crawl): standing near one end of a long straight
    // corridor while the creature comes in from a room or side passage beyond the far
    // end, crawls up the corridor on the floor, climbs the right wall, pauses to look,
    // then crosses the ceiling overhead and passes behind. Returns false if the ship has
    // no corridor long enough.
    bool setupCrawlScene();
    float crawlDuration() const { return crawl_.duration; }
    void setCrawlTime(float t);
    bool creatureActive() const { return creatureOn_ && creature_.valid(); }
    const Creature& creature() const { return creature_; }
    const CrawlPath& crawlPath() const { return crawlPath_; }
    // Where round the corridor the scene's creature is at distance s (0 floor, ... ceiling).
    float crawlU(float s) const;
    float crawlS(float t) const;  // distance along its path at scene time t

    static constexpr float kWalkSpeed = 2.6f;    // scripted walk, m/s
    static constexpr float kCrouchWalkSpeed = 1.25f;
    static constexpr float kStartPause = 0.6f;
    static constexpr float kEndPause = 1.8f;
    static constexpr float kFireInterval = 0.28f;
    static constexpr float kBoltSpeed = 70.0f;   // m/s
    static constexpr int kMaxScorches = 48;
    // Footsteps per second at a ground speed (m/s). One vertical bob per step.
    static float stepRate(float speed) { return 1.55f + 0.17f * speed; }

private:
    struct Bolt {
        Vec3 origin, dir;
        float t0 = 0.0f, dist = 0.0f;
        Vec3 normal;
    };
    struct Impact {
        Vec3 position, normal;
        float t0 = 0.0f;
        uint32_t seed = 0;
    };
    struct Scorch {
        Vec3 position, normal;
        float roll = 0.0f;
        float size = 1.0f;
    };

    void updateLighting();
    void updateEffects();
    void reveal();
    void buildWalkTiming();
    float walkDistanceAt(float t) const;  // t: seconds into the walk after the start pause
    Mat4 gunMatrix() const;
    void addSparks(std::vector<SceneInstance>& out, Vec3 origin, Vec3 dir, float age, uint32_t seed, int count,
                   float speed, float spread) const;

    const Kit& kit_;
    Maze maze_;
    Level level_;
    WalkPath walk_;
    Camera camera_;
    Vec2 velocity_;
    float bob_ = 0.0f;
    float bobAmount_ = 0.0f;
    float crouch_ = 0.0f;
    float time_ = 0.0f;
    std::vector<float> doorOpen_;
    std::vector<Vec3> cellLight_, cellEmissive_, lightRadiance_;
    float lightingChange_ = 0.0f;
    std::vector<uint8_t> explored_;
    bool flashlightOn_ = true;
    // scripted walk timing: distance -> time, with slow crouched stretches
    std::vector<float> walkS_, walkT_, walkC_;
    // gun
    float lastShot_ = -100.0f;
    int shotsFired_ = 0;
    std::vector<Bolt> bolts_;
    std::vector<Impact> impacts_;
    std::vector<Scorch> scorches_;
    // effects
    std::vector<DynamicLight> dynamic_;
    std::vector<SteamPlume> steam_;
    std::vector<int> nearEmitters_;
    int sparkCount_ = 0;
    // the creature and its scene
    Creature creature_;
    CrawlPath crawlPath_;
    bool creatureOn_ = false;
    struct CrawlScene {
        std::vector<Int2> run;           // the straight corridor, far end first
        Vec3 cameraPos, farPoint;
        float sRunStart = 0, sWall0 = 0, sCeil0 = 0, sPause = 0, sCam = 0, sEnd = 0, s0 = 0;
        std::vector<float> tableS, tableT;  // distance -> time
        float duration = 0.0f;
    } crawl_;
    CreaturePose crawlPose(float t) const;
    // kit models
    int gunModel_ = -1, armsModel_ = -1, flashModel_ = -1, boltModel_ = -1, sparkModel_ = -1, scorchModel_ = -1;
    Vec3 muzzleLocal_{0, 0.022f, -0.168f};
};

}  // namespace bh
