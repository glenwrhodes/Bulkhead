#pragma once
// Per-frame effect data the game hands to the renderer: short-lived lights (spark
// bursts, the muzzle flash, bolts, impacts) and steam plumes. No D3D12 here.

#include <vector>

#include "core/Math.h"

namespace bh {

// A point light that is not in the level's light grid: every pixel considers it.
struct DynamicLight {
    Vec3 position;
    Vec3 radiance;        // colour x intensity, W/sr
    float radius = 0.05f; // for soft shadows
    float range = 8.0f;
};

// A jet of steam from a torn pipe: a cone that widens and bends upward, with animated
// turbulence. light is the light falling on it from the fixtures nearby (W/sr / m^2, so
// colour x intensity / distance^2 summed), computed on the CPU each frame.
struct SteamPlume {
    Vec3 origin;
    Vec3 direction{1, 0, 0};
    float length = 1.6f;
    float strength = 1.0f;  // 0 = off .. 1 = full jet (it hisses on and off)
    Vec3 light;
    float seed = 0.0f;
};

// The motion scanner (round 17), drawn on the minimap and the map: a soft blob where
// something moved (never exactly where it is), and the pulse sweeping out from the player.
struct ScanBlip {
    float x = 0.0f, z = 0.0f;  // world metres
    float radius = 2.0f;       // metres
    float alpha = 1.0f;        // 0..1 (fades after the pulse that found it)
};
struct ScanState {
    const std::vector<ScanBlip>* blips = nullptr;
    float front = -1.0f;       // metres out from the player of the pulse's front (< 0: none)
    float frontAlpha = 0.0f;
    float range = 25.0f;
};

}  // namespace bh
