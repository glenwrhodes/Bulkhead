#pragma once
// A polyline you can sample by distance travelled. Used for the scripted route:
// today it moves a marker across the overhead map; later it drives the capture camera.

#include <vector>

namespace bh {

struct Float2 {
    float x = 0.0f, y = 0.0f;
};

class PathTrack {
public:
    PathTrack() = default;
    explicit PathTrack(std::vector<Float2> points);

    float length() const { return cumulative_.empty() ? 0.0f : cumulative_.back(); }
    bool empty() const { return points_.empty(); }
    const std::vector<Float2>& points() const { return points_; }

    // Position after travelling `distance` from the first point (clamped to the ends).
    Float2 at(float distance) const;

private:
    std::vector<Float2> points_;
    std::vector<float> cumulative_;  // distance from the start to each point
};

}  // namespace bh
