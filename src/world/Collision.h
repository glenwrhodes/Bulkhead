#pragma once
// 2D collision on the floor plane (world X, Z): axis-aligned boxes and a circle for
// the player. Boxes tied to a door only block while that door is (mostly) closed.

#include <vector>

#include "core/Math.h"

namespace bh {

struct Box2 {
    float x0, z0, x1, z1;
    int door = -1;  // >= 0: blocks only while doors[door] is closed
};

class Collision {
public:
    void add(Box2 b) { boxes_.push_back(b); }
    const std::vector<Box2>& boxes() const { return boxes_; }

    // doorOpen[i] in 0..1; a door box stops blocking above 0.85.
    bool overlaps(Vec2 p, float radius, const std::vector<float>& doorOpen) const;
    // Pushes a circle out of every box it overlaps.
    Vec2 resolve(Vec2 p, float radius, const std::vector<float>& doorOpen) const;
    // Moves a circle by delta in small steps, sliding along walls.
    Vec2 move(Vec2 p, Vec2 delta, float radius, const std::vector<float>& doorOpen) const;

private:
    bool active(const Box2& b, const std::vector<float>& doorOpen) const;
    std::vector<Box2> boxes_;
};

}  // namespace bh
