#pragma once
// A capture's sound (part 3): after each frame is stepped, collect the game's sounds, mix
// exactly one frame's worth of samples (44100 / fps: 1470 at 30 fps) and keep them. At the
// end, beside the frames:
//   audio.wav          44.1 kHz 16-bit stereo, as long as the frames to the sample
//   audio_events.csv   every one-shot (frame, time, sound, group, position, distance, gain,
//                      how hidden), plus where each loop started and stopped
//   audio_wave.png     the waveform (left above right) under a lane of event markers
//                      coloured by group, with a faint line every second
// No Windows, no D3D12.

#include <filesystem>
#include <string>
#include <vector>

#include "audio/Mixer.h"

namespace bh {

class Game;

class AudioRecorder {
public:
    AudioRecorder(const SoundBank& bank, int fps);
    // Takes stock of the game before the first frame (its first collectSounds), so a late
    // start doesn't open with everything that "happened" during the run-up.
    void prime(Game& game);
    // One frame: collect, mix, keep.
    void frame(Game& game, int frameIndex);
    // Writes the three files; returns false if the WAV could not be written. `summary` gets
    // a few lines for the log (events per group, level, loops).
    bool finish(const std::filesystem::path& dir, std::vector<std::string>& summary);

    size_t samples() const { return pcm_.size() / 2; }

private:
    struct Row {
        int frame;
        float time;
        std::string what, group;
        Vec3 pos;
        float distance, gain, occlusion;
        bool positional;
    };
    Mixer mixer_;
    int fps_;
    uint64_t framesDone_ = 0;
    std::vector<float> pcm_;  // interleaved stereo
    std::vector<Row> rows_;
    std::vector<uint32_t> loopsOn_;
    std::vector<SoundEvent> events_;
    std::vector<SoundLoop> loops_;
};

}  // namespace bh
