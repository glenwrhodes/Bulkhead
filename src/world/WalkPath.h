#pragma once
// The scripted walk used by capture mode: the start->exit route through cell centres,
// with corners rounded off (Chaikin), sampled by distance walked.

#include <vector>

#include "core/Math.h"
#include "world/Maze.h"

namespace bh {

class WalkPath {
public:
    WalkPath() = default;
    explicit WalkPath(const std::vector<Int2>& route, int smoothing = 3);

    float length() const { return cumulative_.empty() ? 0.0f : cumulative_.back(); }
    const std::vector<Vec2>& points() const { return points_; }
    Vec2 at(float s) const;                 // world X, Z
    Vec2 direction(float s, float lookAhead) const;

private:
    std::vector<Vec2> points_;
    std::vector<float> cumulative_;
};

}  // namespace bh
