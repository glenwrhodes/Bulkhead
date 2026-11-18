#pragma once
// The motion scanner (round 17): it tells you something is coming, and roughly from where,
// never exactly where.
//
// Every so often a pulse goes out from the player, its front sweeping to kRange in
// kSweepTime. Anything moving that the front passes (through walls: it senses motion, not
// sight) leaves a blip: a soft blob near it, offset by up to `fuzz` and wider the farther
// away it is, which fades until the next pulse. Things standing still leave nothing. Two
// creatures close together blur into one blob.
//
// The pulse comes round faster as the nearest contact closes in (kPeriodIdle with nothing
// on it, down to kPeriodClose), and its ping rises in pitch. All of it is a pure function
// of the updates it is given, so captures are repeatable. No D3D12 here.

#include <cstdint>
#include <vector>

#include "core/Math.h"
#include "game/Effects.h"

namespace bh {

class Scanner {
public:
    static constexpr float kRange = 25.0f;        // metres (the encounter timer's "within 25 m")
    static constexpr float kSweepTime = 1.0f;     // the front takes this to reach kRange
    static constexpr float kPeriodIdle = 2.4f;    // seconds between pulses with nothing on it
    static constexpr float kPeriodClose = 0.9f;   // ... with something at kCloseRange
    static constexpr float kCloseRange = 5.0f;
    static constexpr float kMinSpeed = 0.2f;      // m/s: slower than this does not show
    static constexpr float kFade = 1.6f;          // a blip fades over this (per e)

    struct Contact {
        Vec3 position;
        float speed = 0.0f;  // m/s
    };
    // A pulse went out: for the ping. nearest is the closest blip of the pulse before
    // (< 0: it found nothing).
    struct Pulse {
        float time = 0.0f;
        float nearest = -1.0f;
        int contacts = 0;
    };

    void reset();
    // Advance to time t (seconds, increasing) with the player's position and what is moving.
    void update(float t, Vec3 player, const std::vector<Contact>& contacts);

    // For the renderer: blips (faded by age) and the pulse's front.
    const std::vector<ScanBlip>& blips() const { return drawn_; }
    ScanState state() const;
    // Pulses since the last call (the game turns them into pings).
    std::vector<Pulse> takePulses();

    // The ping for a pulse: pitch and gain.
    static float pingPitch(const Pulse& p);
    static float pingGain(const Pulse& p);

    int pulses() const { return pulseCount_; }
    float nearestNow() const { return nearestLast_; }  // closest blip of the last finished pulse

private:
    struct Live {
        ScanBlip blip;
        float born = 0.0f, strength = 1.0f;
    };
    float period() const;
    void startPulse(float t);

    bool started_ = false;
    float time_ = 0.0f;
    float pulseAt_ = -100.0f;      // when the current pulse went out
    float nextPulse_ = 0.0f;
    int pulseCount_ = 0;
    float nearestPulse_ = -1.0f;   // closest contact the current pulse has found so far
    float nearestLast_ = -1.0f;    // ... the pulse before
    int contactsLast_ = 0, contactsPulse_ = 0;
    float lastFront_ = 0.0f;
    std::vector<uint8_t> found_;   // per contact index: the current pulse has passed it
    Vec3 player_;
    std::vector<Live> live_;
    std::vector<ScanBlip> drawn_;
    std::vector<Pulse> pulses_;
};

}  // namespace bh
