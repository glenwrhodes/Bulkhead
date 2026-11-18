#pragma once
// Where the creature can crawl: the corridor's cross-section (floor, lower chamfers,
// walls, upper chamfers, ceiling) as a closed loop, swept along a smoothed path through
// maze cells. A point on it is (s, u): s metres along the path, u metres around the
// loop. u = 0 is the middle of the floor; u grows toward the path's right, up the right
// wall, across the ceiling (u = ceilingU()), down the left wall and back to the floor.
// The loop is the corridor kit's exact outline (bk_corridor.py OUTLINE). No D3D12 here.

#include <vector>

#include "core/Math.h"
#include "world/Maze.h"
#include "world/WalkPath.h"

namespace bh {

class CrawlProfile {
public:
    CrawlProfile();
    float length() const { return total_; }
    // Point (x across, to the right; z up) and the inward normal at u (wraps), smoothed
    // round the corners so a body turning from floor to wall turns smoothly.
    Vec2 point(float u) const;
    Vec2 normal(float u) const;
    static float floorU() { return 0.0f; }
    float rightWallU() const { return rightWall_; }
    float ceilingU() const { return total_ * 0.5f; }
    float leftWallU() const { return total_ - rightWall_; }

private:
    std::vector<Vec2> pts_;  // closed loop, first point repeated at the end
    std::vector<float> cum_;
    float total_ = 0.0f;
    float rightWall_ = 0.0f;
};

class CrawlPath {
public:
    CrawlPath() = default;
    explicit CrawlPath(const std::vector<Int2>& cells);

    float length() const { return path_.length(); }
    const CrawlProfile& profile() const { return profile_; }
    const WalkPath& walk() const { return path_; }

    Vec3 tangent(float s) const;  // horizontal, along the path
    Vec3 right(float s) const;    // horizontal, to the right of the path
    Vec3 point(float s, float u) const;
    Vec3 normal(float s, float u) const;  // away from the surface, into the corridor
    // Closest s on the path to a world point (horizontal distance).
    float project(Vec3 p) const;

private:
    WalkPath path_;
    CrawlProfile profile_;
};

}  // namespace bh
