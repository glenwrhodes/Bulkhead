#pragma once
// The ship layout: a grid of 4 m cells. Each cell is Void (solid ship machinery),
// Corridor, or part of a Room. Each edge between two cells is a Wall, Open, or a Door.
//
// Grid coordinates: x grows east, y grows south (row index). North is y - 1.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bh {

class Rng;

enum class Dir : uint8_t { North = 0, East = 1, South = 2, West = 3 };
inline constexpr int kDx[4] = {0, 1, 0, -1};
inline constexpr int kDy[4] = {-1, 0, 1, 0};
constexpr Dir opposite(Dir d) { return Dir((int(d) + 2) & 3); }

enum class CellKind : uint8_t { Void, Corridor, Room };
enum class Edge : uint8_t { Wall, Open, Door };
enum class DoorKind : uint8_t { None, Sliding, Bulkhead, Airlock };
enum class RoomKind : uint8_t { Spawn, Airlock, Storage, Engineering, Crew, Medbay, Reactor };

std::string_view toString(RoomKind k);
std::string_view toString(DoorKind k);

struct Int2 {
    int x = 0, y = 0;
    bool operator==(const Int2&) const = default;
};

struct Room {
    int id = -1;
    int x = 0, y = 0, w = 0, h = 0;
    RoomKind kind = RoomKind::Storage;

    bool contains(int cx, int cy) const { return cx >= x && cy >= y && cx < x + w && cy < y + h; }
    Int2 center() const { return {x + w / 2, y + h / 2}; }
};

struct MazeParams {
    int width = 12;
    int height = 12;
    uint64_t seed = 1234;

    int roomAttempts = 0;          // 0 = derive from map size
    int minRoomSize = 2;           // cells
    int maxRoomSize = 4;           // cells (a room may get +1 along one axis)
    float roomCoverage = 0.38f;    // stop placing rooms once this fraction of cells is rooms
    float straightness = 0.55f;    // corridor carver keeps going straight with this probability
    float extraDoorChance = 0.45f; // per room: chance of an extra door (makes loops)
    float braidChance = 0.45f;     // dead ends joined to a neighbour corridor (loops)
    float pruneChance = 0.55f;     // remaining dead-end branches removed back to a junction
    float bulkheadDoorChance = 0.3f;
};

class Maze {
public:
    // Throws std::invalid_argument for unusable parameters (size outside 6..128, bad room sizes).
    static Maze generate(const MazeParams& params);

    int width() const { return width_; }
    int height() const { return height_; }
    const MazeParams& params() const { return params_; }

    bool inBounds(int x, int y) const { return x >= 0 && y >= 0 && x < width_ && y < height_; }
    bool inBounds(Int2 c) const { return inBounds(c.x, c.y); }
    CellKind kind(int x, int y) const;  // Void when out of bounds
    CellKind kind(Int2 c) const { return kind(c.x, c.y); }
    int roomAt(int x, int y) const;     // -1 when not a room cell

    Edge edge(int x, int y, Dir d) const;       // Wall at the map border
    DoorKind door(int x, int y, Dir d) const;   // None unless edge is a Door
    bool passable(int x, int y, Dir d) const { return edge(x, y, d) != Edge::Wall; }
    int passableCount(int x, int y) const;

    const std::vector<Room>& rooms() const { return rooms_; }
    int startRoom() const { return startRoom_; }
    int exitRoom() const { return exitRoom_; }
    Int2 startCell() const { return rooms_[size_t(startRoom_)].center(); }
    Int2 exitCell() const { return rooms_[size_t(exitRoom_)].center(); }

    // Shortest route through passable edges (A*). Includes both ends. Empty if none.
    std::vector<Int2> findPath(Int2 from, Int2 to) const;
    // Breadth-first step distance from a cell to every cell (-1 = unreachable).
    std::vector<int> distancesFrom(Int2 from) const;

    // Stats
    int countCells(CellKind k) const;
    int countDoors() const;
    int countDeadEnds() const;
    int countLoops() const;  // independent cycles: passable edges - walkable cells + 1

    uint64_t hash() const;        // deterministic fingerprint of the whole layout
    std::string toAscii() const;  // text drawing for logs
    std::string describe() const; // one-line summary

private:
    struct EdgeData {
        Edge type = Edge::Wall;
        DoorKind door = DoorKind::None;
    };

    Maze(const MazeParams& params);
    size_t index(int x, int y) const { return size_t(y) * size_t(width_) + size_t(x); }
    const EdgeData* edgePtr(int x, int y, Dir d) const;
    void setEdge(int x, int y, Dir d, Edge type, DoorKind door = DoorKind::None);
    void setKind(int x, int y, CellKind k) { cells_[index(x, y)] = k; }
    bool isDeadEnd(int x, int y) const;

    void placeRooms(Rng& rng);
    void addRoom(int x, int y, int w, int h);
    void carveCorridors(Rng& rng);
    void connectRegions(Rng& rng);
    void braidAndPrune(Rng& rng);
    void chooseStartAndExit(Rng& rng);
    void assignKinds(Rng& rng);

    MazeParams params_;
    int width_ = 0;
    int height_ = 0;
    std::vector<CellKind> cells_;
    std::vector<int> roomId_;
    std::vector<EdgeData> east_;   // edge on the east side of each cell
    std::vector<EdgeData> south_;  // edge on the south side of each cell
    std::vector<Room> rooms_;
    int startRoom_ = -1;
    int exitRoom_ = -1;
};

}  // namespace bh
