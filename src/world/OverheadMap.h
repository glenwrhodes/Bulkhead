#pragma once
// Renders the maze as a top-down "tactical map" PNG: corridors, rooms (tinted by type),
// walls, doors (by type), the start and exit, and the route between them.

#include <vector>

#include "core/Image.h"
#include "world/Maze.h"
#include "world/PathTrack.h"

namespace bh {

struct OverheadStyle {
    int cellPx = 64;    // pixels per 4 m cell
    int marginPx = 32;  // border around the grid
    bool drawRoute = true;
};

struct OverheadMap {
    Image image;
    int cellPx = 0;
    int marginPx = 0;
    std::vector<Int2> route;  // start -> exit, in cells
    PathTrack routeTrack;     // the same route through cell centres, in image pixels

    Float2 cellCenterPx(Int2 c) const {
        return {marginPx + (c.x + 0.5f) * cellPx, marginPx + (c.y + 0.5f) * cellPx};
    }
};

OverheadMap renderOverheadMap(const Maze& maze, const OverheadStyle& style = {});

// Palette, shared with tests.
Color roomFloorColor(RoomKind k);
inline constexpr Color kStartColor = rgb(0x3ddc84);
inline constexpr Color kExitColor = rgb(0xff4d4d);

}  // namespace bh
