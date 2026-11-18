#include "game/AudioRecorder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <map>

#include "core/Image.h"
#include "game/Game.h"

namespace bh {

namespace {

Color groupColor(const std::string& g) {
    if (g == "player") return rgb(0x46dcff);
    if (g == "gun") return rgb(0xf2d024);
    if (g == "creature") return rgb(0xff6a1a);
    if (g == "world") return rgb(0x6ddc5a);
    if (g == "ambience") return rgb(0x6a7cff);
    return rgb(0xffffff);
}

void put32(std::ofstream& f, uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); }
void put16(std::ofstream& f, uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); }

}  // namespace

AudioRecorder::AudioRecorder(const SoundBank& bank, int fps) : mixer_(bank), fps_(std::max(1, fps)) {}

void AudioRecorder::prime(Game& game) { game.collectSounds(events_, loops_); }

void AudioRecorder::frame(Game& game, int frameIndex) {
    game.collectSounds(events_, loops_);
    const Listener L = game.listener();
    mixer_.setListener(L);
    for (const SoundEvent& e : events_) {
        mixer_.play(e);
        rows_.push_back({frameIndex, e.time, soundFile(e.sound), soundGroup(e.sound), e.pos,
                         e.positional ? length(e.pos - L.pos) : 0.0f, e.gain, e.occlusion, e.positional});
    }
    mixer_.setLoops(loops_);
    // loops starting and stopping
    std::vector<uint32_t> now;
    for (const SoundLoop& l : loops_) {
        now.push_back(l.key);
        if (std::find(loopsOn_.begin(), loopsOn_.end(), l.key) == loopsOn_.end())
            rows_.push_back({frameIndex, game.time(), std::string(soundFile(l.sound)) + " (loop on)", soundGroup(l.sound),
                             l.pos, l.positional ? length(l.pos - L.pos) : 0.0f, l.gain, l.occlusion, l.positional});
    }
    for (uint32_t k : loopsOn_)
        if (std::find(now.begin(), now.end(), k) == now.end())
            rows_.push_back({frameIndex, game.time(), std::format("loop {} off", k), "loop", {}, 0.0f, 0.0f, 0.0f, false});
    loopsOn_ = now;
    // exactly this frame's share of samples (the running total is rounded, never drifts)
    const uint64_t total = (framesDone_ + 1) * uint64_t(Mixer::kRate) / uint64_t(fps_);
    const uint64_t have = framesDone_ * uint64_t(Mixer::kRate) / uint64_t(fps_);
    const int n = int(total - have);
    const size_t at = pcm_.size();
    pcm_.resize(at + size_t(n) * 2);
    mixer_.render(pcm_.data() + at, n);
    ++framesDone_;
}

bool AudioRecorder::finish(const std::filesystem::path& dir, std::vector<std::string>& summary) {
    const size_t frames = pcm_.size() / 2;
    // audio.wav: 16-bit PCM stereo
    {
        std::ofstream f(dir / "audio.wav", std::ios::binary);
        if (!f) return false;
        const uint32_t bytes = uint32_t(frames * 4);
        f.write("RIFF", 4);
        put32(f, 36 + bytes);
        f.write("WAVEfmt ", 8);
        put32(f, 16);
        put16(f, 1);  // PCM
        put16(f, 2);
        put32(f, uint32_t(Mixer::kRate));
        put32(f, uint32_t(Mixer::kRate) * 4);
        put16(f, 4);
        put16(f, 16);
        f.write("data", 4);
        put32(f, bytes);
        std::vector<int16_t> s(pcm_.size());
        for (size_t i = 0; i < pcm_.size(); ++i)
            s[i] = int16_t(std::lround(std::clamp(pcm_[i], -1.0f, 1.0f) * 32767.0f));
        f.write(reinterpret_cast<const char*>(s.data()), std::streamsize(s.size() * 2));
        if (!f) return false;
    }
    // audio_events.csv
    {
        std::ofstream f(dir / "audio_events.csv");
        f << "frame,time_s,sound,group,x,y,z,distance_m,gain,occlusion,positional\n";
        for (const Row& r : rows_)
            f << std::format("{},{:.3f},{},{},{:.2f},{:.2f},{:.2f},{:.1f},{:.2f},{:.0f},{}\n", r.frame + 1, r.time,
                             r.what, r.group, r.pos.x, r.pos.y, r.pos.z, r.distance, r.gain, r.occlusion,
                             r.positional ? 1 : 0);
    }
    // audio_wave.png
    {
        const int W = 1600, H = 440, lane = 48;
        Image img(W, H, rgb(0x0b0e12));
        const double seconds = double(frames) / Mixer::kRate;
        for (int s = 1; s < int(seconds); ++s) {
            const int x = int(double(s) / seconds * W);
            img.fillRect(x, 0, x + 1, H, rgb(0x1c232b));
        }
        const int bandH = (H - lane) / 2;
        for (int side = 0; side < 2; ++side) {
            const int y0 = lane + side * bandH, mid = y0 + bandH / 2;
            img.fillRect(0, mid, W, mid + 1, rgb(0x2a333d));
            img.fillRect(0, y0 + bandH / 2 - int(0.9 * bandH / 2), W, y0 + bandH / 2 - int(0.9 * bandH / 2) + 1,
                         rgb(0x3a2020));  // the limiter's knee (0.9)
            for (int x = 0; x < W; ++x) {
                const size_t a = size_t(double(x) / W * double(frames)), b = std::max(a + 1, size_t(double(x + 1) / W * double(frames)));
                float lo = 0.0f, hi = 0.0f;
                for (size_t i = a; i < std::min(b, frames); ++i) {
                    const float v = pcm_[i * 2 + size_t(side)];
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                const int ya = mid - int(hi * float(bandH / 2)), yb = mid - int(lo * float(bandH / 2));
                img.fillRect(x, ya, x + 1, yb + 1, side == 0 ? rgb(0x9fb4c8) : rgb(0x8aa0b4));
            }
        }
        for (const Row& r : rows_) {
            if (r.group == "loop") continue;
            const double t = double(r.frame) / fps_;
            const int x = int(t / std::max(1e-6, seconds) * W);
            const bool loop = r.what.find("(loop on)") != std::string::npos;
            const int y = r.group == "player" ? 6 : r.group == "gun" ? 14 : r.group == "creature" ? 22 : r.group == "world" ? 30 : 38;
            const Color c = groupColor(r.group);
            if (loop) img.fillRect(x, y - 3, x + 6, y + 4, c);
            else img.fillRect(x, y - 3, x + 2, y + 4, c);
            img.fillRect(x, lane, x + 1, H, Color{c.r, c.g, c.b, 60});
        }
        img.writePng(dir / "audio_wave.png");
    }
    // summary
    std::map<std::string, int> byGroup, bySound;
    for (const Row& r : rows_) {
        if (r.group == "loop") continue;
        ++byGroup[r.group];
        ++bySound[r.what];
    }
    double sum2 = 0.0;
    for (float v : pcm_) sum2 += double(v) * double(v);
    const double rms = pcm_.empty() ? 0.0 : std::sqrt(sum2 / double(pcm_.size()));
    summary.push_back(std::format("Audio: {} samples ({:.2f} s at {} Hz, {} a frame at {} fps), RMS {:.3f}, peak before "
                                  "the limiter {:.2f}, {} samples bent by it",
                                  frames, double(frames) / Mixer::kRate, Mixer::kRate, Mixer::kRate / fps_, fps_, rms,
                                  mixer_.peak(), mixer_.overs()));
    std::string g = "  events by group:";
    for (const auto& [k, v] : byGroup) g += std::format(" {} {},", k, v);
    summary.push_back(g);
    std::string s = "  by sound:";
    for (const auto& [k, v] : bySound) s += std::format(" {} {},", k, v);
    summary.push_back(s);
    return true;
}

}  // namespace bh
