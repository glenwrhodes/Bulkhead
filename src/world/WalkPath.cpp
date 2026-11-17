#include "world/WalkPath.h"

#include <algorithm>

#include "world/Level.h"

namespace bh {

WalkPath::WalkPath(const std::vector<Int2>& route, int smoothing) {
    std::vector<Vec2> pts;
    for (Int2 c : route) {
        Vec3 p = cellCenter(c);
        pts.push_back({p.x, p.z});
    }
    // Drop collinear points so only real corners get rounded.
    std::vector<Vec2> corners;
    for (size_t i = 0; i < pts.size(); ++i) {
        if (i > 0 && i + 1 < pts.size()) {
            Vec2 a = pts[i] - pts[i - 1], b = pts[i + 1] - pts[i];
            if (std::abs(a.x * b.y - a.y * b.x) < 1e-4f) continue;
        }
        corners.push_back(pts[i]);
    }
    // Chaikin corner cutting keeps the curve inside the original polyline's corners.
    for (int it = 0; it < smoothing && corners.size() > 2; ++it) {
        std::vector<Vec2> next{corners.front()};
        for (size_t i = 0; i + 1 < corners.size(); ++i) {
            Vec2 a = corners[i], b = corners[i + 1];
            float len = bh::length(b - a);
            // cut at most 1.1 m from each corner so long runs stay straight
            float t = std::min(0.25f, 1.1f / std::max(len, 1e-3f));
            if (i > 0) next.push_back(a + (b - a) * t);
            if (i + 2 < corners.size()) next.push_back(a + (b - a) * (1.0f - t));
        }
        next.push_back(corners.back());
        corners.swap(next);
    }
    points_ = corners;
    cumulative_.reserve(points_.size());
    float total = 0;
    for (size_t i = 0; i < points_.size(); ++i) {
        if (i) total += bh::length(points_[i] - points_[i - 1]);
        cumulative_.push_back(total);
    }
}

Vec2 WalkPath::at(float s) const {
    if (points_.empty()) return {};
    if (s <= 0 || points_.size() == 1) return points_.front();
    if (s >= length()) return points_.back();
    size_t i = size_t(std::upper_bound(cumulative_.begin(), cumulative_.end(), s) - cumulative_.begin());
    float seg = cumulative_[i] - cumulative_[i - 1];
    float t = seg > 0 ? (s - cumulative_[i - 1]) / seg : 0;
    return points_[i - 1] + (points_[i] - points_[i - 1]) * t;
}

Vec2 WalkPath::direction(float s, float lookAhead) const {
    Vec2 a = at(s), b = at(s + lookAhead);
    Vec2 d = b - a;
    if (bh::length(d) < 1e-3f) {
        a = at(s - lookAhead);
        d = b - a;
    }
    float l = bh::length(d);
    return l > 1e-6f ? d * (1.0f / l) : Vec2{0, -1};
}

}  // namespace bh
