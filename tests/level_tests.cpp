// Unit tests for the kit loader (real assets/*.glb), level builder, collision, scripted
// walk and game state. Usage: level_tests <assets dir>

#include <cmath>
#include <cstdio>
#include <format>
#include <set>
#include <string>
#include <vector>

#include "assets/Kit.h"
#include "game/Game.h"
#include "world/Collision.h"
#include "world/Level.h"
#include "world/WalkPath.h"

using namespace bh;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s:%d: %s\n", file, line, what.c_str());
    }
}
#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)
#define CHECK_MSG(cond, msg) check((cond), (msg), __FILE__, __LINE__)

Kit g_kit;

const char* kRequired[] = {"corridor_straight", "corridor_corner", "corridor_tee", "corridor_cross", "corridor_end",
                           "door_frame", "door_sliding", "door_bulkhead", "door_airlock", "room_floor",
                           "room_ceiling", "room_wall_a", "room_wall_b", "room_wall_door", "room_corner",
                           "crate_small", "crate_medium", "crate_long", "barrel_blue", "barrel_red", "canister",
                           "console", "handgun"};

void testKit() {
    CHECK(g_kit.models().size() >= 23);
    for (const char* name : kRequired) {
        int i = g_kit.find(name);
        CHECK_MSG(i >= 0, std::format("kit model {} missing", name));
        if (i < 0) continue;
        const KitModel& m = g_kit.model(i);
        CHECK_MSG(!m.parts.empty() && m.triangles > 0, std::format("{} has no geometry", name));
    }
    // index ranges stay inside their submesh
    bool ok = true;
    for (const KitSubmesh& s : g_kit.submeshes())
        for (uint32_t i = 0; i < s.indexCount && ok; ++i) ok = g_kit.indices()[s.firstIndex + i] < s.vertexCount;
    CHECK(ok);

    // door leaves are separate parts the engine can animate
    const KitModel& sliding = g_kit.model(g_kit.find("door_sliding"));
    int leafL = 0, leafR = 0, glass = 0;
    for (uint32_t p : sliding.parts) {
        const KitPart& part = g_kit.parts()[p];
        leafL += part.name.find("LeafL") != std::string::npos;
        leafR += part.name.find("LeafR") != std::string::npos;
        glass += part.transparent;
    }
    CHECK(leafL > 0 && leafR > 0);
    CHECK(glass > 0);

    // corridors carry their fixture lights; the cell is 4 m wide and Y is up
    const KitModel& straight = g_kit.model(g_kit.find("corridor_straight"));
    CHECK(straight.lights.size() == 2);
    for (const KitLight& l : straight.lights) CHECK(l.intensity > 1.0f && l.position.y > 2.0f && l.position.y < 2.9f);
    CHECK(std::abs(straight.boundsMax.x - 2.0f) < 0.05f && std::abs(straight.boundsMin.z + 2.0f) < 0.05f);
    CHECK(straight.boundsMax.y > 2.8f && straight.boundsMax.y < 3.0f);

    // the handgun is real scale and points down -Z
    const KitModel& gun = g_kit.model(g_kit.find("handgun"));
    float len = gun.boundsMax.z - gun.boundsMin.z;
    CHECK_MSG(len > 0.18f && len < 0.32f, std::format("handgun length {}", len));

    bool emissive = false, metallic = false;
    for (const KitMaterial& m : g_kit.materials()) {
        emissive |= m.emissive[0] + m.emissive[1] + m.emissive[2] > 1.0f;
        metallic |= m.metallic > 0.8f;
    }
    CHECK(emissive && metallic);
    std::printf("  kit: %zu models, %zu parts, %zu materials, %zu vertices, %zu triangles\n", g_kit.models().size(),
                g_kit.parts().size(), g_kit.materials().size(), g_kit.vertices().size(), g_kit.indices().size() / 3);
}

void testLevels() {
    std::vector<float> closed, open;
    for (uint64_t seed = 1; seed <= 40; ++seed) {
        MazeParams p;
        p.seed = seed;
        const Maze maze = Maze::generate(p);
        const Level level = buildLevel(maze, g_kit);
        const std::string tag = std::format("seed {}", seed);
        CHECK_MSG(level.missingModels.empty(), tag + ": missing kit models");
        CHECK_MSG(int(level.doors.size()) == maze.countDoors(), tag + ": door count");
        closed.assign(level.doors.size(), 0.0f);
        open.assign(level.doors.size(), 1.0f);

        // every corridor and room cell has a light in it
        int unlit = 0;
        for (int y = 0; y < maze.height(); ++y)
            for (int x = 0; x < maze.width(); ++x)
                if (maze.kind(x, y) != CellKind::Void && level.cellCount[size_t(y * maze.width() + x)] == 0) ++unlit;
        CHECK_MSG(unlit == 0, std::format("{}: {} unlit cells", tag, unlit));

        CHECK_MSG(!level.collision.overlaps({level.spawn.x, level.spawn.z}, 0.32f, closed), tag + ": spawn blocked");
        CHECK_MSG(!level.collision.overlaps({level.exitPoint.x, level.exitPoint.z}, 0.32f, closed),
                  tag + ": exit blocked");

        // the scripted walk never clips a wall, prop or door jamb (doors open)
        WalkPath walk(level.route);
        int clipped = 0;
        for (float s = 0; s <= walk.length(); s += 0.25f)
            if (level.collision.overlaps(walk.at(s), 0.25f, open)) ++clipped;
        CHECK_MSG(clipped == 0, std::format("{}: walk clips geometry at {} samples", tag, clipped));
        Vec2 a = walk.at(0), b = walk.at(walk.length());
        CHECK(std::abs(a.x - level.spawn.x) < 0.01f && std::abs(a.y - level.spawn.z) < 0.01f);
        CHECK(std::abs(b.x - level.exitPoint.x) < 0.01f && std::abs(b.y - level.exitPoint.z) < 0.01f);

        // walking into walls keeps the player inside walkable cells
        for (int d = 0; d < 4; ++d) {
            Vec3 dir = dirVector(Dir(d));
            Vec2 pos = level.collision.move({level.spawn.x, level.spawn.z}, {dir.x * 60.0f, dir.z * 60.0f}, 0.32f,
                                            closed);
            int cx = int(std::floor(pos.x / kCell + 0.5f)), cy = int(std::floor(pos.y / kCell + 0.5f));
            CHECK_MSG(maze.inBounds(cx, cy) && maze.kind(cx, cy) == CellKind::Room,
                      std::format("{}: escaped the spawn room going {}", tag, d));
        }
    }
}

void testDoors() {
    MazeParams p;
    p.seed = 1234;
    const Maze maze = Maze::generate(p);
    const Level level = buildLevel(maze, g_kit);
    CHECK(!level.doors.empty());
    const Door& door = level.doors[0];
    Vec3 n = dirVector(door.side);
    std::vector<float> state(level.doors.size(), 0.0f);
    Vec2 from{door.center.x - n.x * 1.5f, door.center.z - n.z * 1.5f};
    Vec2 delta{n.x * 3.0f, n.z * 3.0f};
    Vec2 shut = level.collision.move(from, delta, 0.32f, state);
    float before = (shut.x - door.center.x) * n.x + (shut.y - door.center.z) * n.z;
    CHECK_MSG(before < 0.0f, "a closed door let the player through");
    state[0] = 1.0f;
    Vec2 through = level.collision.move(from, delta, 0.32f, state);
    float after = (through.x - door.center.x) * n.x + (through.y - door.center.z) * n.z;
    CHECK_MSG(after > 1.0f, std::format("an open door blocked the player (got {:.2f})", after));
}

void testGame() {
    MazeParams p;
    p.seed = 1234;
    Game game(g_kit, p);
    CHECK(game.walkDuration() > 5.0f);
    game.setWalkTime(0.0f);
    CHECK(!game.reachedExit());
    game.setWalkTime(game.walkDuration());
    CHECK(game.reachedExit());

    std::vector<SceneInstance> instances;
    game.buildInstances(instances);
    size_t parts = 0;
    for (const PlacedModel& pm : game.level().placed) parts += g_kit.model(pm.model).parts.size();
    size_t gunParts = g_kit.model(g_kit.find("handgun")).parts.size();
    CHECK(instances.size() == parts + gunParts);
    int gun = 0;
    for (const SceneInstance& si : instances) gun += (si.flags & kInstanceGun) != 0;
    CHECK(size_t(gun) == gunParts);

    // the player stays put without input and moves forward with it
    Game g2(g_kit, p);
    Vec3 start = g2.camera().position;
    g2.update(0.1f, InputState{});
    CHECK(length(g2.camera().position - start) < 0.05f);
    InputState fwd;
    fwd.forward = true;
    for (int i = 0; i < 30; ++i) g2.update(1.0f / 30.0f, fwd);
    CHECK(length(g2.camera().position - start) > 0.5f);

    const auto shots = game.findShots();
    std::set<std::string> names;
    std::vector<float> none(game.level().doors.size(), 0.0f);
    for (const Shot& s : shots) {
        names.insert(s.name);
        CHECK_MSG(!game.level().collision.overlaps({s.camera.position.x, s.camera.position.z}, 0.2f, none),
                  "shot camera inside geometry: " + s.name);
    }
    CHECK(names.count("corridor") && names.count("door") && names.count("room") && names.count("junction"));
}

// The camera bob: about two footsteps a second, not a rabbit's hop.
void testBob() {
    CHECK(Game::stepRate(Game::kWalkSpeed) > 1.8f && Game::stepRate(Game::kWalkSpeed) < 2.2f);
    CHECK(Game::stepRate(3.4f) < 2.4f && Game::stepRate(5.8f) < 2.8f);
    MazeParams p;
    p.seed = 1234;
    Game game(g_kit, p);
    // count upward crossings of eye height over 8 s of the scripted walk
    int crossings = 0;
    float prev = 0.0f;
    const float t0 = 2.0f, t1 = std::min(10.0f, game.walkDuration() - Game::kEndPause - 0.5f);
    for (float t = t0; t <= t1; t += 1.0f / 240.0f) {
        game.setWalkTime(t);
        float y = game.camera().position.y - kEyeHeight;
        if (t > t0 && prev <= 0.0f && y > 0.0f) ++crossings;
        prev = y;
    }
    const float hz = float(crossings) / (t1 - t0);
    std::printf("  scripted walk bob: %.2f per second (was 3.5)\n", hz);
    CHECK_MSG(hz > 1.7f && hz < 2.3f, std::format("bob {:.2f} Hz", hz));
}

// Sections on the full-size ship: changing lights, emissive power, explored cells, the
// flashlight, and one corridor shot per mood.
void testMood() {
    MazeParams p;
    p.width = p.height = 40;
    p.seed = 1234;
    Game game(g_kit, p);
    const Level& level = game.level();
    CHECK(level.missingModels.empty());
    CHECK(game.lightRadiance().size() == level.lights.size());

    int dead = 0, lit = 0;
    for (const Vec3& r : game.lightRadiance()) (r.x + r.y + r.z < 1e-4f ? dead : lit)++;
    std::printf("  40x40 seed 1234: %zu lights, %d dark right now, %d lit\n", level.lights.size(), dead, lit);
    CHECK(dead > 50 && lit > 200);

    // flicker: some lights change over a few seconds
    const std::vector<Vec3> before = game.lightRadiance();
    int changed = 0;
    for (float t = 0.1f; t < 6.0f; t += 0.1f) {
        game.setTime(t);
        for (size_t i = 0; i < before.size(); ++i)
            if (std::abs(game.lightRadiance()[i].x - before[i].x) > 0.5f) {
                ++changed;
                break;
            }
    }
    CHECK_MSG(changed > 10, std::format("lights changed in only {} of 59 steps", changed));

    std::vector<SceneInstance> instances;
    game.buildInstances(instances);
    int darkPanels = 0, gunNeutral = 0;
    for (const SceneInstance& si : instances) {
        darkPanels += si.emissive.x < 0.05f;
        if (si.flags & kInstanceGun) gunNeutral += si.emissive.x == 1.0f && si.emissive.y == 1.0f;
    }
    CHECK(darkPanels > 100);
    CHECK(gunNeutral > 0);

    // explored: the spawn is seen at once, the walk reveals more, never the whole ship
    auto seen = [&] {
        int n = 0;
        for (uint8_t e : game.explored()) n += e != 0;
        return n;
    };
    const Int2 spawn = game.maze().startCell();
    CHECK(game.explored()[size_t(spawn.y * 40 + spawn.x)] != 0);
    const int atStart = seen();
    for (float t = 0.0f; t < game.walkDuration(); t += 0.25f) {
        game.setWalkTime(t);
        game.updateDoors(0.25f);
    }
    const int atEnd = seen();
    std::printf("  explored: %d cells at the start, %d at the exit (of %d)\n", atStart, atEnd, 40 * 40);
    CHECK(atEnd > atStart + 30 && atEnd < 40 * 40);

    const Flashlight f = game.flashlight();
    CHECK(f.on && f.intensity > 0.0f && f.cosInner > f.cosOuter);
    CHECK(std::abs(length(f.direction) - 1.0f) < 1e-3f && length(f.position - game.camera().position) < 1.0f);
    CHECK(dot(f.direction, game.camera().forward()) > 0.98f);
    game.setFlashlight(false);
    CHECK(!game.flashlight().on);

    // one corridor shot per mood, standing inside that mood
    std::vector<float> none(level.doors.size(), 0.0f);
    std::set<std::string> names;
    for (const Shot& s : game.findShots()) {
        names.insert(s.name);
        CHECK_MSG(!level.collision.overlaps({s.camera.position.x, s.camera.position.z}, 0.2f, none),
                  "shot camera inside geometry: " + s.name);
        if (!s.name.starts_with("corridor_")) continue;
        game.setCamera(s.camera);
        CHECK_MSG(s.name == "corridor_" + std::string(toString(game.moodAtCamera())),
                  s.name + " is in a " + std::string(toString(game.moodAtCamera())) + " section");
    }
    for (const char* n : {"corridor_lit", "corridor_dim", "corridor_dark", "corridor_derelict"})
        CHECK_MSG(names.count(n) == 1, std::string("no shot ") + n);

    // a few full-size levels pass the same structural checks as the small ones
    for (uint64_t seed : {7u, 42u, 2026u}) {
        p.seed = seed;
        const Maze maze = Maze::generate(p);
        const Level l = buildLevel(maze, g_kit);
        std::vector<float> open(l.doors.size(), 1.0f);
        WalkPath walk(l.route);
        int clipped = 0;
        for (float s = 0; s <= walk.length(); s += 0.25f)
            if (l.collision.overlaps(walk.at(s), 0.25f, open)) ++clipped;
        CHECK_MSG(clipped == 0, std::format("40x40 seed {}: walk clips geometry at {} samples", seed, clipped));
        CHECK(l.missingModels.empty() && int(l.doors.size()) == maze.countDoors());
    }
}

void testCollision() {
    Collision c;
    c.add({0, 0, 1, 1});
    std::vector<float> none;
    Vec2 p = c.resolve({1.2f, 0.5f}, 0.3f, none);
    CHECK(std::abs(p.x - 1.3f) < 1e-4f);
    p = c.move({-1.0f, 0.5f}, {3.0f, 0.0f}, 0.3f, none);
    CHECK(p.x <= -0.3f + 1e-3f);
    c.add({2, 0, 3, 1, 0});
    std::vector<float> doorOpen{1.0f};
    CHECK(!c.overlaps({2.5f, 0.5f}, 0.1f, doorOpen));
    doorOpen[0] = 0.0f;
    CHECK(c.overlaps({2.5f, 0.5f}, 0.1f, doorOpen));
}

}  // namespace

int main(int argc, char** argv) {
    std::string error;
    std::filesystem::path assets = argc > 1 ? argv[1] : findAssetsDirectory({});
    if (!g_kit.loadDirectory(assets, error)) {
        std::printf("cannot load kit: %s\n", error.c_str());
        return 1;
    }
    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {{"kit", testKit}, {"collision", testCollision}, {"levels (40 seeds)", testLevels},
                          {"doors", testDoors}, {"game", testGame}, {"camera bob", testBob},
                          {"sections, lights, map, flashlight (40x40)", testMood}};
    for (const Test& t : tests) {
        const int before = g_failures;
        std::printf("[ RUN  ] %s\n", t.name);
        try {
            t.fn();
        } catch (const std::exception& ex) {
            ++g_failures;
            std::printf("  EXCEPTION: %s\n", ex.what());
        }
        std::printf("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", t.name);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
