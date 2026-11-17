#pragma once
// Turns a Maze into a level built from the Blender kit: which model goes where, the
// doors, every light in world space (plus a per-cell lookup grid), the collision boxes,
// props, and the start/exit. World space: X east, Y up, Z south; cell (x, y) is centred
// at (4x, 0, 4y). No D3D12 here.

#include <cstdint>
#include <string>
#include <vector>

#include "assets/Kit.h"
#include "core/Math.h"
#include "world/Collision.h"
#include "world/Maze.h"
#include "world/Sections.h"

namespace bh {

inline constexpr float kCell = 4.0f;
inline constexpr float kEyeHeight = 1.7f;

inline Vec3 cellCenter(int x, int y) { return {x * kCell, 0.0f, y * kCell}; }
inline Vec3 cellCenter(Int2 c) { return cellCenter(c.x, c.y); }
// Unit vector of a maze direction in world space (north = -Z).
inline Vec3 dirVector(Dir d) {
    static const Vec3 v[4] = {{0, 0, -1}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}};
    return v[int(d)];
}
// Yaw that rotates the canonical "north" of a kit piece to face direction d.
inline float yawFor(Dir d) {
    static const float a[4] = {0.0f, -kPi / 2, kPi, kPi / 2};  // N, E, S, W
    return a[int(d)];
}

struct PlacedModel {
    int model = -1;
    Mat4 transform;
    int door = -1;  // door leaves: index into Level::doors (animated)
    int cell = -1;  // grid cell (y * width + x) whose power feeds its emissive surfaces
    bool selfPowered = false;  // emergency beacons: pulse on their own, whatever the cell's power
};

// Where effects come from: Blender markers on placed pieces (round 6).
struct Emitter {
    enum Kind : uint8_t { Spark, Steam } kind = Spark;
    Vec3 position;
    Vec3 direction{0, -1, 0};
    int cell = -1;
    float phase = 0.0f;  // 0..1, so emitters do not fire in step
};

// A pipe bundle across a corridor that the player has to crouch under.
struct LowPipe {
    Vec3 center;          // on the floor under the bundle
    Vec3 along;           // the corridor's direction (unit)
    float bottom = 1.3f;  // underside height
};

inline constexpr float kStandHeight = 1.85f;   // body height for collision, standing
inline constexpr float kCrouchHeight = 1.2f;   // and crouching
inline constexpr float kCrouchEye = 1.05f;

struct Door {
    Int2 cell;            // cell on the corridor side (or either side)
    Dir side = Dir::North;
    DoorKind kind = DoorKind::Sliding;
    Vec3 center;          // edge midpoint on the floor
    float open = 0.0f;    // 0 closed .. 1 open
};

struct WorldLight {
    Vec3 position;
    Vec3 radiance;        // colour x intensity (W/sr) of the fixture at full power
    float radius = 0.15f;
    float range = 9.0f;
    int cell = 0;         // grid cell (y * width + x); its CellPower scales the radiance
    bool beacon = false;  // an emergency beacon: pulses red on its own
};

struct Level {
    std::vector<PlacedModel> placed;
    std::vector<Door> doors;
    std::vector<WorldLight> lights;
    // Light grid: for cell (x, y), lights cellLights[cellStart[i] .. +cellCount[i]], i = y * w + x.
    int gridW = 0, gridH = 0;
    std::vector<uint32_t> cellStart, cellCount, cellLights;
    Collision collision;
    Sections sections;        // moods, per-cell light behaviour, wear and haze
    std::vector<Emitter> emitters;
    std::vector<LowPipe> lowPipes;
    int brokenCeilings = 0, steamPipes = 0, debrisPiles = 0, beacons = 0;
    std::vector<Int2> route;  // start -> exit
    Vec3 spawn;
    float spawnYaw = 0.0f;
    Vec3 exitPoint;
    std::vector<std::string> missingModels;  // kit pieces the level wanted but the kit lacks
    int propCount = 0;
};

Level buildLevel(const Maze& maze, const Kit& kit);

// Ceiling height over a world position (corridor 2.85 m, room 3.4 m).
float ceilingAt(const Maze& maze, Vec3 p);

// First thing a ray hits: floor, ceiling, or a collision box (walls, doors, props, low
// pipes), approximated from the level's collision. Returns the distance (maxDist if
// nothing) and the surface normal.
float raycastLevel(const Maze& maze, const Level& level, Vec3 origin, Vec3 dir, float maxDist,
                   const std::vector<float>& doorOpen, Vec3& normal);

// Leaf offset (model space) for a door part at a given opening.
Vec3 doorLeafOffset(const std::string& partName, float open);

}  // namespace bh
