#include "world/Collision.h"

#include <cmath>

namespace bh {

bool Collision::active(const Box2& b, const std::vector<float>& doorOpen, float height) const {
    if (b.bottom > 0.0f && height <= b.bottom) return false;  // passes underneath
    if (b.door < 0) return true;
    return size_t(b.door) >= doorOpen.size() || doorOpen[size_t(b.door)] < 0.85f;
}

bool Collision::overlaps(Vec2 p, float r, const std::vector<float>& doorOpen, float height) const {
    for (const Box2& b : boxes_) {
        if (!active(b, doorOpen, height)) continue;
        float qx = std::clamp(p.x, b.x0, b.x1), qz = std::clamp(p.y, b.z0, b.z1);
        float dx = p.x - qx, dz = p.y - qz;
        if (dx * dx + dz * dz < r * r - 1e-6f) return true;
    }
    return false;
}

float Collision::headroom(Vec2 p, float r) const {
    float low = 0.0f;
    for (const Box2& b : boxes_) {
        if (b.bottom <= 0.0f) continue;
        float qx = std::clamp(p.x, b.x0, b.x1), qz = std::clamp(p.y, b.z0, b.z1);
        float dx = p.x - qx, dz = p.y - qz;
        if (dx * dx + dz * dz < r * r && (low == 0.0f || b.bottom < low)) low = b.bottom;
    }
    return low;
}

void Collision::buildIndex(float cellSize, int gridW, int gridH) {
    cellSize_ = cellSize;
    gridW_ = gridW;
    gridH_ = gridH;
    index_.assign(size_t(gridW) * size_t(gridH), {});
    auto cell = [&](float v, int n) { return std::clamp(int(std::floor(v / cellSize_ + 0.5f)), 0, n - 1); };
    for (size_t i = 0; i < boxes_.size(); ++i) {
        const Box2& b = boxes_[i];
        for (int y = cell(b.z0, gridH); y <= cell(b.z1, gridH); ++y)
            for (int x = cell(b.x0, gridW); x <= cell(b.x1, gridW); ++x)
                index_[size_t(y) * size_t(gridW) + size_t(x)].push_back(int(i));
    }
}

const Box2* Collision::blockedAt(Vec2 p, float y, const std::vector<float>& doorOpen) const {
    if (index_.empty()) return nullptr;
    int cx = int(std::floor(p.x / cellSize_ + 0.5f)), cy = int(std::floor(p.y / cellSize_ + 0.5f));
    if (cx < 0 || cy < 0 || cx >= gridW_ || cy >= gridH_) return nullptr;
    for (int i : index_[size_t(cy) * size_t(gridW_) + size_t(cx)]) {
        const Box2& b = boxes_[size_t(i)];
        if (p.x < b.x0 || p.x > b.x1 || p.y < b.z0 || p.y > b.z1) continue;
        if (b.bottom > 0.0f && (y < b.bottom || y > b.bottom + 0.4f)) continue;
        if (b.door >= 0 && size_t(b.door) < doorOpen.size() && doorOpen[size_t(b.door)] >= 0.85f) continue;
        return &b;
    }
    return nullptr;
}

Vec2 Collision::resolve(Vec2 p, float r, const std::vector<float>& doorOpen, float height) const {
    for (int iter = 0; iter < 4; ++iter) {
        bool moved = false;
        for (const Box2& b : boxes_) {
            if (!active(b, doorOpen, height)) continue;
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

Vec2 Collision::move(Vec2 p, Vec2 delta, float r, const std::vector<float>& doorOpen, float height) const {
    float len = length(delta);
    int steps = std::max(1, int(std::ceil(len / 0.08f)));
    Vec2 step = delta * (1.0f / float(steps));
    for (int i = 0; i < steps; ++i) p = resolve(p + step, r, doorOpen, height);
    return p;
}

}  // namespace bh
