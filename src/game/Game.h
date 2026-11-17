#pragma once
// Game state shared by the window and the capture modes: the generated level, the
// player (keyboard + mouse, collision), doors that open as you approach, the lights of
// every section changing over time, the flashlight, the cells the player has seen (for
// the map), the scripted walk from start to exit, and the per-frame list of instances to
// render. No D3D12 here.

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
    Vec3 emissive{1, 1, 1};  // multiplies the part's emissive materials (power of its cell)
};

struct InputState {
    bool forward = false, back = false, left = false, right = false, run = false;
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
    // Capture mode: put the camera at time t (seconds) on the scripted walk.
    void setWalkTime(float t);
    float walkDuration() const;
    void setCamera(const Camera& c);
    // Sets the clock that drives flickering lights (setWalkTime and update also do).
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

    // Current radiance of every Level light (flicker, dead, emergency pulse applied).
    const std::vector<Vec3>& lightRadiance() const { return lightRadiance_; }
    // How much the lighting near the camera changed in the last update (0 = steady).
    // The window drops temporal history when this is large, so flicker stays crisp.
    float lightingChange() const { return lightingChange_; }

    void setFlashlight(bool on) { flashlightOn_ = on; }
    bool flashlightOn() const { return flashlightOn_; }
    Flashlight flashlight() const;

    // Cells the player has seen (1 = seen), y * width + x. Feeds the minimap and map.
    const std::vector<uint8_t>& explored() const { return explored_; }

    void buildInstances(std::vector<SceneInstance>& out, bool withGun = true) const;
    std::vector<Shot> findShots() const;

    static constexpr float kWalkSpeed = 2.6f;    // scripted walk, m/s
    static constexpr float kStartPause = 0.6f;
    static constexpr float kEndPause = 1.8f;
    // Footsteps per second at a ground speed (m/s). One vertical bob per step.
    static float stepRate(float speed) { return 1.55f + 0.17f * speed; }

private:
    void updateLighting();
    void reveal();

    const Kit& kit_;
    Maze maze_;
    Level level_;
    WalkPath walk_;
    Camera camera_;
    Vec2 velocity_;
    float bob_ = 0.0f;
    float bobAmount_ = 0.0f;
    float time_ = 0.0f;
    std::vector<float> doorOpen_;
    std::vector<Vec3> cellLight_, cellEmissive_, lightRadiance_;
    float lightingChange_ = 0.0f;
    std::vector<uint8_t> explored_;
    bool flashlightOn_ = true;
    int gunModel_ = -1;
};

}  // namespace bh
