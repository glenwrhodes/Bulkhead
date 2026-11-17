#pragma once
// Game state shared by the window and the capture modes: the generated level, the
// player (keyboard + mouse, collision), doors that open as you approach, the scripted
// walk from start to exit, and the per-frame list of instances to render.
// No D3D12 here.

#include <string>
#include <vector>

#include "assets/Kit.h"
#include "core/Math.h"
#include "world/Level.h"
#include "world/Maze.h"
#include "world/WalkPath.h"

namespace bh {

enum InstanceFlags : uint32_t { kInstanceGun = 1u };

struct SceneInstance {
    uint32_t part = 0;   // Kit::parts index
    Mat4 transform;
    uint32_t flags = 0;
};

struct InputState {
    bool forward = false, back = false, left = false, right = false, run = false;
    float mouseDx = 0.0f, mouseDy = 0.0f;  // pixels since last frame
};

struct Shot {
    std::string name;
    Camera camera;
};

class Game {
public:
    Game(const Kit& kit, const MazeParams& params);

    const Maze& maze() const { return maze_; }
    const Level& level() const { return level_; }
    const WalkPath& walk() const { return walk_; }

    // Window mode
    void update(float dt, const InputState& input);
    // Capture mode: put the camera at time t (seconds) on the scripted walk.
    void setWalkTime(float t);
    float walkDuration() const;
    void setCamera(const Camera& c);

    // Advances the doors toward open/closed given the camera position.
    void updateDoors(float dt);
    void openDoorsNear(Vec3 p, float radius);  // snaps doors open (for still shots)

    const Camera& camera() const { return camera_; }
    bool reachedExit() const;
    float bobPhase() const { return bob_; }

    void buildInstances(std::vector<SceneInstance>& out, bool withGun = true) const;
    std::vector<Shot> findShots() const;

    static constexpr float kWalkSpeed = 2.6f;    // scripted walk, m/s
    static constexpr float kStartPause = 0.6f;
    static constexpr float kEndPause = 1.8f;

private:
    const Kit& kit_;
    Maze maze_;
    Level level_;
    WalkPath walk_;
    Camera camera_;
    Vec2 velocity_;
    float bob_ = 0.0f;
    float bobAmount_ = 0.0f;
    std::vector<float> doorOpen_;
    int gunModel_ = -1;
};

}  // namespace bh
