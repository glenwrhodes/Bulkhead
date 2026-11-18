#pragma once
// Game state shared by the window and the capture modes: the generated level, the
// player (keyboard + mouse, collision, crouching), doors that open as you approach, the
// lights of every section changing over time, the flashlight, the cells the player has
// seen (for the map), spark bursts and steam jets, the gun (firing bolts, recoil, the
// muzzle flash, impacts and scorch marks), the scripted walk from start to exit, and the
// per-frame list of instances to render. No D3D12 here.

#include <string>
#include <vector>

#include <array>

#include "assets/Kit.h"
#include "audio/Sounds.h"
#include "core/Math.h"
#include "game/Creature.h"
#include "game/Effects.h"
#include "game/Hunter.h"
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
    const std::vector<float>& doorOpen() const { return doorOpen_; }  // per Level door, 0 shut .. 1 open

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

    // ---- the climb (round 15) ----
    // The crawl as a showcase (capture --scene climb, ~25 s): along a lit straight corridor
    // the creature crawls on the floor, up the right wall, slowly through the edge where
    // wall meets ceiling, along the ceiling, down the left wall and back to the floor,
    // filmed from six close setups that cut on its progress. False if no corridor is long
    // enough. The overlay version draws the system over the same frames (CrawlOverlay).
    bool setupClimbScene();
    float climbDuration() const { return climb_.duration; }
    void setClimbTime(float t);
    struct ClimbInfo {
        float s = 0.0f;               // the creature's place on its path
        const char* stage = "";       // floor, wall, the edge, ceiling, ...
        int shot = 0;                 // which camera setup
    };
    ClimbInfo climbInfo() const;
    float climbU(float s) const;      // round the cross-section at s
    void keepCreatureLegs(bool on) { creature_.setKeepLegs(on); }
    Mood climbMood() const { return climb_.mood; }  // the corridor's mood (at its middle)

    // ---- sound (part 3) ----
    // Call once a frame, after the game has stepped (window, captures alike): the one-shots
    // since the last call (footsteps, the gun and its hits, claws landing, hisses, screeches,
    // doors, sparks, distant groans), each with how hidden it is behind walls, and the loops
    // sounding now (the ship's hum, steam jets, skittering, feeding). The first call only
    // takes stock and returns no one-shots.
    void collectSounds(std::vector<SoundEvent>& events, std::vector<SoundLoop>& loops);
    Listener listener() const;

    // ---- viewports (part 3) ----
    // The window scene (capture --scene window, ~17 s): walk up a corridor to a viewport on
    // the ship's edge and look out at the stars; the creature crawls in behind you along the
    // ceiling and down the opposite wall, seen in the glass; pan to the other viewport, the
    // flashlight sweeping across the glass; turn round to face it, hissing on the wall.
    // False if no viewport has two corridor cells on one side (or the kit has no creature).
    bool setupWindowScene();
    float windowSceneLength() const { return window_.length; }
    void setWindowTime(float t);

    // ---- hunters (rounds 9-10) ----
    // Puts `count` creatures loose in the ship, in the dim, dark and derelict sections
    // away from the player. They roam, stalk, chase, leap and kill; four hits kill one.
    // `salt` varies where they start (0: the window's choice); restarts keep it.
    void spawnHunters(int count, uint64_t salt = 0);
    const std::vector<Hunter>& hunters() const { return hunters_; }
    // Advances everything but the player: doors, lights, hunters, effects, the death scene.
    void stepWorld(float dt);
    // Puts the player's eye somewhere without the window's movement code (the encounter
    // timer's simulated player). stepWorld then reveals the map around it.
    void movePlayer(const Camera& c);
    // Benchmarks: the creatures hunt the scripted walk but can never grab it (a leap that
    // reaches you lands and comes again). stepCreatures steps only them and their lights,
    // so setWalkTime keeps the clock.
    void setInvulnerable(bool on) { invulnerable_ = on; }
    void stepCreatures(float dt);
    // How the creatures find the player (round 11); see HuntTuning.
    void setHuntTuning(const HuntTuning& t) { tuning_ = t; }
    const HuntTuning& huntTuning() const { return tuning_; }
    // How much noise the player makes: 0 still, 1 walking, 2 running (window: from the keys).
    void setPlayerNoise(int n) { noise_ = n; }
    // The player's death: a hunter has you. 0 alive; seconds since it grabbed you.
    bool playerDead() const { return deathT_ >= 0.0f; }
    float deathTime() const { return deathT_; }
    float redout() const;    // 0..1 red over the screen
    float blackout() const;  // 0..1 fade to black
    static constexpr float kDeathLength = 4.6f;  // then the window restarts the run
    void restart();          // back to the start: player, hunters, doors
    int kills() const { return kills_; }

    // Filmed scenes driven by the real AI at fixed steps (capture --scene hunt|fight).
    // hunt: you walk a dark corridor, it stalks you on the ceiling, you back off and hit
    //       it twice, it charges, leaps, and you die.
    // fight: you hold a corridor while one charges; four hits and it dies.
    // approach (round 13, capture --scene approach): you stand still and don't shoot; one
    // that has spotted you closes in and leaps (the before/after film of the approach).
    // threat (capture --scene threat): the approach filmed for the threat itself: a lit
    // corridor (no derelict sparks), the creature kept to the ceiling, charging at once.
    bool setupHuntScene(bool fight, bool approach = false, bool threatShot = false);
    void stepHuntScene(float t, float dt);
    float huntSceneLength() const { return scene_.length; }
    bool huntSceneDone(float t) const { return scene_.endAt >= 0.0f && t >= scene_.endAt; }

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
        int hunter = -1;  // >= 0: it hits this creature
    };
    struct Impact {
        Vec3 position, normal;
        float t0 = 0.0f;
        uint32_t seed = 0;
        bool creature = false;
    };
    struct HuntScene {
        bool fight = false;
        bool approach = false;
        bool threatShot = false;
        std::vector<Int2> run;      // the corridor, player's end first
        Vec3 dir;                   // along the run, away from the player's end
        Vec3 start;                 // player's eye at t = 0
        float length = 0.0f;
        float yaw = 0.0f, pitch = 0.0f;
        float backed = 0.0f;        // how far the player has backed off
        float nextShot = 0.0f;
        int shots = 0;
        float endAt = -1.0f;
    } scene_;
    HuntWorld huntWorld(bool shot) const;
    void spawnHunterAt(Int2 cell, int surface, uint64_t seed);
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
    Vec2 sway_;  // sideways camera sway (x, z) drawn last step: a view offset only, never part of the position
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
    // hunters and the player's death
    std::vector<Hunter> hunters_;
    int huntersWanted_ = 0;
    uint64_t spawnSalt_ = 0;
    bool invulnerable_ = false;
    HuntTuning tuning_;
    int noise_ = 0;
    std::vector<int> playerDist_;  // walking distance (cells) from the player's cell, for hearing
    Int2 playerDistFrom_{-1, -1};
    void stepHunters(float dt);
    std::vector<float> respawnAt_;
    float deathT_ = -1.0f;
    int killer_ = -1;
    Camera deathCam_;
    bool shotThisStep_ = false;
    int kills_ = 0;
    uint64_t spawnCounter_ = 0;
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
    // sound: what is pushed as it happens (the gun, hits), and what collectSounds compares
    // with the last frame to find the rest
    std::vector<SoundEvent> pendingSounds_;
    void sound(Snd s, Vec3 pos, float gain = 1.0f, float pitch = 1.0f, bool positional = true);
    float occlusionTo(Vec3 p) const;
    struct CreatureEars {
        std::array<bool, 4> planted{};
        float jaw = 0.0f, hissAgain = 0.0f;
        HunterState state = HunterState::Roam;
        int claw = 0;
    };
    bool soundStarted_ = false;
    float soundTime_ = 0.0f, lastBob_ = 0.0f, stepAcc_ = 0.0f;
    Vec3 lastCam_;
    int steps_ = 0;
    std::vector<float> lastDoor_;
    std::vector<CreatureEars> hunterEars_;
    CreatureEars crawlEars_;
    bool wasDead_ = false;
    void listenToCreature(const Creature& c, CreatureEars& ears, bool scripted, float dt);
    struct WindowScene {
        Int2 cell;
        Vec3 C, A, O;          // cell centre (floor), along the corridor (the creature's way), out
        Vec3 v1, v2;           // the two viewports' centres on the glass (v1 on the -A side)
        float sStart = 0, sDesc = 0, sStop = 0, uWall = 0;
        float creatureFrom = 5.0f, moveFrom = 5.0f, speed = 1.05f;  // there from, crawling from
        float length = 17.0f;
    } window_;
    float windowU(float s) const;
    struct ClimbScene {
        std::vector<float> keyS, keyU;    // u at each key (eased between)
        std::vector<int> keyStage;        // the stage from each key to the next
        std::vector<float> tableS, tableT;  // distance -> time
        float s0 = 0.0f, sEnd = 0.0f, duration = 0.0f;
        Vec3 target;                      // where the camera looks (eased)
        float lastT = -1.0f;
        int lastShot = -1;
        Mood mood = Mood::Lit;
    } climb_;
    float climbS(float t) const;
    int climbStage(float s) const;
    // kit models
    int gunModel_ = -1, armsModel_ = -1, flashModel_ = -1, boltModel_ = -1, sparkModel_ = -1, scorchModel_ = -1;
    Vec3 muzzleLocal_{0, 0.022f, -0.168f};
};

}  // namespace bh
