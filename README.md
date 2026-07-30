# MeatEngine

A small, readable, single-player **voxel FPS engine** in modern C++20 — tight raw-mouse
FPS feel, chunked voxel worlds you can build by hand in an in-engine **Room Designer** or
generate as seeded **procedural dungeons**, with a deliberately **PSX-forward** renderer
(half-res, nearest filtering, dither, fog — all toggleable).

**Status: playable vertical slice.** The core loop, netcode, combat, inventory, save/load,
editor, dungeons, scripting and packaging are implemented; see [ROADMAP.md](ROADMAP.md) for
what's done vs. pending and [ARCHITECTURE.md](ARCHITECTURE.md) for how it fits together.
Honest caveats: skeletal **bodies** animate on any mixamorig character but **hand fidelity
on variant rigs** and full locomotion/viewmodels are still landing; Linux build + CI, torch
light propagation, and human feel-playtests are pending.

## Feature set

Implemented:

- GLFW + OpenGL 4.5 core, fixed 60 Hz simulation, interpolated rendering
- Voxel chunks (32³; **dev-configurable voxel size**, 0.5 m default) with worker-thread
  meshing, greedy mesher, DDA raycast and streaming
- Jolt physics: capsule character controller tuned for FPS, chunk colliders, hitscan
- Forward Blinn-Phong (directional + point/spot) with a toggleable PSX post pipeline
- Server-authoritative netcode: loopback single-player + ENet host/join, client prediction
  with rewind-reconciliation, 20 Hz snapshots; LAN discovery + master-list session browsing
- Combat: hitscan + projectiles + AoE, material penetration, weapon archetypes (pistol/SMG/
  shotgun/sniper/RPG/grenade/claymore), deployable **turrets** and mobile **companions**
- FBX / OBJ / GLB models via Assimp; PNG/JPG textures; skeletal animation (Mixamo-named
  skeleton) with clip-merge and a VLM-gated render booth
- Slot inventory (multiple GameRules models), pickups, equip/use; full save/load (JSON meta
  + chunk-delta overlay), autosave
- Room Designer editor (F1) + in-engine IDE panels (asset browser, Lua hot-reload)
- Seeded procedural dungeons (rooms, corridors, loops, verticality), mixable with
  hand-authored spaces
- Lua gameplay scripting (sol2), sandboxed with an instruction budget
- Packaging SDK: `--project`, `new_project.py`, `package.ps1` → shippable zip

Pending: Linux build + CI, torch light propagation, animation blend graph + retargeting for
variant rigs, delta-compressed snapshots + lag compensation (PvP), human feel-playtests.

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
| LMB / RMB | Fire or mine / place block |
| 1-9 / scroll | Hotbar select |
| E | Use held item (medkit) |
| Tab | Backpack |
| F1 | Room Designer editor (host/single-player) |
| F5 | Quicksave (auto-saves on exit; `--load saves/autosave.json` to resume) |
| F6 | Shader hot-reload |
| F12 | Screenshot → `build/shot.png` |

Multiplayer: `--host` to serve, `--join <ip>` to connect, `--server` for headless dedicated.

## Making a game

A game is a project folder — config, Lua, and assets — no C++ required:

```powershell
python tools/new_project.py MyGame --dir F:\Games    # scaffold a runnable game
./build/MeatEngine.exe --project F:\Games\MyGame --play   # run it
powershell tools/package.ps1 -Project F:\Games\MyGame     # → dist/MyGame + .zip to ship
```

`game.json` sets the name, seed, inventory model, and economy/ballistics rules;
`scripts/*.lua` is server-authoritative gameplay (see `assets/scripts/example.lua`
for the `game` API). The packaged folder runs standalone via its `Play.bat`.

## License

Licensed under the **[Apache License 2.0](LICENSE)** — free to use, modify, fork and build on, commercially or not.

**Credit is required.** Apache-2.0 §4(c)–(d) obliges you to keep the copyright notice and to reproduce [`NOTICE`](NOTICE) in anything you distribute, including binaries and hosted builds. Credit it as `MeatEngine by MysteryMeat` (https://github.com/MidwestMysteryMeat/MeatEngine) in your credits screen, About box, or docs. The project name and the MysteryMeat name are not licensed for endorsement or promotion (§6).

Third-party libraries stay under their own licenses ([THIRD_PARTY.md](THIRD_PARTY.md)).
Bundled art/audio is CC-BY/CC0 with per-asset credits in [assets/ATTRIBUTION.md](assets/ATTRIBUTION.md) — those credits are required separately from this project's.

Previously MIT; relicensed to Apache-2.0 on 2026-07-30. Snapshots released under MIT stay MIT.
