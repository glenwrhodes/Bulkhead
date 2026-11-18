#include "audio/Mixer.h"

#include <algorithm>
#include <cmath>
#include <format>

#include <dr_mp3.h>
#include <dr_wav.h>

namespace bh {

namespace {

struct SoundInfo {
    const char* file;
    const char* group;
};
constexpr SoundInfo kSounds[kSoundCount] = {
    {"footstep_1", "player"},       {"footstep_2", "player"},       {"footstep_3", "player"},
    {"gun_fire", "gun"},            {"hit_metal", "gun"},           {"hit_creature", "gun"},
    {"claw_1", "creature"},         {"claw_2", "creature"},
    {"creature_hiss", "creature"},  {"creature_screech", "creature"}, {"creature_pain", "creature"},
    {"creature_death", "creature"}, {"creature_skitter_loop", "creature"}, {"creature_feeding_loop", "creature"},
    {"player_thud", "player"},
    {"door_sliding", "world"},      {"door_heavy", "world"},
    {"sparks", "world"},            {"steam_loop", "world"},        {"ship_hum_loop", "ambience"},
    {"hull_groan", "ambience"},     {"distant_knock", "ambience"},
    {"scanner_ping", "ui"},
};

// Linear resampling to the mixer's rate (the ElevenLabs files are 44.1 kHz already).
std::vector<float> resample(const float* in, size_t frames, int channels, unsigned rate) {
    if (rate == unsigned(Mixer::kRate)) return std::vector<float>(in, in + frames * size_t(channels));
    const double step = double(rate) / Mixer::kRate;
    const size_t outFrames = size_t(double(frames) / step);
    std::vector<float> out(outFrames * size_t(channels));
    for (size_t i = 0; i < outFrames; ++i) {
        const double p = double(i) * step;
        const size_t a = std::min(frames - 1, size_t(p));
        const size_t b = std::min(frames - 1, a + 1);
        const float f = float(p - double(a));
        for (int c = 0; c < channels; ++c)
            out[i * size_t(channels) + size_t(c)] =
                in[a * size_t(channels) + size_t(c)] * (1.0f - f) + in[b * size_t(channels) + size_t(c)] * f;
    }
    return out;
}

constexpr int kCombTune[4] = {1116, 1188, 1277, 1356};  // Freeverb's, in samples at 44.1 kHz
constexpr int kAllPassTune[2] = {556, 441};
constexpr int kStereoSpread = 23;

}  // namespace

const char* soundFile(Snd s) { return kSounds[size_t(s)].file; }
const char* soundGroup(Snd s) { return kSounds[size_t(s)].group; }

int SoundBank::load(const std::filesystem::path& dir, std::string& report) {
    int loaded = 0;
    for (int i = 0; i < kSoundCount; ++i) {
        SoundClip c;
        const std::filesystem::path mp3 = dir / (std::string(kSounds[i].file) + ".mp3");
        const std::filesystem::path wav = dir / (std::string(kSounds[i].file) + ".wav");
        if (std::filesystem::exists(mp3)) {
            drmp3_config cfg{};
            drmp3_uint64 frames = 0;
            float* p = drmp3_open_file_and_read_pcm_frames_f32(mp3.string().c_str(), &cfg, &frames, nullptr);
            if (p && frames > 0 && cfg.channels > 0) {
                c.channels = int(std::min<drmp3_uint32>(cfg.channels, 2));
                if (cfg.channels > 2) {  // keep the first two
                    std::vector<float> two(size_t(frames) * 2);
                    for (size_t f = 0; f < size_t(frames); ++f)
                        for (int k = 0; k < 2; ++k) two[f * 2 + size_t(k)] = p[f * cfg.channels + size_t(k)];
                    c.samples = resample(two.data(), size_t(frames), 2, cfg.sampleRate);
                } else {
                    c.samples = resample(p, size_t(frames), c.channels, cfg.sampleRate);
                }
            }
            if (p) drmp3_free(p, nullptr);
        } else if (std::filesystem::exists(wav)) {
            unsigned channels = 0, rate = 0;
            drwav_uint64 frames = 0;
            float* p = drwav_open_file_and_read_pcm_frames_f32(wav.string().c_str(), &channels, &rate, &frames, nullptr);
            if (p && frames > 0 && channels > 0 && channels <= 2) {
                c.channels = int(channels);
                c.samples = resample(p, size_t(frames), c.channels, rate);
            }
            if (p) drwav_free(p, nullptr);
        }
        if (c.empty()) {
            report += std::format("  {}: missing or unreadable\n", kSounds[i].file);
        } else {
            float pk = 0.0f;
            for (float s : c.samples) pk = std::max(pk, std::abs(s));
            report += std::format("  {}: {:.2f} s, {} ch, peak {:.2f}\n", kSounds[i].file,
                                  double(c.frames()) / Mixer::kRate, c.channels, pk);
            ++loaded;
        }
        clips_[size_t(i)] = std::move(c);
    }
    return loaded;
}

Mixer::Mixer(const SoundBank& bank) : bank_(bank) {
    for (int side = 0; side < 2; ++side) {
        for (int k = 0; k < 4; ++k) combs_[size_t(side * 4 + k)].buf.assign(size_t(kCombTune[k] + side * kStereoSpread), 0.0f);
        for (int k = 0; k < 2; ++k)
            allpass_[size_t(side * 2 + k)].buf.assign(size_t(kAllPassTune[k] + side * kStereoSpread), 0.0f);
    }
}

void Mixer::reset() {
    voices_.clear();
    rendered_ = 0;
    peak_ = 0.0f;
    overs_ = 0;
    for (Comb& c : combs_) std::fill(c.buf.begin(), c.buf.end(), 0.0f), c.store = 0.0f;
    for (AllPass& a : allpass_) std::fill(a.buf.begin(), a.buf.end(), 0.0f);
}

void Mixer::targets(const Voice& v, float& gl, float& gr, float& send, float& lpA) const {
    const float g = v.gain * (1.0f - 0.45f * v.occlusion);
    if (!v.positional) {
        gl = gr = g;
        send = 0.04f * g;
        lpA = 1.0f;
        return;
    }
    const Vec3 d = v.pos - listener_.pos;
    const float dist = length(d);
    // quieter with distance: full within a metre, half at 4 m, a quarter at 10 m, a seventh at
    // 20 m (gentler than real life: a creature hissing down the corridor must be heard); gone past 50 m
    float att = 1.0f / (1.0f + std::max(0.0f, dist - 1.0f) / 3.0f);
    if (dist > 45.0f) att *= std::max(0.0f, 1.0f - (dist - 45.0f) / 5.0f);
    const Vec3 dir = dist > 1e-3f ? d * (1.0f / dist) : listener_.forward;
    const float pan = std::clamp(dot(dir, listener_.right), -1.0f, 1.0f) * 0.8f;
    const float a = (pan + 1.0f) * kPi * 0.25f;
    gl = std::cos(a) * att * g * 1.2f;
    gr = std::sin(a) * att * g * 1.2f;
    // muffled a little behind you, a lot behind walls
    const float behind = std::clamp(-dot(dir, listener_.forward), 0.0f, 1.0f);
    float fc = 18000.0f + (6500.0f - 18000.0f) * behind * 0.6f;
    fc = fc + (900.0f - fc) * std::clamp(v.occlusion, 0.0f, 1.0f);
    lpA = 1.0f - std::exp(-2.0f * kPi * fc / float(kRate));
    // more room than direct sound the farther it is
    send = (0.12f + 0.3f * std::clamp(dist / 25.0f, 0.0f, 1.0f)) * att * g;
}

void Mixer::play(const SoundEvent& e) {
    const SoundClip& c = bank_.clip(e.sound);
    if (c.empty() || e.gain <= 0.0f) return;
    if (int(voices_.size()) >= kMaxVoices) {  // drop the oldest one-shot
        auto oldest = voices_.end();
        for (auto it = voices_.begin(); it != voices_.end(); ++it)
            if (!it->loop && (oldest == voices_.end() || it->serial < oldest->serial)) oldest = it;
        if (oldest == voices_.end()) return;
        voices_.erase(oldest);
    }
    Voice v;
    v.sound = e.sound;
    v.pitch = std::clamp(e.pitch, 0.25f, 4.0f);
    v.pos = e.pos;
    v.positional = e.positional;
    v.gain = e.gain;
    v.occlusion = e.occlusion;
    v.serial = serial_++;
    targets(v, v.gl, v.gr, v.send, v.lpA);  // start at the right level (no ramp from silence)
    v.started = true;
    voices_.push_back(v);
}

void Mixer::setLoops(const std::vector<SoundLoop>& loops) {
    for (Voice& v : voices_)
        if (v.loop) v.fadeTarget = 0.0f;
    for (const SoundLoop& l : loops) {
        const SoundClip& c = bank_.clip(l.sound);
        if (c.empty()) continue;
        auto it = std::find_if(voices_.begin(), voices_.end(),
                               [&](const Voice& v) { return v.loop && v.key == l.key && v.sound == l.sound; });
        if (it == voices_.end()) {
            if (int(voices_.size()) >= kMaxVoices) continue;
            Voice v;
            v.sound = l.sound;
            v.loop = true;
            v.key = l.key;
            v.fade = 0.0f;
            // start each loop at its own place, so two steam jets don't play in unison
            v.cursor = double((uint64_t(l.key) * 2654435761u) % std::max<size_t>(1, c.frames()));
            v.serial = serial_++;
            voices_.push_back(v);
            it = voices_.end() - 1;
            it->pos = l.pos;
            it->positional = l.positional;
            it->gain = l.gain;
            it->occlusion = l.occlusion;
            targets(*it, it->gl, it->gr, it->send, it->lpA);
        }
        it->pos = l.pos;
        it->positional = l.positional;
        it->gain = l.gain;
        it->pitch = std::clamp(l.pitch, 0.25f, 4.0f);
        it->occlusion = l.occlusion;
        it->fadeTarget = 1.0f;
    }
}

void Mixer::render(float* out, int frames) {
    std::fill(out, out + size_t(frames) * 2, 0.0f);
    std::vector<float> wet(size_t(frames), 0.0f);
    const float fadeStep = 1.0f / (0.25f * float(kRate));
    const float smooth = 0.0025f;  // ~9 ms to settle on new gains
    for (Voice& v : voices_) {
        const SoundClip& c = bank_.clip(v.sound);
        const size_t n = c.frames();
        if (n < 2) {
            v.fade = v.fadeTarget = 0.0f;
            v.loop = false;
            v.cursor = 1e30;
            continue;
        }
        for (int base = 0; base < frames; base += 64) {
            const int end = std::min(frames, base + 64);
            float tl, tr, ts, ta;
            targets(v, tl, tr, ts, ta);
            for (int i = base; i < end; ++i) {
                if (!v.loop && v.cursor >= double(n - 1)) break;
                if (v.loop && v.cursor >= double(n)) v.cursor -= double(n);
                const size_t a = size_t(v.cursor);
                const size_t b = v.loop ? (a + 1) % n : std::min(n - 1, a + 1);
                const float f = float(v.cursor - double(a));
                float l, r;
                if (c.channels == 2) {
                    l = c.samples[a * 2] * (1.0f - f) + c.samples[b * 2] * f;
                    r = c.samples[a * 2 + 1] * (1.0f - f) + c.samples[b * 2 + 1] * f;
                    if (v.positional) l = r = 0.5f * (l + r);
                } else {
                    l = r = c.samples[a] * (1.0f - f) + c.samples[b] * f;
                }
                v.cursor += double(v.pitch);
                v.gl += (tl - v.gl) * smooth;
                v.gr += (tr - v.gr) * smooth;
                v.send += (ts - v.send) * smooth;
                v.lpA += (ta - v.lpA) * smooth;
                if (v.fade < v.fadeTarget) v.fade = std::min(v.fadeTarget, v.fade + fadeStep);
                else if (v.fade > v.fadeTarget) v.fade = std::max(v.fadeTarget, v.fade - fadeStep);
                v.lpL += v.lpA * (l - v.lpL);
                v.lpR += v.lpA * (r - v.lpR);
                out[size_t(i) * 2] += v.lpL * v.gl * v.fade;
                out[size_t(i) * 2 + 1] += v.lpR * v.gr * v.fade;
                wet[size_t(i)] += 0.5f * (v.lpL + v.lpR) * v.send * v.fade;
            }
        }
    }
    // a small metallic room
    for (int i = 0; i < frames; ++i) {
        const float in = wet[size_t(i)] * 0.25f;
        for (int side = 0; side < 2; ++side) {
            float acc = 0.0f;
            for (int k = 0; k < 4; ++k) {
                Comb& cb = combs_[size_t(side * 4 + k)];
                const float o = cb.buf[cb.i];
                cb.store = o * 0.65f + cb.store * 0.35f;  // damping
                cb.buf[cb.i] = in + cb.store * 0.80f;     // feedback
                cb.i = (cb.i + 1) % cb.buf.size();
                acc += o;
            }
            for (int k = 0; k < 2; ++k) {
                AllPass& ap = allpass_[size_t(side * 2 + k)];
                const float bo = ap.buf[ap.i];
                const float o = bo - acc;
                ap.buf[ap.i] = acc + bo * 0.5f;
                ap.i = (ap.i + 1) % ap.buf.size();
                acc = o;
            }
            out[size_t(i) * 2 + size_t(side)] += acc * 0.6f;
        }
    }
    // master level and a soft limiter
    for (int i = 0; i < frames * 2; ++i) {
        const float x = out[i] * 0.85f;
        const float ax = std::abs(x);
        peak_ = std::max(peak_, ax);
        if (ax > 0.9f) {
            ++overs_;
            out[i] = std::copysign(0.9f + 0.1f * std::tanh((ax - 0.9f) / 0.1f), x);
        } else {
            out[i] = x;
        }
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [&](const Voice& v) {
                                     const size_t n = bank_.clip(v.sound).frames();
                                     if (v.loop) return v.fadeTarget == 0.0f && v.fade <= 0.0f;
                                     return v.cursor >= double(n > 0 ? n - 1 : 0);
                                 }),
                  voices_.end());
    rendered_ += uint64_t(frames);
}

}  // namespace bh
