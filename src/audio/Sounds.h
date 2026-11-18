#pragma once
// The game's sounds (part 3): one id per file in assets/sounds (sources and licences in
// assets/sounds/CREDITS.md), and what the game hands the mixer. No Windows, no D3D12.
//
// The game produces two things each frame:
//   SoundEvent  a one-shot (a footstep, the gun, a claw landing, a hiss): it starts when the
//               mixer gets it and plays to the end
//   SoundLoop   something that keeps sounding while it is there (the ship's hum, a steam
//               jet, a creature skittering): the game lists the loops playing now, keyed;
//               the mixer fades a key in when it appears and out when it goes

#include <cstdint>

#include "core/Math.h"

namespace bh {

enum class Snd : uint8_t {
    Footstep1, Footstep2, Footstep3,
    GunFire, HitMetal, HitCreature,
    Claw1, Claw2,
    CreatureHiss, CreatureScreech, CreaturePain, CreatureDeath, CreatureSkitter, Feeding,
    PlayerThud,
    DoorSliding, DoorHeavy,
    Sparks, Steam, ShipHum, HullGroan, DistantKnock,
    ScannerPing,
    Count
};
constexpr int kSoundCount = int(Snd::Count);

const char* soundFile(Snd s);  // file name in assets/sounds without the extension
// What the sound is for (the capture's audio_events.csv groups by it):
// "player", "gun", "creature", "world", "ambience", "ui"
const char* soundGroup(Snd s);

struct SoundEvent {
    Snd sound = Snd::Footstep1;
    float time = 0.0f;       // game seconds (for the log; it starts when the mixer gets it)
    Vec3 pos;
    bool positional = true;  // false: plays as is, centred (the ship's hum, the gun in your hand)
    float gain = 1.0f;
    float pitch = 1.0f;      // playback rate
    float occlusion = 0.0f;  // 0 in plain view .. 1 behind walls (muffled and quieter)
};

struct SoundLoop {
    Snd sound = Snd::ShipHum;
    uint32_t key = 0;        // the same thing keeps the same key from frame to frame
    Vec3 pos;
    bool positional = true;
    float gain = 1.0f;
    float pitch = 1.0f;
    float occlusion = 0.0f;
};

struct Listener {
    Vec3 pos;
    Vec3 right{1, 0, 0};
    Vec3 forward{0, 0, -1};
};

}  // namespace bh
