#include "world/CrawlSurface.h"

#include <algorithm>
#include <cmath>

#include "world/Level.h"

namespace bh {

namespace {
// bk_corridor.py OUTLINE, walked from the middle of the floor toward +x (counter-clockwise
// seen with x right and z up, so the corridor's inside is on the left of travel).
const Vec2 kOutline[] = {{0.0f, 0.0f},   {0.9f, 0.0f},   {1.2f, 0.45f},  {1.2f, 2.25f}, {0.75f, 2.85f},
                         {-0.75f, 2.85f}, {-1.2f, 2.25f}, {-1.2f, 0.45f}, {-0.9f, 0.0f}};
constexpr float kCornerBlend = 0.22f;  // metres either side of a corner over which the normal turns
}  // namespace

CrawlProfile::CrawlProfile() {
    for (const Vec2& p : kOutline) pts_.push_back(p);
    pts_.push_back(kOutline[0]);
    cum_.push_back(0.0f);
    for (size_t i = 1; i < pts_.size(); ++i) cum_.push_back(cum_.back() + bh::length(pts_[i] - pts_[i - 1]));
    total_ = cum_.back();
    // the middle of the right wall: halfway between (1.2, 0.45) and (1.2, 2.25)
    rightWall_ = 0.5f * (cum_[2] + cum_[3]);
}

static float wrapU(float u, float total) {
    u = std::fmod(u, total);
    return u < 0.0f ? u + total : u;
}

Vec2 CrawlProfile::point(float u) const {
    u = wrapU(u, total_);
    size_t i = size_t(std::upper_bound(cum_.begin(), cum_.end(), u) - cum_.begin());
    i = std::clamp<size_t>(i, 1, pts_.size() - 1);
    float t = (u - cum_[i - 1]) / std::max(1e-6f, cum_[i] - cum_[i - 1]);
    return pts_[i - 1] + (pts_[i] - pts_[i - 1]) * t;
}

Vec2 CrawlProfile::normal(float u) const {
    auto segNormal = [&](size_t i) {  // inward normal of segment i -> i+1: tangent turned left
        Vec2 d = pts_[i + 1] - pts_[i];
        float l = bh::length(d);
        return Vec2{-d.y / l, d.x / l};
    };
    u = wrapU(u, total_);
    const size_t nseg = pts_.size() - 1;
    size_t i = size_t(std::upper_bound(cum_.begin(), cum_.end(), u) - cum_.begin());
    i = std::clamp<size_t>(i, 1, nseg) - 1;  // segment index
    Vec2 n = segNormal(i);
    // blend toward the neighbouring segment near each end
    const float toStart = u - cum_[i], toEnd = cum_[i + 1] - u;
    if (toEnd < kCornerBlend) {
        float w = 0.5f * (1.0f - toEnd / kCornerBlend);
        n = n * (1.0f - w) + segNormal((i + 1) % nseg) * w;
    } else if (toStart < kCornerBlend) {
        float w = 0.5f * (1.0f - toStart / kCornerBlend);
        n = n * (1.0f - w) + segNormal((i + nseg - 1) % nseg) * w;
    }
    float l = bh::length(n);
    return l > 1e-6f ? n * (1.0f / l) : Vec2{0, 1};
}

CrawlPath::CrawlPath(const std::vector<Int2>& cells) : path_(cells, 2) {}

Vec3 CrawlPath::tangent(float s) const {
    Vec2 d = path_.direction(std::clamp(s, 0.0f, length()), 0.35f);
    return {d.x, 0.0f, d.y};
}

Vec3 CrawlPath::right(float s) const {
    Vec3 t = tangent(s);
    return {-t.z, 0.0f, t.x};
}

Vec3 CrawlPath::point(float s, float u) const {
    const float sc = std::clamp(s, 0.0f, length());
    Vec2 c = path_.at(sc);
    Vec3 t = tangent(sc);
    // beyond the ends, carry on straight
    Vec3 base{c.x + t.x * (s - sc), 0.0f, c.y + t.z * (s - sc)};
    Vec2 q = profile_.point(u);
    Vec3 r = right(sc);
    return base + r * q.x + Vec3{0, q.y, 0};
}

Vec3 CrawlPath::normal(float s, float u) const {
    Vec2 n = profile_.normal(u);
    Vec3 r = right(std::clamp(s, 0.0f, length()));
    return normalize(r * n.x + Vec3{0, n.y, 0});
}

float CrawlPath::project(Vec3 p, float from, float to) const {
    from = std::max(0.0f, from);
    to = std::min(length(), to);
    float best = from, bestD = 1e30f;
    for (float s = from; s <= to + 1e-4f; s += 0.05f) {
        Vec2 c = path_.at(s);
        float d = (c.x - p.x) * (c.x - p.x) + (c.y - p.z) * (c.y - p.z);
        if (d < bestD) {
            bestD = d;
            best = s;
        }
    }
    return best;
}

float CrawlPath::project(Vec3 p) const {
    float best = 0.0f, bestD = 1e30f;
    for (float s = 0.0f; s <= length(); s += 0.1f) {
        Vec2 c = path_.at(s);
        float d = (c.x - p.x) * (c.x - p.x) + (c.y - p.z) * (c.y - p.z);
        if (d < bestD) {
            bestD = d;
            best = s;
        }
    }
    return best;
}

}  // namespace bh
