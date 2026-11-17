#pragma once
// The ship is split into sections, each with a mood that sets how it is lit and how worn
// it looks:
//   Lit       clean and fully lit (the spawn section is always Lit)
//   Dim       fewer, warmer lights; some flicker or are dead
//   Dark      most lights dead; red emergency lights
//   Derelict  broken: flickering and strobing tubes, heavy grime and haze
// Sections grow outward from anchor cells along the corridors (a room always belongs to
// one section), so each section is a connected piece of the ship. Moods get darker the
// farther a section is from the start. Everything is derived from the maze and its seed
// with separate random streams, so the maze itself is unchanged. No D3D12 here.

#include <cstdint>
#include <string_view>
#include <vector>

#include "core/Math.h"
#include "world/Maze.h"

namespace bh {

enum class Mood : uint8_t { Lit, Dim, Dark, Derelict };
inline constexpr int kMoodCount = 4;
std::string_view toString(Mood m);

// How the lights of one cell behave over time.
enum class LightMode : uint8_t { Steady, Flicker, Strobe, Dead, Emergency };
std::string_view toString(LightMode m);

struct CellPower {
    LightMode mode = LightMode::Steady;
    float level = 1.0f;   // brightness relative to the kit's fixture
    Vec3 tint{1, 1, 1};   // colour multiplier
    float phase = 0.0f;   // 0..1, so neighbours do not flicker in step
};

// Radiance multiplier (colour) for a cell's lights at time t (seconds).
Vec3 lightMultiplier(const CellPower& p, float t);
// Multiplier for the emissive surfaces (light panels, screens) in the cell at time t.
Vec3 emissiveMultiplier(const CellPower& p, float t);

struct Sections {
    int width = 0, height = 0;
    std::vector<int> cellSection;   // per cell, -1 for void
    std::vector<Mood> mood;         // per section
    std::vector<Int2> anchor;       // per section: the cell it grew from
    std::vector<int> depth;         // per section: walking distance of the anchor from the start
    // per cell (void cells copy their neighbours, so shading blends smoothly up to walls)
    std::vector<CellPower> power;
    std::vector<float> decay;       // 0 clean .. 1 filthy (grime, rust streaks)
    std::vector<float> haze;        // haze density, 1/m

    int count() const { return int(mood.size()); }
    int sectionAt(int x, int y) const {
        return (x < 0 || y < 0 || x >= width || y >= height) ? -1 : cellSection[size_t(y * width + x)];
    }
    // Void and out-of-range cells report Lit.
    Mood moodAt(int x, int y) const {
        int s = sectionAt(x, y);
        return s < 0 ? Mood::Lit : mood[size_t(s)];
    }
    Mood moodAt(Int2 c) const { return moodAt(c.x, c.y); }
};

Sections buildSections(const Maze& maze);

}  // namespace bh
