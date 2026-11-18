#pragma once
// The encounter timer (round 11): how long a player exploring the ship takes to first
// see a creature. CPU only, no D3D12.
//
// A simulated player walks the ship at the window's walking speed (3.4 m/s, W held),
// always heading for the nearest cell the map has not shown yet, facing where it walks.
// The real Game and creature AI run around it at fixed steps. It does not collide with
// props or crouch (it passes through them), and it never fires.
//
// "Seen" means a live creature's head or body centre is within 25 m, inside the window's
// field of view (65 degrees tall at 16:9, so about 97 wide), with nothing in the way
// (raycastLevel through walls, closed doors and props). Lighting is ignored: in the dark
// it would show by its own amber glow and the flashlight. If a creature kills the player
// before it was ever seen, that counts as the first encounter (an ambush). After a death
// the run restarts at the spawn, as the window does.

#include <cstdint>
#include <string>
#include <vector>

#include "assets/Kit.h"
#include "game/Hunter.h"

namespace bh {

struct EncounterConfig {
    HuntTuning tuning;
    float minutes = 10.0f;  // each run's length (first sightings later than this count as none)
    float dt = 1.0f / 30.0f;
    int mazeWidth = 40, mazeHeight = 40;
};

struct EncounterRun {
    uint64_t seed = 0;
    int variant = 0;           // varies the player's route choices and where the creatures start
    int spawned = 0;           // creatures that found a place at the start
    float firstSeen = -1.0f;   // seconds; -1 = never in the run
    float firstGrab = -1.0f;   // the first death
    float firstNear = -1.0f;   // first time one is within 25 m (walls or not): what a scanner would show
    float encounter = -1.0f;   // min(firstSeen, firstGrab)
    int seenSurface = -1;      // what it was on when first seen (Hunter::surfaceKind)
    int seenState = -1;        // HunterState
    float seenDistance = 0.0f;
    int sightings = 0;         // a creature coming into view after at least 8 s out of it
    int deaths = 0;
    float viewSeconds[4] = {0, 0, 0, 0};  // creature-seconds in view by surface (floor, wall, ceiling, air)
    float maxInView = 0;       // most creatures in view at once
    int explored = 0;          // cells the map showed by the end
    int drifts = 0;            // drift legs the creatures started (round 11; creatures replaced after a death not counted)
    double msPerStep = 0.0;    // CPU time of one game step (world + creatures + the check)
};

EncounterRun runEncounter(const Kit& kit, uint64_t seed, int variant, const EncounterConfig& cfg);

// Runs seeds x variants in parallel and returns them in order.
std::vector<EncounterRun> runEncounters(const Kit& kit, const std::vector<uint64_t>& seeds, int variants,
                                        const EncounterConfig& cfg);

// The summary table as text lines (mean, median, quartiles, ...).
std::vector<std::string> summarizeEncounters(const std::vector<EncounterRun>& runs, const EncounterConfig& cfg);

}  // namespace bh
