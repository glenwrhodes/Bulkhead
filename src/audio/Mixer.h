#pragma once
// Sound playback in software (part 3), the same for the window and the captures, so a
// capture's WAV is what the window would have played. No Windows here: the window feeds
// the mixer's output to XAudio2 (AudioDevice), a capture writes it beside the frames.
//
//   SoundBank  the decoded clips (dr_mp3 / dr_wav), 44.1 kHz float
//   Mixer      voices for one-shots and keyed loops, placed round the listener:
//              distance (quieter far off), left/right (equal power), muffled behind you
//              and behind walls (one-pole low-pass), a small metallic room (Schroeder
//              reverb: 4 combs, 2 all-passes per side), and a soft limiter on the output
//
// render() makes exactly the number of frames asked for, so a capture at 30 fps takes
// 1470 a frame and stays in step with the pictures. Not thread-safe: the window locks.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "audio/Sounds.h"

namespace bh {

struct SoundClip {
    std::vector<float> samples;  // interleaved, at Mixer::kRate
    int channels = 1;
    size_t frames() const { return channels ? samples.size() / size_t(channels) : 0; }
    bool empty() const { return samples.empty(); }
};

class SoundBank {
public:
    // Loads assets/sounds/<file>.mp3 (or .wav) for every Snd. Missing files stay silent.
    // Returns how many loaded; `report` lists each (name, seconds, peak) or what failed.
    int load(const std::filesystem::path& soundsDir, std::string& report);
    const SoundClip& clip(Snd s) const { return clips_[size_t(s)]; }
    // A clip made in code (tests).
    void set(Snd s, SoundClip c) { clips_[size_t(s)] = std::move(c); }

private:
    std::array<SoundClip, kSoundCount> clips_{};
};

class Mixer {
public:
    static constexpr int kRate = 44100;
    static constexpr int kMaxVoices = 64;

    explicit Mixer(const SoundBank& bank);

    void setListener(const Listener& l) { listener_ = l; }
    void play(const SoundEvent& e);
    // The loops sounding now. New keys fade in (0.25 s), missing keys fade out.
    void setLoops(const std::vector<SoundLoop>& loops);
    // Interleaved stereo, exactly `frames` frames.
    void render(float* out, int frames);
    void reset();

    uint64_t framesRendered() const { return rendered_; }
    int voices() const { return int(voices_.size()); }
    float peak() const { return peak_; }        // largest |sample| before the limiter
    uint64_t overs() const { return overs_; }   // samples the limiter had to bend (|x| > 0.9)

private:
    struct Voice {
        Snd sound = Snd::Footstep1;
        double cursor = 0.0;
        float pitch = 1.0f;
        Vec3 pos;
        bool positional = true;
        float gain = 1.0f, occlusion = 0.0f;
        bool loop = false;
        uint32_t key = 0;
        float fade = 1.0f, fadeTarget = 1.0f;
        float gl = 0.0f, gr = 0.0f, send = 0.0f, lpA = 1.0f;  // current (smoothed) settings
        float lpL = 0.0f, lpR = 0.0f;
        bool started = false;
        uint64_t serial = 0;
    };
    void targets(const Voice& v, float& gl, float& gr, float& send, float& lpA) const;

    const SoundBank& bank_;
    Listener listener_;
    std::vector<Voice> voices_;
    uint64_t rendered_ = 0, serial_ = 0, overs_ = 0;
    float peak_ = 0.0f;
    // reverb
    struct Comb {
        std::vector<float> buf;
        size_t i = 0;
        float store = 0.0f;
    };
    struct AllPass {
        std::vector<float> buf;
        size_t i = 0;
    };
    std::array<Comb, 8> combs_;      // 4 per side
    std::array<AllPass, 4> allpass_; // 2 per side
};

}  // namespace bh
