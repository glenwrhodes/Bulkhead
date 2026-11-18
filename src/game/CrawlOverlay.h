#pragma once
// The climb film's overlay (round 15): the crawl's machinery drawn over a rendered frame,
// so the plain and overlay films can be cut together frame for frame.
//
//   legs        each leg's two bones: hip to knee (femur), knee to claw (tibia), in its colour
//   footholds   where each claw is planted (a dot), and the next foothold it will step to (a ring)
//   step arc    for a claw in the air, the arc it follows from one foothold to the next
//   surface     a strip of the surface it is attached to, round the corridor's cross-section
//               where it is, and an arrow for which way is up for it (the surface normal)
//   gait chart  each leg's place in its step cycle: planted for the first 62%, then swinging
//   labels      in a plain sans (Segoe UI, from Windows)
//
// Drawn on the CPU over the finished image (lines anti-aliased, no depth test). No D3D12.

#include <functional>

#include "core/Image.h"
#include "core/Math.h"

namespace bh {

class Creature;
class CrawlPath;

struct CrawlOverlayInput {
    const Creature* creature = nullptr;
    const CrawlPath* path = nullptr;
    Camera camera;
    std::function<float(float)> uOf;  // round the cross-section at s (where it crawls)
    float sBody = 0.0f;       // its middle (thorax) on the path
    const char* stage = "";   // what it is on, for the caption
    float time = 0.0f;
};

void drawCrawlOverlay(Image& img, const CrawlOverlayInput& in);

}  // namespace bh
