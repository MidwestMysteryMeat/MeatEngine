# MeatEngine

A small, readable, single-player **voxel FPS engine** in modern C++20 — tight raw-mouse
FPS feel, chunked voxel worlds you can build by hand in an in-engine **Room Designer** or
generate as seeded **procedural dungeons**, with a deliberately **PSX-forward** renderer
(half-res, nearest filtering, dither, fog — all toggleable).

**Status: early scaffold.** See [ROADMAP.md](ROADMAP.md) for the build order and
[ARCHITECTURE.md](ARCHITECTURE.md) for how it fits together.

## Planned feature set (MVP)

- GLFW + OpenGL 4.5 core, fixed 60 Hz simulation, interpolated rendering
- Voxel chunks (32³, 0.5 m) with worker-thread meshing and streaming
- Jolt physics: capsule character controller tuned for FPS, chunk colliders, hitscan
- Forward Blinn-Phong (directional + point/spot) with a PSX post pipeline
- FBX / OBJ / GLB models via Assimp; PNG/JPG textures; skeletal animation on a
  canonical Mixamo-named skeleton
- Slot inventory, pickups, equip/use; full save/load (JSON meta + RLE chunk deltas)
- Room Designer editor mode (F1): grid-snapped modular building, lights, outliner, gizmos
- Seeded procedural dungeons (rooms, corridors, loops, verticality) mixable with
  hand-authored spaces
- Lua gameplay scripting (sol2)
- Auto-rigging tool (Pinocchio) for unrigged humanoid meshes → shared animation set

## Building (Windows)

Requires Visual Studio 2022 (Desktop C++), CMake 3.28+, Python 3 (for glad generation).

```powershell
./scripts/build.ps1            # configure + build (Ninja, Release)
./build/MeatEngine.exe         # run
```

First build fetches and compiles all third-party dependencies (Assimp and Jolt dominate;
expect 10–20 minutes once). Linux support is intended; CI pending.

## Controls

| Input | Action |
|---|---|
| WASD / mouse | Move / look |
| Space / LShift / LCtrl | Jump / sprint / crouch |
| LMB | Fire |
| E | Use / pick up |
| Tab | Inventory |
| F1 | Editor mode |
| F5 / F9 | Save / load |
| F6 | Shader hot-reload |

## License

Code: [MIT](LICENSE). Third-party libraries under their own licenses ([THIRD_PARTY.md](THIRD_PARTY.md)).
Bundled art/audio is CC-BY/CC0 with per-asset credits in [assets/ATTRIBUTION.md](assets/ATTRIBUTION.md).
