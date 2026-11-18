// Unit tests for the sound (part 3): the sound bank (the real assets/sounds files), the
// mixer (timing to the sample, left/right, distance, walls, loops) and the sounds the game
// produces in the hunt and fight scenes. No GPU, no audio device.
// Usage: audio_tests <assets dir>

#include <cmath>
#include <cstdio>
#include <format>
#include <map>
#include <string>
#include <vector>

#include "assets/Kit.h"
#include "audio/Mixer.h"
#include "game/Game.h"

using namespace bh;

namespace {

int g_checks = 0, g_failures = 0;
void check(bool ok, const std::string& what, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL line %d: %s\n", line, what.c_str());
    }
}
#define CHECK(c) check((c), #c, __LINE__)
#define CHECK_MSG(c, m) check((c), (m), __LINE__)

double rms(const std::vector<float>& v, int channel) {
    double s = 0.0;
    size_t n = 0;
    for (size_t i = size_t(channel); i < v.size(); i += 2, ++n) s += double(v[i]) * double(v[i]);
    return n ? std::sqrt(s / double(n)) : 0.0;
}

SoundClip tone(float hz, float seconds, float amp = 0.3f) {
    SoundClip c;
    c.channels = 1;
    const size_t n = size_t(seconds * Mixer::kRate);
    c.samples.resize(n);
    for (size_t i = 0; i < n; ++i) c.samples[i] = amp * std::sin(2.0f * kPi * hz * float(i) / Mixer::kRate);
    return c;
}

void testBank(const std::filesystem::path& assets) {
    SoundBank bank;
    std::string report;
    const int n = bank.load(assets / "sounds", report);
    std::printf("%s", report.c_str());
    CHECK_MSG(n == kSoundCount, std::format("{} of {} sounds loaded", n, kSoundCount));
    for (int i = 0; i < kSoundCount; ++i) {
        const SoundClip& c = bank.clip(Snd(i));
        float pk = 0.0f;
        for (float s : c.samples) pk = std::max(pk, std::abs(s));
        CHECK_MSG(c.frames() > size_t(Mixer::kRate / 5), std::format("{} is shorter than 0.2 s", soundFile(Snd(i))));
        CHECK_MSG(pk > 0.02f, std::format("{} is silent", soundFile(Snd(i))));
    }
}

void testMixer() {
    SoundBank bank;
    // a click: one full-scale sample, then silence
    SoundClip click;
    click.samples.assign(4410, 0.0f);
    click.samples[0] = 1.0f;
    bank.set(Snd::ScannerPing, click);
    bank.set(Snd::Steam, tone(440.0f, 1.0f));
    bank.set(Snd::GunFire, tone(1000.0f, 0.5f));
    Mixer m(bank);
    // timing: a sound starts on the first sample rendered after play()
    std::vector<float> out(1470 * 2);
    m.render(out.data(), 1470);
    SoundEvent e;
    e.sound = Snd::ScannerPing;
    e.positional = false;
    e.gain = 0.5f;
    m.play(e);
    m.render(out.data(), 1470);
    int first = -1;
    for (int i = 0; i < 1470; ++i)
        if (std::abs(out[size_t(i) * 2]) > 0.05f) {
            first = i;
            break;
        }
    std::printf("  click played at sample %d of the frame; rendered %llu frames\n", first,
                (unsigned long long)m.framesRendered());
    CHECK(first == 0);
    CHECK(m.framesRendered() == 2940);

    // left and right: a tone to the listener's right is louder on the right
    auto level = [&](Vec3 pos, float occlusion, int channel) {
        Mixer mm(bank);
        Listener L;
        mm.setListener(L);
        SoundEvent t;
        t.sound = Snd::GunFire;
        t.pos = pos;
        t.occlusion = occlusion;
        mm.play(t);
        std::vector<float> o(4410 * 2);
        mm.render(o.data(), 4410);
        return rms(o, channel);
    };
    const double l = level({3, 0, 0}, 0, 0), r = level({3, 0, 0}, 0, 1);
    std::printf("  tone 3 m to the right: left %.3f, right %.3f\n", l, r);
    CHECK(r > 2.0 * l);
    // distance: 10 m is about a third as loud as 2 m (half at 4 m, a quarter at 10 m)
    const double near = level({0, 0, -2}, 0, 0) + level({0, 0, -2}, 0, 1);
    const double far = level({0, 0, -10}, 0, 0) + level({0, 0, -10}, 0, 1);
    std::printf("  tone ahead at 2 m %.3f, at 10 m %.3f (ratio %.2f)\n", near, far, near / far);
    CHECK(near / far > 2.3 && near / far < 4.0);
    // behind a wall: quieter and muffled (a 1 kHz tone loses some to the low-pass too)
    const double walled = level({0, 0, -4}, 1, 0) + level({0, 0, -4}, 1, 1);
    const double open = level({0, 0, -4}, 0, 0) + level({0, 0, -4}, 0, 1);
    std::printf("  tone 4 m ahead: open %.3f, behind a wall %.3f\n", open, walled);
    CHECK(walled < 0.6 * open);

    // loops: fade in when listed, out when not
    Mixer lm(bank);
    std::vector<SoundLoop> loops{{Snd::Steam, 7u, {0, 0, -2}, true, 1.0f, 1.0f, 0.0f}};
    std::vector<float> o(Mixer::kRate * 2);
    lm.setLoops(loops);
    lm.render(o.data(), Mixer::kRate / 2);  // half a second: faded in
    const double on = rms(std::vector<float>(o.begin() + Mixer::kRate / 2, o.begin() + Mixer::kRate), 0);
    lm.setLoops({});
    lm.render(o.data(), Mixer::kRate / 2);  // half a second later: gone
    std::vector<float> tail(o.begin() + Mixer::kRate / 2, o.begin() + Mixer::kRate);
    const double off = rms(tail, 0);
    std::printf("  loop: playing %.3f, half a second after it was dropped %.4f; voices left %d\n", on, off, lm.voices());
    CHECK(on > 0.05 && off < 0.01 && lm.voices() == 0);
}

// The hunt and fight scenes, stepped as the capture does: the sounds each should make.
void testGameSounds(const Kit& kit) {
    MazeParams p;
    p.width = p.height = 40;
    p.seed = 1234;
    for (bool fight : {false, true}) {
        Game game(kit, p);
        CHECK(game.setupHuntScene(fight));
        std::vector<SoundEvent> ev;
        std::vector<SoundLoop> loops;
        std::map<std::string, int> seen;
        std::map<std::string, float> firstAt;
        bool skitter = false, feeding = false, hum = false;
        const float dt = 1.0f / 30.0f;
        game.collectSounds(ev, loops);
        for (float t = 0.0f; t < game.huntSceneLength() && !game.huntSceneDone(t); t += dt) {
            game.stepHuntScene(t, dt);
            game.collectSounds(ev, loops);
            for (const SoundEvent& e : ev) {
                const std::string n = soundFile(e.sound);
                if (!seen[n]++) firstAt[n] = t;
            }
            for (const SoundLoop& l : loops) {
                skitter = skitter || l.sound == Snd::CreatureSkitter;
                feeding = feeding || l.sound == Snd::Feeding;
                hum = hum || l.sound == Snd::ShipHum;
            }
        }
        std::string line;
        for (const auto& [k, v] : seen) line += std::format(" {} x{} (first {:.1f} s),", k, v, firstAt[k]);
        std::printf("  %s scene:%s skitter %s, feeding %s\n", fight ? "fight" : "hunt", line.c_str(),
                    skitter ? "yes" : "no", feeding ? "yes" : "no");
        CHECK(hum && skitter);
        CHECK(seen["claw_1"] + seen["claw_2"] > 10);
        // (the hunt's two panicked shots: one hits it, one the wall; the fight's four all hit)
        CHECK(seen["gun_fire"] >= 2 && seen["hit_creature"] >= (fight ? 4 : 1));
        CHECK(seen["creature_hiss"] >= 1);
        if (fight) {
            CHECK(seen["creature_death"] == 1);
        } else {
            CHECK(seen["creature_screech"] >= 1 && seen["player_thud"] == 1 && feeding);
            CHECK(firstAt["creature_screech"] < firstAt["player_thud"]);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path assets = argc > 1 ? argv[1] : findAssetsDirectory({});
    std::printf("[ RUN  ] sound bank\n");
    testBank(assets);
    std::printf("[ RUN  ] mixer\n");
    testMixer();
    Kit kit;
    std::string error;
    if (!kit.loadDirectory(assets, error)) {
        std::printf("cannot load kit: %s\n", error.c_str());
        return 1;
    }
    std::printf("[ RUN  ] the sounds of the hunt and the fight\n");
    testGameSounds(kit);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
