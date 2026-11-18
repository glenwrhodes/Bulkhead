#include "game/Scanner.h"

#include <algorithm>
#include <cmath>

namespace bh {

namespace {
uint32_t hash32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
float unit01(uint32_t h) { return float(h >> 8) * (1.0f / 16777216.0f); }
// 0 at kRange or beyond .. 1 at kCloseRange or nearer
float closeness(float d) {
    return std::clamp(1.0f - (d - Scanner::kCloseRange) / (Scanner::kRange - Scanner::kCloseRange), 0.0f, 1.0f);
}
}  // namespace

void Scanner::reset() { *this = Scanner{}; }

float Scanner::period() const {
    if (nearestLast_ < 0.0f) return kPeriodIdle;
    return 2.0f + (kPeriodClose - 2.0f) * closeness(nearestLast_);
}

void Scanner::startPulse(float t) {
    // the pulse that is ending hands on what it found
    nearestLast_ = nearestPulse_;
    contactsLast_ = contactsPulse_;
    nearestPulse_ = -1.0f;
    contactsPulse_ = 0;
    pulseAt_ = t;
    lastFront_ = 0.0f;
    std::fill(found_.begin(), found_.end(), uint8_t(0));
    ++pulseCount_;
    pulses_.push_back({t, nearestLast_, contactsLast_});
    if (pulses_.size() > 8) pulses_.erase(pulses_.begin());  // (nobody is collecting them)
    nextPulse_ = t + period();
}

void Scanner::update(float t, Vec3 player, const std::vector<Contact>& contacts) {
    if (!started_) {
        started_ = true;
        nextPulse_ = t + 0.4f;
    }
    time_ = t;
    player_ = player;
    while (t >= nextPulse_) startPulse(nextPulse_);
    if (found_.size() < contacts.size()) found_.resize(contacts.size(), 0);

    // the front: whatever it has passed since the last update leaves a blip
    const float front = (t - pulseAt_) / kSweepTime * kRange;
    if (pulseCount_ > 0 && front >= 0.0f && lastFront_ <= kRange) {  // (no pulse has gone out yet: nothing)
        for (size_t i = 0; i < contacts.size(); ++i) {
            if (found_[i]) continue;
            const Contact& c = contacts[i];
            const float dx = c.position.x - player.x, dz = c.position.z - player.z;
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d > kRange || d > front) continue;
            found_[i] = 1;
            if (c.speed < kMinSpeed) continue;  // standing still: nothing
            const uint32_t h = hash32(uint32_t(pulseCount_) * 7919u + uint32_t(i) * 104729u + 17u);
            const float fuzz = 0.8f + 0.07f * d;  // metres: never exactly where it is
            const float a = 6.2831853f * unit01(h), m = fuzz * std::sqrt(unit01(hash32(h)));
            Live l;
            l.blip.x = c.position.x + std::cos(a) * m;
            l.blip.z = c.position.z + std::sin(a) * m;
            l.blip.radius = 2.0f + 0.08f * d;  // (a blob two cells across at the edge of its range)
            l.strength = std::clamp(0.45f + 0.4f * c.speed, 0.45f, 1.0f);
            l.born = pulseAt_ + d / kRange * kSweepTime;
            live_.push_back(l);
            ++contactsPulse_;
            nearestPulse_ = nearestPulse_ < 0.0f ? d : std::min(nearestPulse_, d);
        }
    }
    lastFront_ = front;

    // fade
    drawn_.clear();
    for (Live& l : live_) {
        const float age = std::max(0.0f, t - l.born);
        // a flash as it is found, then fading until the pulses after have moved on
        l.blip.alpha = std::min(1.0f, l.strength * (std::exp(-age / kFade) + 0.5f * std::exp(-age * 6.0f)));
        if (l.blip.alpha >= 0.03f) drawn_.push_back(l.blip);
    }
    live_.erase(std::remove_if(live_.begin(), live_.end(), [&](const Live& l) { return l.blip.alpha < 0.03f; }),
                live_.end());
    if (drawn_.size() > 32) drawn_.erase(drawn_.begin(), drawn_.end() - 32);  // (the renderer's limit)
}

ScanState Scanner::state() const {
    ScanState s;
    s.blips = &drawn_;
    s.range = kRange;
    const float front = (time_ - pulseAt_) / kSweepTime * kRange;
    if (pulseCount_ > 0 && front >= 0.0f && front <= kRange) {
        s.front = front;
        s.frontAlpha = 1.0f - 0.7f * (front / kRange) * (front / kRange);
    }
    return s;
}

std::vector<Scanner::Pulse> Scanner::takePulses() {
    std::vector<Pulse> out;
    out.swap(pulses_);
    return out;
}

float Scanner::pingPitch(const Pulse& p) { return p.nearest < 0.0f ? 1.0f : 1.0f + 0.45f * closeness(p.nearest); }
float Scanner::pingGain(const Pulse& p) { return p.nearest < 0.0f ? 0.3f : 0.45f + 0.3f * closeness(p.nearest); }

}  // namespace bh
