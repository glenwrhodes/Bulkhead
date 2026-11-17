# Bulkhead: notes for agents

A C++20 / DirectX 12 / DXR first-person shooter in a generated sci-fi ship maze. The plan and
milestone status are in `docs/PLAN.md`; update that table when a milestone lands.

## How work is verified on this machine

- Bash can only run `cmake` and `ctest`. Build and test with:
  - `cmake -S . -B build -G "Visual Studio 17 2022" -A x64`
  - `cmake --build build --config Release`
  - `ctest --test-dir build -C Release --output-on-failure` (labels `unit`, `smoke`, `film`)
- Nobody watches the screen. Each feature must be visible in a headless capture
  (`Bulkhead.exe --capture <dir>`), run as a ctest test. Open the resulting PNGs with the Read
  tool and look at them before calling a build good.
- The game is filmed for a YouTube video. Keep Blender `.blend` sources in `art/blender/` so
  they can be filmed, and add `film`-labelled capture tests for new features.
- The user launches the interactive window themselves; do not launch it.
- The orchestrator commits after each round; git is not available to agents here.

## Conventions

- Code compiles with `/W4 /WX`. Keep it warning-free.
- `src/world` and `src/core/Image|Log` must stay free of D3D12 (they are linked into unit tests).
- Use `bh::Rng` (PCG32) for anything seeded. Never use `<random>` distributions: a seed must
  give the same ship everywhere. `maze_tests` prints `rng(1234).next()`; if that changes, every
  layout changes.
- Grid: 4 m cells, x east, y south (row). The maze stores east and south edges for each cell.
- Shaders: add an entry with `bulkhead_compile_shader()` in `CMakeLists.txt`. It produces
  `g_<Name>.h`, which is included directly.
- Third-party code comes in through FetchContent, pinned to a commit (stb at present; cgltf comes next).
