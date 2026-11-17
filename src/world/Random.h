#pragma once
// PCG32 random generator. Used instead of <random> distributions because those are
// implementation-defined: a seed must give the same ship on every machine and build.

#include <cstdint>
#include <utility>
#include <vector>

namespace bh {

class Rng {
public:
    explicit Rng(uint64_t seed, uint64_t stream = 0xda3e39cb94b95bdbULL) {
        inc_ = (stream << 1u) | 1u;
        next();
        state_ += seed;
        next();
    }

    uint32_t next() {
        uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        uint32_t xorshifted = uint32_t(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = uint32_t(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
    }

    // Uniform in [0, bound). Unbiased.
    uint32_t below(uint32_t bound) {
        if (bound <= 1) return 0;
        uint32_t threshold = (0u - bound) % bound;
        for (;;) {
            uint32_t r = next();
            if (r >= threshold) return r % bound;
        }
    }

    // Uniform in [lo, hi], inclusive.
    int range(int lo, int hi) {
        if (hi <= lo) return lo;
        return lo + int(below(uint32_t(hi - lo + 1)));
    }

    // Uniform in [0, 1).
    float unit() { return float(next() >> 8) * (1.0f / 16777216.0f); }

    bool chance(float probability) { return unit() < probability; }

    template <class T>
    void shuffle(std::vector<T>& v) {
        for (size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[below(uint32_t(i))]);
    }

private:
    uint64_t state_ = 0;
    uint64_t inc_ = 0;
};

}  // namespace bh
