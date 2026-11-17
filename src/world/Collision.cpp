#include "world/Collision.h"

#include <cmath>

namespace bh {

bool Collision::active(const Box2& b, const std::vector<float>& doorOpen) const {
    if (b.door < 0) return true;
    return size_t(b.door) >= doorOpen.size() || doorOpen[size_t(b.door)] < 0.85f;
}

bool Collision::overlaps(Vec2 p, float r, const std::vector<float>& doorOpen) const {
    for (const Box2& b : boxes_) {
        if (!active(b, doorOpen)) continue;
        float qx = std::clamp(p.x, b.x0, b.x1), qz = std::clamp(p.y, b.z0, b.z1);
        float dx = p.x - qx, dz = p.y - qz;
        if (dx * dx + dz * dz < r * r - 1e-6f) return true;
    }
    return false;
}

Vec2 Collision::resolve(Vec2 p, float r, const std::vector<float>& doorOpen) const {
    for (int iter = 0; iter < 4; ++iter) {
        bool moved = false;
        for (const Box2& b : boxes_) {
            if (!active(b, doorOpen)) continue;
            float qx = std::clamp(p.x, b.x0, b.x1), qz = std::clamp(p.y, b.z0, b.z1);
            float dx = p.x - qx, dz = p.y - qz;
            float d2 = dx * dx + dz * dz;
            if (d2 >= r * r) continue;
            if (d2 > 1e-10f) {
                float d = std::sqrt(d2);
                p.x += dx / d * (r - d);
                p.y += dz / d * (r - d);
            } else {
                // centre inside the box: leave by the nearest side
                float l = p.x - b.x0, rr = b.x1 - p.x, t = p.y - b.z0, bo = b.z1 - p.y;
                float m = std::min(std::min(l, rr), std::min(t, bo));
                if (m == l) p.x = b.x0 - r;
                else if (m == rr) p.x = b.x1 + r;
                else if (m == t) p.y = b.z0 - r;
                else p.y = b.z1 + r;
            }
            moved = true;
        }
        if (!moved) break;
    }
    return p;
}

Vec2 Collision::move(Vec2 p, Vec2 delta, float r, const std::vector<float>& doorOpen) const {
    float len = length(delta);
    int steps = std::max(1, int(std::ceil(len / 0.08f)));
    Vec2 step = delta * (1.0f / float(steps));
    for (int i = 0; i < steps; ++i) p = resolve(p + step, r, doorOpen);
    return p;
}

}  // namespace bh
