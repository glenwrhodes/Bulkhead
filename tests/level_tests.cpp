// Unit tests for the kit loader (real assets/*.glb), level builder, collision, scripted
// walk and game state. Usage: level_tests <assets dir>

#include <array>
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
        for (float s = 0; s <= walk.length(); s += 0.25f) {
            // crouching under low pipes
            const float h = level.collision.headroom(walk.at(s), 0.25f) > 0.0f ? kCrouchHeight : kStandHeight;
            if (level.collision.overlaps(walk.at(s), 0.25f, open, h)) ++clipped;
        }
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
    size_t gunParts = g_kit.model(g_kit.find("handgun")).parts.size() + g_kit.model(g_kit.find("arms")).parts.size();
    size_t world = 0, gun = 0;
    for (const SceneInstance& si : instances) {
        world += si.flags == 0;
        gun += (si.flags & kInstanceGun) != 0;
    }
    CHECK(world == parts);   // no scorch marks yet
    CHECK(gun == gunParts);  // the gun and the arms holding it

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
        for (float s = 0; s <= walk.length(); s += 0.25f) {
            const float h = l.collision.headroom(walk.at(s), 0.25f) > 0.0f ? kCrouchHeight : kStandHeight;
            if (l.collision.overlaps(walk.at(s), 0.25f, open, h)) ++clipped;
        }
        CHECK_MSG(clipped == 0, std::format("40x40 seed {}: walk clips geometry at {} samples", seed, clipped));
        CHECK(l.missingModels.empty() && int(l.doors.size()) == maze.countDoors());
    }
}

// Round 6-7: markers, derelict dressing, crouching under low pipes, effects, firing.
void testDerelictAndGun() {
    auto markers = [&](const char* model, const char* prefix) {
        int n = 0;
        int i = g_kit.find(model);
        if (i < 0) return -1;
        for (const KitMarker& m : g_kit.model(i).markers) n += m.name.starts_with(prefix);
        return n;
    };
    CHECK(markers("handgun", "muzzle") == 1);
    CHECK(markers("corridor_straight_broken", "spark") == 4);
    CHECK(markers("pipe_burst", "steam") == 1);
    for (const char* name : {"arms", "muzzle_flash", "bolt", "spark", "scorch", "pipes_low", "debris", "beacon"})
        CHECK_MSG(g_kit.find(name) >= 0, std::string("kit model missing: ") + name);
    // the muzzle is in front of the grip, forward is -Z
    const KitModel& gun = g_kit.model(g_kit.find("handgun"));
    if (!gun.markers.empty()) {
        CHECK(gun.markers[0].position.z < -0.12f && gun.markers[0].direction.z < -0.9f);
    }

    MazeParams p;
    p.width = p.height = 40;
    p.seed = 1234;
    Game game(g_kit, p);
    const Level& level = game.level();
    int sparks = 0, steam = 0;
    for (const Emitter& e : level.emitters) (e.kind == Emitter::Spark ? sparks : steam)++;
    std::printf("  40x40 seed 1234: %d broken ceilings, %d steam pipes, %d debris piles, %d beacons, %zu low pipes, "
                "%d spark and %d steam emitters\n",
                level.brokenCeilings, level.steamPipes, level.debrisPiles, level.beacons, level.lowPipes.size(), sparks,
                steam);
    CHECK(level.brokenCeilings > 5 && level.steamPipes > 3 && level.debrisPiles > 5 && level.beacons > 3);
    CHECK(level.lowPipes.size() >= 2 && sparks > 10 && steam > 3);

    // a low pipe blocks a standing body and lets a crouched one through
    const LowPipe& lp = level.lowPipes.front();
    const std::vector<float> open(level.doors.size(), 1.0f);
    const Vec2 under{lp.center.x, lp.center.z};
    CHECK(level.collision.overlaps(under, 0.3f, open, kStandHeight));
    CHECK(!level.collision.overlaps(under, 0.3f, open, kCrouchHeight));

    // the scripted walk crouches under a pipe on the route, and slows down there
    float maxCrouch = 0.0f, minEye = 9.0f;
    for (float t = 0.0f; t < game.walkDuration(); t += 0.05f) {
        game.setWalkTime(t);
        maxCrouch = std::max(maxCrouch, game.crouch());
        minEye = std::min(minEye, game.camera().position.y);
    }
    std::printf("  walk: %.1f s (%.1f s at full speed), deepest crouch %.2f, lowest eye %.2f m\n", game.walkDuration(),
                Game::kStartPause + game.walk().length() / Game::kWalkSpeed + Game::kEndPause, maxCrouch, minEye);
    CHECK(maxCrouch > 0.95f && minEye < 1.15f);
    CHECK(game.walkDuration() > Game::kStartPause + game.walk().length() / Game::kWalkSpeed + Game::kEndPause + 1.0f);

    // window mode: walking at a pipe standing up stops in front of it; crouching gets under
    {
        Game g(g_kit, p);
        auto approach = [&](bool crouch) {
            Camera c;
            Vec3 start = lp.center - lp.along * 2.0f;
            c.position = {start.x, kEyeHeight, start.z};
            c.yaw = std::atan2(-lp.along.x, -lp.along.z);
            g.setCamera(c);
            InputState in;
            in.forward = true;
            in.crouch = crouch;
            for (int i = 0; i < 120; ++i) g.update(1.0f / 60.0f, in);
            Vec3 d = g.camera().position - lp.center;
            return d.x * lp.along.x + d.z * lp.along.z;  // how far past the pipe's centre line
        };
        const float standing = approach(false), crouched = approach(true);
        std::printf("  window: standing stops %.2f m before the pipe; crouching gets %.2f m past it\n", -standing,
                    crouched);
        CHECK(standing < -0.2f);
        CHECK(crouched > 0.5f);
    }

    // effects: somewhere along the walk there are spark bursts and steam
    int burstFrames = 0, steamFrames = 0, lightFrames = 0;
    for (float t = 0.0f; t < game.walkDuration(); t += 0.25f) {
        game.setWalkTime(t);
        burstFrames += game.sparksVisible() > 0;
        steamFrames += !game.steamPlumes().empty();
        lightFrames += !game.dynamicLights().empty();
    }
    std::printf("  walk effects (every 0.25 s): sparks in %d, steam in %d, short-lived lights in %d samples\n",
                burstFrames, steamFrames, lightFrames);
    CHECK(burstFrames > 5 && steamFrames > 5 && lightFrames > 5);

    // firing: a bolt flies, hits, leaves a scorch; the gun has a cooldown
    {
        Game g(g_kit, p);
        Camera c;
        for (const Shot& s : g.findShots())
            if (s.name == "corridor_dark") c = s.camera;
        g.setCamera(c);
        g.setTime(10.0f);
        CHECK(g.fire());
        CHECK(!g.fire());  // too soon
        std::vector<SceneInstance> inst;
        g.setTime(10.01f);
        g.buildInstances(inst);
        int fx = 0, gunInst = 0;
        for (const SceneInstance& si : inst) {
            fx += (si.flags & kInstanceFx) != 0;
            gunInst += (si.flags & kInstanceGun) != 0;
        }
        const size_t gunParts = g_kit.model(g_kit.find("handgun")).parts.size() + g_kit.model(g_kit.find("arms")).parts.size();
        CHECK(fx >= 1);                         // the bolt
        CHECK(size_t(gunInst) > gunParts);      // plus the muzzle flash
        CHECK(!g.dynamicLights().empty());      // muzzle light
        g.setTime(12.0f);
        CHECK(g.scorchCount() == 1);
        CHECK(g.fire());
        CHECK(g.shotsFired() == 2);
        // the camera ray straight down hits the floor at eye height
        Vec3 n;
        float d = raycastLevel(g.maze(), g.level(), c.position, {0, -1, 0}, 10.0f, std::vector<float>(level.doors.size(), 0.0f), n);
        CHECK(std::abs(d - c.position.y) < 0.05f && n.y > 0.9f);
    }
}

// Round 8: the creature. Its parts load, the crawl scene finds a corridor, the body stays
// on the surface (floor, wall, ceiling), planted claws do not slide, and the IK keeps
// every leg its own length.
void testCreature() {
    const KitModel& cm = g_kit.model(g_kit.find("creature"));
    int bones = 0, feet = 0;
    for (const KitMarker& mk : cm.markers) {
        bones += mk.name.starts_with("bone_");
        feet += mk.name.starts_with("foot_");
    }
    CHECK(bones == 27 && feet == 4);
    Creature probe(g_kit);
    CHECK(probe.valid());

    // the cross-section loop: floor at u = 0, the right wall, the ceiling half way round
    CrawlProfile pr;
    CHECK(std::abs(pr.point(0).y) < 1e-4f && pr.normal(0).y > 0.99f);
    CHECK(pr.point(pr.rightWallU()).x > 1.19f && pr.normal(pr.rightWallU()).x < -0.99f);
    CHECK(std::abs(pr.point(pr.ceilingU()).y - 2.85f) < 1e-3f && pr.normal(pr.ceilingU()).y < -0.99f);

    std::string moods;
    for (uint64_t seed : {1234u, 7u, 42u, 2026u, 5u, 11u, 99u, 314u}) {
        MazeParams p;
        p.width = p.height = 40;
        p.seed = seed;
        Game game(g_kit, p);
        const bool ok = game.setupCrawlScene();
        CHECK_MSG(ok, std::format("seed {}: no crawl scene", seed));
        if (!ok) continue;
        game.setCrawlTime(0.0f);
        moods += std::format(" {}:{}", seed, toString(game.moodAtCamera()));
        if (seed != 1234u) continue;

        // walk the scene: the body hugs the surface, planted claws stay put, legs keep length
        const CrawlPath& path = game.crawlPath();
        float maxLenErr = 0.0f, maxSlide = 0.0f, maxLift = 0.0f;
        bool sawWall = false, sawCeiling = false;
        std::array<Vec3, 4> prevFoot{};
        std::array<bool, 4> prevPlanted{};
        for (float t = 0.0f; t < game.crawlDuration(); t += 1.0f / 30.0f) {
            game.setCrawlTime(t);
            const Creature& c = game.creature();
            for (int l = 0; l < 4; ++l) {
                const float miss = length(c.posedClaw(l) - c.foot(l));
                if (miss > maxLenErr && miss > 0.06f)
                    std::printf("    miss %.3f: t %.2f leg %s planted %d s %.2f u %.2f\n", miss, t, Creature::legName(l),
                                int(c.planted(l)), game.crawlS(t), game.crawlU(game.crawlS(t)));
                maxLenErr = std::max(maxLenErr, miss);
                if (c.planted(l) && prevPlanted[size_t(l)]) maxSlide = std::max(maxSlide, length(c.foot(l) - prevFoot[size_t(l)]));
                prevFoot[size_t(l)] = c.foot(l);
                prevPlanted[size_t(l)] = c.planted(l);
            }
            const float s = game.crawlS(t), u = game.crawlU(s);
            const Vec3 n = path.normal(s, u);
            sawWall |= std::abs(n.y) < 0.2f;
            sawCeiling |= n.y < -0.95f;
            // the thorax rides 0.3-0.7 m off whatever surface it is on
            const Vec3 surf = path.point(s + 0.38f, u);
            const float lift = dot(c.bodyCenter() - surf, path.normal(s + 0.38f, u));
            maxLift = std::max(maxLift, std::abs(lift - 0.5f));
        }
        std::printf("  crawl (seed 1234): %.1f s; worst IK miss %.3f m, planted-claw slide %.3f m, body height off by "
                    "%.2f m; wall %s, ceiling %s\n",
                    game.crawlDuration(), maxLenErr, maxSlide, maxLift, sawWall ? "yes" : "no", sawCeiling ? "yes" : "no");
        CHECK(sawWall && sawCeiling);
        CHECK_MSG(maxLenErr < 0.06f, std::format("a leg cannot reach its foothold ({:.3f} m)", maxLenErr));
        CHECK_MSG(maxSlide < 0.02f, std::format("a planted claw slides ({:.3f} m)", maxSlide));
        CHECK(maxLift < 0.25f);
    }
    std::printf("  crawl scene corridor by seed:%s\n", moods.c_str());
}

// Rounds 9-10: hunters. They spawn away from you, roam without falling off the world, and
// the two filmed scenes play out as written: the hunt ends with the player dead, the
// fight with the creature dead after exactly four hits.
void testHunters() {
    MazeParams p;
    p.width = p.height = 40;
    p.seed = 1234;
    {
        Game game(g_kit, p);
        game.spawnHunters(3);
        CHECK(game.hunters().size() == 3);
        const std::vector<int> dist = game.maze().distancesFrom(game.maze().startCell());
        for (const Hunter& h : game.hunters()) {
            const Int2 c = h.cell();
            CHECK(game.maze().inBounds(c) && dist[size_t(c.y * 40 + c.x)] >= 10);
        }
        // a minute of the player standing at the spawn: they roam, stay inside the ship
        game.update(1.0f / 30.0f, InputState{});  // poses them
        std::vector<Vec3> start;
        for (const Hunter& h : game.hunters()) start.push_back(h.bodyCenter());
        bool finite = true, inside = true;
        float maxJump = 0.0f;
        std::vector<Vec3> prev = start;
        for (int i = 0; i < 60 * 30; ++i) {
            game.update(1.0f / 30.0f, InputState{});
            for (size_t k = 0; k < game.hunters().size(); ++k) {
                const Hunter& h = game.hunters()[k];
                const Vec3 b = h.bodyCenter();
                const float jump = length(b - prev[k]);
                if (jump > 0.5f && i > 0)
                    std::printf("    jump %.2f m: hunter %zu t %.2f state %s\n", jump, k, i / 30.0f, toString(h.state()));
                if (i > 0) maxJump = std::max(maxJump, jump);
                prev[k] = b;
                finite = finite && std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.z);
                inside = inside && b.y > 0.0f && b.y < 3.0f && game.maze().kind(h.cell()) != CellKind::Void;
            }
        }
        float moved = 0.0f;
        for (size_t i = 0; i < start.size(); ++i) moved = std::max(moved, length(game.hunters()[i].bodyCenter() - start[i]));
        std::printf("  3 hunters, 60 s: furthest moved %.1f m; states %s %s %s\n", moved,
                    toString(game.hunters()[0].state()), toString(game.hunters()[1].state()),
                    toString(game.hunters()[2].state()));
        CHECK(finite && inside);
        CHECK(moved > 5.0f);
        CHECK_MSG(maxJump < 0.4f, std::format("a hunter jumped {:.2f} m in one step", maxJump));
    }
    // Round 11: ten loose, drifting toward the player and hearing footsteps. They all find
    // a place, drift legs happen, they close in on a player standing still (making walking
    // noise), and re-routing at cell centres never moves one more than 0.4 m in a step.
    // Turning round on the path (snapped 0.7 m before round 13) is held to the same limit
    // now that it swings round on the spot. The same ten with the original tuning are run
    // too, to compare how often they turn round.
    int closestWith[2] = {0, 0};  // round 11, original
    for (bool round11 : {true, false}) {
        Game game(g_kit, p);
        HuntTuning tuning = round11 ? HuntTuning::round11() : HuntTuning::original();
        tuning.creatures = HuntTuning::round11().creatures;
        game.setHuntTuning(tuning);
        game.spawnHunters(tuning.creatures);
        CHECK(int(game.hunters().size()) == tuning.creatures);
        game.setPlayerNoise(1);
        const Int2 pc = game.cameraCell();
        const std::vector<int> dist = game.maze().distancesFrom(pc);
        auto nearest = [&] {
            int best = 1 << 30;
            for (const Hunter& h : game.hunters())
                if (game.maze().inBounds(h.cell())) best = std::min(best, dist[size_t(h.cell().y * 40 + h.cell().x)]);
            return best;
        };
        game.stepWorld(1.0f / 30.0f);  // poses them
        const int before = nearest();
        std::vector<Vec3> prev;
        std::vector<int> prevTurns;
        for (const Hunter& h : game.hunters()) {
            prev.push_back(h.bodyCenter());
            prevTurns.push_back(h.uTurns());
        }
        float maxJump = 0.0f, maxTurnJump = 0.0f;
        int closest = before;
        float t = 0.0f;
        for (; t < 150.0f && !game.playerDead(); t += 1.0f / 30.0f) {
            game.stepWorld(1.0f / 30.0f);
            for (size_t k = 0; k < game.hunters().size(); ++k) {
                const Hunter& h = game.hunters()[k];
                const float jump = length(h.bodyCenter() - prev[k]);
                const bool turned = h.uTurns() != prevTurns[k];
                prev[k] = h.bodyCenter();
                prevTurns[k] = h.uTurns();
                if (jump > 0.4f)
                    std::printf("    jump %.2f m: hunter %zu t %.2f state %s (%.2f s in it), last path change: %s%s\n",
                                jump, k, t, toString(h.state()), h.stateTime(), h.lastRoute(),
                                turned ? " (turning round)" : "");
                // (feeding: the game poses it on the player's face, it is not crawling)
                if (h.state() == HunterState::Feed) continue;
                if (turned) maxTurnJump = std::max(maxTurnJump, jump);
                else maxJump = std::max(maxJump, jump);
            }
            closest = std::min(closest, nearest());
        }
        int drifts = 0, turns = 0;
        for (const Hunter& h : game.hunters()) {
            drifts += h.drifts();
            turns += h.uTurns();
        }
        std::printf("  %s tuning, 10 hunters: nearest %d cells away at the start, %d at the closest; %d drift legs; "
                    "player %s after %.0f s; biggest step %.2f m; %d turned round (biggest step then %.2f m)\n",
                    round11 ? "round 11" : "original", before, closest, drifts,
                    game.playerDead() ? "killed" : "alive", t, maxJump, turns, maxTurnJump);
        if (round11) CHECK(drifts > 0);
        closestWith[round11 ? 0 : 1] = closest;
        CHECK_MSG(maxJump < 0.4f, std::format("a hunter jumped {:.2f} m in one step", maxJump));
        CHECK_MSG(maxTurnJump < 0.4f, std::format("a hunter jumped {:.2f} m turning round", maxTurnJump));
    }
    // drifting toward you brings them closer than wandering did
    CHECK(closestWith[0] < closestWith[1]);
    // Round 13: the approach, you standing still. It closes in over the walls and ceiling,
    // threatens (stopped, reared, jaws wide), coils and leaps, and has you. Its claws stay
    // put while it threatens and coils, it never moves more than 0.4 m in a step before the
    // leap, and some of the approach is off the floor. With the original tuning: no threat.
    for (bool current : {true, false}) {
        Game game(g_kit, p);
        game.setHuntTuning(current ? HuntTuning::round11() : HuntTuning::original());
        CHECK(game.setupHuntScene(false, true));
        std::string seq;
        HunterState last = HunterState::Dead;
        const float dt = 1.0f / 30.0f;
        float t = 0.0f, maxJump = 0.0f, maxSlide = 0.0f, threatAt = -1.0f, offFloor = 0.0f, chasing = 0.0f;
        int leapSurface = -1;
        Vec3 prev = game.hunters()[0].bodyCenter();
        std::array<Vec3, 4> feet{};
        std::array<bool, 4> planted{};
        for (; t < game.huntSceneLength() && !game.huntSceneDone(t); t += dt) {
            const int surfaceBefore = game.hunters()[0].surfaceKind();
            game.stepHuntScene(t, dt);
            const Hunter& h = game.hunters()[0];
            const HunterState s = h.state();
            if (s != last) {
                seq += std::format(" {}@{:.1f}", toString(s), t);
                if (s == HunterState::Threat) threatAt = length(h.headPosition() - game.camera().position);
                if (s == HunterState::Leap) leapSurface = surfaceBefore;
                last = s;
            }
            if (s == HunterState::Chase || s == HunterState::Stalk) {
                chasing += dt;
                if (h.surfaceKind() != 0) offFloor += dt;
            }
            const bool still = s == HunterState::Threat || s == HunterState::Coil;
            for (int l = 0; l < 4; ++l) {
                const Vec3 f = h.creature().foot(l);
                if (still && planted[size_t(l)] && h.creature().planted(l))
                    maxSlide = std::max(maxSlide, length(f - feet[size_t(l)]));
                feet[size_t(l)] = f;
                planted[size_t(l)] = h.creature().planted(l);
            }
            // (from the second step: before the first it has never been posed)
            if (t > 0.0f && s != HunterState::Leap && s != HunterState::Feed) {
                const float jump = length(h.bodyCenter() - prev);
                if (jump > 0.4f)
                    std::printf("    jump %.2f m at %.2f s, %s (%.2f s in it), last path change: %s, %s, %d turns; "
                                "from (%.2f %.2f %.2f) to (%.2f %.2f %.2f), surface %d\n",
                                jump, t, toString(s), h.stateTime(), h.lastRoute(), h.turning() ? "turning" : "not turning",
                                h.uTurns(), prev.x, prev.y, prev.z, h.bodyCenter().x, h.bodyCenter().y, h.bodyCenter().z,
                                h.surfaceKind());
                maxJump = std::max(maxJump, jump);
            }
            prev = h.bodyCenter();
        }
        std::printf("  approach (%s tuning): %.1f s,%s; threat begun at %.1f m; leapt from surface %d; %.0f%% of the "
                    "closing in off the floor; biggest step %.2f m, planted claws slid %.3f m; player %s\n",
                    current ? "current" : "original", t, seq.c_str(), threatAt, leapSurface,
                    100.0f * offFloor / std::max(dt, chasing), maxJump, maxSlide,
                    game.playerDead() ? "dead" : "alive");
        CHECK(game.playerDead());
        CHECK(maxJump < 0.4f);
        if (current) {
            const size_t th = seq.find("threat"), co = seq.find("coil"), le = seq.find("leap"), fe = seq.find("feed");
            CHECK_MSG(th != std::string::npos && co > th && le > co && fe > le && co != std::string::npos &&
                          le != std::string::npos && fe != std::string::npos,
                      "approach: threat, coil, leap, feed in that order");
            CHECK(threatAt > 2.2f && threatAt < Hunter::kThreatRange + 0.5f);
            CHECK(maxSlide < 0.02f);
            CHECK(offFloor > 0.5f * chasing);
        } else {
            CHECK(seq.find("threat") == std::string::npos);
        }
    }
    for (bool fight : {false, true}) {
        Game game(g_kit, p);
        game.setHuntTuning(HuntTuning::round11());
        CHECK(game.setupHuntScene(fight));
        std::string seq;
        HunterState last = HunterState::Dead;
        float t = 0.0f;
        const float dt = 1.0f / 30.0f;
        for (; t < game.huntSceneLength() && !game.huntSceneDone(t); t += dt) {
            game.stepHuntScene(t, dt);
            const HunterState s = game.hunters()[0].state();
            if (s != last) {
                seq += std::format(" {}@{:.1f}", toString(s), t);
                last = s;
            }
        }
        const Hunter& h = game.hunters()[0];
        std::printf("  %s scene: %.1f s,%s; hp %d, shots %d, player %s\n", fight ? "fight" : "hunt", t, seq.c_str(),
                    h.health(), game.shotsFired(), game.playerDead() ? "dead" : "alive");
        if (fight) {
            CHECK_MSG(!h.alive() && !game.playerDead(), "fight: the creature should die, the player live");
            CHECK(h.health() == 0 && game.kills() == 1);
        } else {
            CHECK_MSG(game.playerDead(), "hunt: the player should die");
            CHECK(seq.find("stalk") != std::string::npos && seq.find("chase") != std::string::npos &&
                  seq.find("leap") != std::string::npos && seq.find("feed") != std::string::npos);
            CHECK(h.health() < Hunter::kHealth && h.alive());  // hit, and still came on
            CHECK(game.redout() > 0.9f);
        }
    }
    // three hits do not kill; the fourth does
    {
        Game game(g_kit, p);
        game.spawnHunters(1);
        Hunter h = game.hunters()[0];
        HuntWorld w;
        w.maze = &game.maze();
        w.level = &game.level();
        std::vector<float> doors(game.level().doors.size(), 0.0f);
        w.doorOpen = &doors;
        w.player = game.camera().position;
        for (int i = 0; i < 3; ++i) h.hit(w);
        CHECK(h.alive() && h.health() == 1);
        h.hit(w);
        CHECK(!h.alive());
    }
}

// The window's walk: forward held, no mouse, down a straight corridor, at several frame
// rates. The camera may sway 1.2 cm either side of the line it walks, never more, and
// the walk must not depend on the frame rate. (The sway once fed back into the position
// each step: a 0.35 m weave at 100 fps, 0.84 m at 240, and a slide into the wall while
// pressed against the low pipe at the end of this corridor.)
void testWalkStraight() {
    MazeParams p;
    p.width = p.height = 40;
    p.seed = 1234;
    Camera start;
    {
        Game g(g_kit, p);
        for (const Shot& s : g.findShots())
            if (s.name == "corridor") start = s.camera;
    }
    start.pitch = -0.03f;
    const Vec3 fwd = normalize(Vec3{start.forward().x, 0, start.forward().z});
    const Vec3 right{-fwd.z, 0, fwd.x};
    float along2[5] = {};
    int k = 0;
    for (int hz : {30, 60, 100, 144, 240}) {
        Game g(g_kit, p);
        g.setCamera(start);
        InputState in;
        in.forward = true;
        const float dt = 1.0f / float(hz);
        float maxSide = 0.0f, endAlong = 0.0f, freeMin = 0.0f, freeMax = 0.0f;
        for (int i = 1; i <= hz * 6; ++i) {  // 6 s: ~15 m to the low pipe, then pressing on it
            g.update(dt, in);
            const Vec3 rel = g.camera().position - start.position;
            const float side = dot(rel, right);
            maxSide = std::max(maxSide, std::abs(side));
            if (i <= hz * 4) {  // walking freely, before the pipe stops it
                freeMin = std::min(freeMin, side);
                freeMax = std::max(freeMax, side);
            }
            if (i == hz * 2) along2[k] = dot(rel, fwd);
            endAlong = dot(rel, fwd);
        }
        std::printf("  %3d Hz: walking freely %+.3f .. %+.3f m off the line (swing %.3f); overall furthest %.3f m; "
                    "%.2f m along at 2 s, %.2f m at 6 s\n",
                    hz, freeMin, freeMax, freeMax - freeMin, maxSide, along2[k], endAlong);
        CHECK_MSG(maxSide < 0.025f, std::format("{} Hz: the walk weaves {:.3f} m off its line", hz, maxSide));
        ++k;
    }
    for (int i = 1; i < 5; ++i)
        CHECK_MSG(std::abs(along2[i] - along2[0]) < 0.08f,
                  std::format("distance walked in 2 s depends on frame rate ({:.2f} vs {:.2f} m)", along2[i], along2[0]));
}

// Part 3: viewports. On the ship's edge, the glass beyond the wall on the outward side,
// a transparent glass part, and nothing else in the ship moved (every model and emitter
// outside the viewport cells, including the dressing stream's spark and steam phases, is
// the same as without them). The window scene finds one and reaches its beats.
void testViewports() {
    MazeParams p;
    p.width = p.height = 40;
    p.seed = 1234;
    const Maze maze = Maze::generate(p);
    const Level with = buildLevel(maze, g_kit), without = buildLevel(maze, g_kit, false);
    const int win = g_kit.find("corridor_straight_window");
    CHECK(win >= 0);
    bool glass = false;
    for (uint32_t part : g_kit.model(win).parts) glass = glass || g_kit.parts()[part].transparent;
    CHECK(glass);
    std::set<int> cells;
    for (const Level::Viewport& v : with.viewports) cells.insert(v.cell.y * 40 + v.cell.x);
    int placed = 0, outward = 0, onEdge = 0;
    for (const PlacedModel& pm : with.placed) {
        if (pm.model != win) continue;
        ++placed;
        const Int2 c{pm.cell % 40, pm.cell / 40};
        for (const Level::Viewport& v : with.viewports) {
            if (!(v.cell == c)) continue;
            const Vec3 g = pm.transform.point({1.5f, 1.4f, 0.0f});  // on the glass (model space)
            if (dot(g - cellCenter(c), dirVector(v.out)) > 1.4f) ++outward;
            const Int2 beyond{c.x + kDx[int(v.out)], c.y + kDy[int(v.out)]};
            if (!maze.inBounds(beyond)) ++onEdge;
        }
    }
    std::printf("  seed 1234: %zu viewports (%d placed, %d facing out, %d on the edge); %d broken ceilings, %d steam "
                "pipes, %d debris, %d beacons (without viewports: %d, %d, %d, %d)\n",
                with.viewports.size(), placed, outward, onEdge, with.brokenCeilings, with.steamPipes, with.debrisPiles,
                with.beacons, without.brokenCeilings, without.steamPipes, without.debrisPiles, without.beacons);
    CHECK(with.viewports.size() >= 4 && placed == int(with.viewports.size()));
    CHECK(outward == placed && onEdge == placed);
    CHECK(without.viewports.empty());
    // everything outside the viewport cells is placed exactly as before
    auto same = [](const Mat4& a, const Mat4& b) {
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k)
                if (std::abs(a.m[r][k] - b.m[r][k]) > 1e-6f) return false;
        return true;
    };
    std::vector<const PlacedModel*> a, b;
    for (const PlacedModel& pm : with.placed)
        if (!cells.count(pm.cell)) a.push_back(&pm);
    for (const PlacedModel& pm : without.placed)
        if (!cells.count(pm.cell)) b.push_back(&pm);
    bool identical = a.size() == b.size();
    for (size_t i = 0; identical && i < a.size(); ++i)
        identical = a[i]->model == b[i]->model && same(a[i]->transform, b[i]->transform);
    CHECK_MSG(identical, std::format("models outside the viewport cells moved ({} vs {})", a.size(), b.size()));
    std::vector<const Emitter*> ea, eb;
    for (const Emitter& e : with.emitters)
        if (!cells.count(e.cell)) ea.push_back(&e);
    for (const Emitter& e : without.emitters)
        if (!cells.count(e.cell)) eb.push_back(&e);
    bool sameFx = ea.size() == eb.size();
    for (size_t i = 0; sameFx && i < ea.size(); ++i)
        sameFx = length(ea[i]->position - eb[i]->position) < 1e-5f && ea[i]->phase == eb[i]->phase;
    CHECK_MSG(sameFx, std::format("spark/steam emitters outside the viewport cells changed ({} vs {})", ea.size(),
                                  eb.size()));
    // the window scene
    Game game(g_kit, p);
    CHECK(game.setupWindowScene());
    float closest = 1e9f;
    bool creatureSeen = false;
    for (float t = 0.0f; t < game.windowSceneLength(); t += 1.0f / 30.0f) {
        game.setWindowTime(t);
        if (game.creatureActive()) {
            creatureSeen = true;
            closest = std::min(closest, length(game.creature().headPosition() - game.camera().position));
        }
    }
    std::printf("  window scene: %.1f s, in a %s section; the creature comes within %.1f m\n", game.windowSceneLength(),
                std::string(toString(game.moodAtCamera())).c_str(), closest);
    CHECK(creatureSeen && closest > 0.8f && closest < 4.0f);
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

// The motion scanner (round 17): moving things inside its range leave a blip near them
// (never on them), still things and far things none; the pulse comes round faster and the
// ping rises as something closes in. Then the scanner scene: blips from more than one way,
// and one comes into view ahead.
void testScanner() {
    {
        Scanner sc;
        std::vector<Scanner::Contact> contacts = {{{10.0f, 0, 0}, 1.0f}, {{0, 0, -8.0f}, 0.0f}, {{30.0f, 0, 0}, 1.5f}};
        int blips = 0;
        float maxOff = 0.0f, sumOff = 0.0f, nearStill = 0.0f, nearFar = 0.0f;
        std::vector<float> pulseTimes;
        std::vector<Scanner::Pulse> pulses;
        for (int i = 0; i <= 30 * 8; ++i) {
            const float t = float(i) / 30.0f;
            contacts[0].position.z = 0.5f * t;  // (it moves; slowly enough to stay ~10 m out)
            sc.update(t, {0, 1.7f, 0}, contacts);
            for (const Scanner::Pulse& p : sc.takePulses()) pulses.push_back(p);
            for (const ScanBlip& b : sc.blips()) {
                const float dA = std::hypot(b.x - contacts[0].position.x, b.z - contacts[0].position.z);
                if (b.alpha > 0.5f) ++blips, sumOff += dA, maxOff = std::max(maxOff, dA);
                if (std::hypot(b.x, b.z + 8.0f) < 2.0f) nearStill = 1.0f;
                if (std::hypot(b.x - 30.0f, b.z) < 6.0f) nearFar = 1.0f;
            }
        }
        std::printf("  scanner: %zu pulses in 8 s, %d bright blip-frames, offset from the contact up to %.2f m (mean %.2f)\n",
                    pulses.size(), blips, maxOff, blips ? sumOff / float(blips) : 0.0f);
        for (const Scanner::Pulse& p : pulses)
            std::printf("    pulse at %.2f s: nearest %.1f m, %d found, ping pitch %.2f gain %.2f\n", p.time, p.nearest,
                        p.contacts, Scanner::pingPitch(p), Scanner::pingGain(p));
        CHECK_MSG(blips > 0, "a moving contact at 10 m left no blip");
        CHECK_MSG(maxOff < 0.8f + 0.07f * 11.0f + 0.6f, std::format("a blip {:.2f} m from what it found", maxOff));
        CHECK_MSG(blips == 0 || sumOff / float(blips) > 0.15f, "blips sit right on the contact");
        CHECK_MSG(nearStill == 0.0f, "something standing still showed");
        CHECK_MSG(nearFar == 0.0f, "something beyond its range showed");
        CHECK(pulses.size() >= 4);
        if (pulses.size() >= 4) {
            CHECK_MSG(pulses[0].nearest < 0.0f && Scanner::pingPitch(pulses[0]) == 1.0f, "the first ping is not the empty one");
            const Scanner::Pulse& late = pulses.back();
            CHECK_MSG(late.nearest > 8.0f && late.nearest < 13.0f, std::format("nearest {:.1f} m", late.nearest));
            CHECK(Scanner::pingPitch(late) > 1.0f && Scanner::pingGain(late) > Scanner::pingGain(pulses[0]));
            const float gapLate = pulses.back().time - pulses[pulses.size() - 2].time;
            CHECK_MSG(gapLate < Scanner::kPeriodIdle - 0.3f, std::format("pulses {:.2f} s apart with a contact at 10 m", gapLate));
        }
        // nothing about: idle pulses
        Scanner idle;
        std::vector<Scanner::Pulse> ip;
        for (int i = 0; i <= 30 * 10; ++i) {
            idle.update(float(i) / 30.0f, {}, {});
            for (const Scanner::Pulse& p : idle.takePulses()) ip.push_back(p);
        }
        CHECK(ip.size() >= 4 && idle.blips().empty());
        if (ip.size() >= 2) CHECK(std::abs(ip[1].time - ip[0].time - Scanner::kPeriodIdle) < 0.05f);
    }
    // the scene
    MazeParams mp;
    mp.width = mp.height = 40;
    mp.seed = 1234;
    Game game(g_kit, mp);
    game.setHuntTuning(HuntTuning::round11());
    CHECK_MSG(game.setupScannerScene(), "no junction for the scanner scene");
    if (game.hunters().empty()) return;
    const float dt = 1.0f / 30.0f;
    std::set<int> ways;  // which quarter (round the player) blips have shown in
    float t = 0.0f;
    for (; t < game.scannerSceneLength() && !game.scannerSceneDone(t); t += dt) {
        game.stepScannerScene(t, dt);
        const Vec3 p = game.camera().position;
        for (const ScanBlip& b : game.scanner().blips()) {
            const float a = std::atan2(b.z - p.z, b.x - p.x);
            ways.insert(int(std::floor((a + kPi) / (0.5f * kPi) + 0.5f)) % 4);
        }
    }
    std::printf("  scanner scene (seed 1234): %.1f s, one in view at %.1f s, blips from %zu of 4 directions, %d pulses, "
                "player %s\n",
                t, game.scannerSeenAt(), ways.size(), game.scanner().pulses(), game.playerDead() ? "dead" : "alive");
    CHECK_MSG(game.scannerSeenAt() > 5.0f && game.scannerSeenAt() < 20.0f,
              std::format("one came into view at {:.1f} s", game.scannerSeenAt()));
    CHECK_MSG(ways.size() >= 2, "blips from only one direction");
    CHECK(!game.playerDead());
}

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
                          {"sections, lights, map, flashlight (40x40)", testMood},
                          {"derelict pieces, crouching, effects, firing (40x40)", testDerelictAndGun},
                          {"creature: parts, crawl surface, gait, IK", testCreature},
                          {"hunters: roaming, the hunt, the fight, four hits", testHunters},
                          {"walking straight: no weave at any frame rate", testWalkStraight},
                          {"viewports: on the edge, facing out, nothing else moved; the window scene", testViewports},
                          {"the motion scanner: blips, pulses, pings; the scanner scene", testScanner}};
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
