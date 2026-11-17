#pragma once
// 2D collision on the floor plane (world X, Z): axis-aligned boxes and a circle for
// the player. Boxes tied to a door only block while that door is (mostly) closed.

#include <vector>

#include "core/Math.h"

namespace bh {

struct Box2 {
    float x0, z0, x1, z1;
    int door = -1;        // >= 0: blocks only while doors[door] is closed
    float bottom = 0.0f;  // > 0: an overhead obstacle (low pipes); blocks only bodies taller than this
};

class Collision {
public:
    void add(Box2 b) { boxes_.push_back(b); }
    const std::vector<Box2>& boxes() const { return boxes_; }

    // doorOpen[i] in 0..1; a door box stops blocking above 0.85. height is how tall the
    // body is (standing ~1.85 m, crouching ~1.2 m): overhead boxes above it do not block.
    bool overlaps(Vec2 p, float radius, const std::vector<float>& doorOpen, float height = 1.85f) const;
    // Pushes a circle out of every box it overlaps.
    Vec2 resolve(Vec2 p, float radius, const std::vector<float>& doorOpen, float height = 1.85f) const;
    // Moves a circle by delta in small steps, sliding along walls.
    Vec2 move(Vec2 p, Vec2 delta, float radius, const std::vector<float>& doorOpen, float height = 1.85f) const;
    // Lowest overhead obstacle over a circle (0 = none): how low the body has to be there.
    float headroom(Vec2 p, float radius) const;

    // Point queries (for rays): index the boxes by grid cell (cells centred on multiples
    // of cellSize), then ask whether a point at height y is inside a box. Overhead boxes
    // span bottom .. bottom + 0.4 m; the rest are floor to ceiling.
    void buildIndex(float cellSize, int gridW, int gridH);
    const Box2* blockedAt(Vec2 p, float y, const std::vector<float>& doorOpen) const;

private:
    bool active(const Box2& b, const std::vector<float>& doorOpen, float height) const;
    std::vector<Box2> boxes_;
    float cellSize_ = 4.0f;
    int gridW_ = 0, gridH_ = 0;
    std::vector<std::vector<int>> index_;
};

}  // namespace bh
