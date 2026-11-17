#include "world/PathTrack.h"

#include <algorithm>
#include <cmath>

namespace bh {

PathTrack::PathTrack(std::vector<Float2> points) : points_(std::move(points)) {
    cumulative_.reserve(points_.size());
    float total = 0.0f;
    for (size_t i = 0; i < points_.size(); ++i) {
        if (i > 0) total += std::hypot(points_[i].x - points_[i - 1].x, points_[i].y - points_[i - 1].y);
        cumulative_.push_back(total);
    }
}

Float2 PathTrack::at(float distance) const {
    if (points_.empty()) return {};
    if (distance <= 0.0f || points_.size() == 1) return points_.front();
    if (distance >= length()) return points_.back();
    // First point whose cumulative distance is beyond `distance`.
    auto it = std::upper_bound(cumulative_.begin(), cumulative_.end(), distance);
    size_t i = size_t(it - cumulative_.begin());
    float segment = cumulative_[i] - cumulative_[i - 1];
    float t = segment > 0.0f ? (distance - cumulative_[i - 1]) / segment : 0.0f;
    const Float2& a = points_[i - 1];
    const Float2& b = points_[i];
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

}  // namespace bh
