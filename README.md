# MeatEngine

A **PSX-styled, host-authoritative voxel FPS engine** in modern C++20 — chunked voxel
worlds with a deliberately retro renderer (low internal resolution, ordered dither,
vertex jitter, affine texture mapping), a Quake/Source-lineage networking model, and an
in-engine Room Designer, all in one executable with no scripting language required to
ship a game.

The simulation always runs inside a server: in-process behind a loopback transport for
single-player, a listen server for co-op/PvP, and headless for a dedicated host. The
client is a renderer plus predictor talking to that server through a transport interface —
there is no separate single-player code path.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the engine contract and
[docs/ROLLOUT.md](docs/ROLLOUT.md) for the current roadmap (what's done vs. planned).

## Features

**Voxel world & rendering**
- Chunked voxel world (fixed 32³ chunks; dev-configurable voxel size, 0.5 m default) with
  worker-thread greedy meshing, DDA raycast, and radius-based streaming
- PSX-forward renderer: half-resolution internal target with nearest upscale, ordered
  dither, per-vertex snapping (jitter), and affine/`noperspective` texture mapping — the
  signature PS1 look, all toggleable at runtime (F6 shader hot-reload)
- Forward Blinn-Phong lighting: one directional sun + ambient, point lights (≤32) and
  spot lights, emissive materials, atlas materials, torch-style block-light flood-fill
  (levels 0–15) baked into the mesh, per-vertex **ambient occlusion** (corner/edge
  darkening, merge-key-preserved through greedy meshing), plus vertex fog

**World generation & templates**
- FastNoiseLite OpenSimplex2 + FBm terrain (deterministic from a seed)
- Seeded procedural dungeon generation (rooms, corridors, loops, verticality), mixable
  with hand-authored spaces
- **World templates** — pick `normal` (terrain + dungeon), `superflat` (flat building
  ground), or `void` (blank canvas + spawn pad) per project (`game.json "terrain"` / `--terrain`)
- **Environment presets** — `surface`, `underwater` (buoyant gravity + thick blue fog), or
  `space` (near-zero gravity + black void, no fog); drives gravity + fog + ambient together
  (`game.json "environment"` / `--env`)

**Netcode (host-authoritative)**
- ENet UDP transport with reliable + unreliable channels behind a `Transport` interface
  (loopback and ENet share one code path)
- Client-side prediction with rewind-and-replay reconciliation; remote entities
  interpolate 100 ms behind the newest snapshot
- Delta-compressed snapshots with acks (~97% smaller) and 16-bit position quantization
  (MP-verified)
- Session discovery: LAN broadcast beacon + browser and a self-hostable master-server
  list (`tools/master_server.py`)

**Physics & AI**
- Jolt Physics: `CharacterVirtual` capsule controller tuned for FPS feel, chunk mesh
  colliders, and world raycasts
- Recast/Detour navmesh with an A* fallback for NPC pathing
- GAS-lite (GameplayAbilitySystem-inspired) ability layer; `NpcZombie` melee chaser and an
  armed `NpcShooter`

**Animation**
- ozz-animation skeletal core (offset-authoritative), cross-skeleton retargeting, and
  clip-merge onto a canonical Mixamo-named skeleton
- Idle↔walk blending with a server-authoritative walk weight, foot-curve grounding, and
  deterministic facing
- Two-bone foot IK for terrain foot-planting (VLM-verified, no regression)

**Weapons & combat (server-authoritative)**
- Distinct fire modes with real trigger discipline: **SemiAuto** (one shot per trigger
  pull — holding never repeats), **Auto** (held cadence), **Burst** (N per pull), and
  shotgun feel via pellets + spread
- **Magazines**: per-weapon mag size + reserve ammo, empty-mag block, timed reload (R),
  HUD mag/reserve readout; the `finiteAmmo` rule is the arcade↔realistic master switch
- Hitscan (pistol/AR/SMG/shotgun/sniper with penetration + AP/HP ammo) and projectiles
  (RPG/grenade), composed from the GAS-lite effect system

**Editor & authoring**
- ImGui **Room Designer** with a UE5-style dark theme, voxel brush tools, and ImGuizmo gizmos
- **Content Browser** — recursive asset scan, tile grid with type-colored tiles, folder
  breadcrumbs, name filter
- **Drag-drop asset placement** — place models from the browser into the world; placed props
  are **server-authoritative** (Jolt box collision, multiplayer-synced, saved with the world)
- `tools/autorig/` — a headless Blender auto-rigger that rigs an un-rigged humanoid mesh to
  the Mixamo-named skeleton and exports a rigged FBX, so shared clips play with no retargeting

**Scripting & audio**
- Lua 5.4 gameplay scripting via sol2, sandboxed with an instruction budget; hot-reloads live
- Positional 3D audio (miniaudio)

## Tech stack

All third-party dependencies are fetched and built at configure time via CMake
`FetchContent` (pinned versions in [`cmake/Dependencies.cmake`](cmake/Dependencies.cmake);
license summary in [THIRD_PARTY.md](THIRD_PARTY.md)):

| Library | Role |
|---|---|
| GLFW 3.4 | Window, input, GL context |
| glad 2 (GL 4.5 core) | OpenGL loader (generated at configure) |
| GLM 1.0.1 | Math |
| Assimp 5.4.3 | FBX / OBJ / GLB import |
| Jolt Physics 5.2.0 | Physics + character controller |
| Recast/Detour 1.6.0 | Navmesh build + query |
| ozz-animation 0.16.0 | Skeletal animation, blending, IK jobs |
| Lua 5.4.7 + sol2 3.3.0 | Scripting runtime + bindings |
| Dear ImGui (docking) + ImGuizmo | Editor & debug UI |
| ENet 1.3.18 | UDP transport |
| FastNoiseLite 1.1.1 | Worldgen noise |
| enkiTS 1.11 | Task scheduler (parallel meshing/worldgen) |
| stb (stb_image) | PNG / JPG loading |
| miniaudio 0.11.21 | Audio playback |
| nlohmann/json 3.11.3 | Save files & configs |
| EnTT 3.13.2, bitsery 5.2.4 | Vendored, staged for incremental adoption (see roadmap) |

## Building

Both platforms configure and build on every push via CI (Ubuntu/Clang + Windows/MSVC).
The **first** configure fetches and compiles every dependency — Assimp and Jolt dominate;
expect roughly 10–20 minutes once. Requires CMake 3.28+ and Python 3 (for the glad loader
generation, which needs `jinja2`).

### Windows

Requires Visual Studio 2022 (Desktop C++) and Ninja.

```powershell
./scripts/build.ps1            # enters the VS dev shell, configures + builds (Ninja, Release)
./build/MeatEngine.exe         # run
```

`build.ps1` installs `jinja2` for the current user and accepts `-Config Debug` and
`-Clean`.

### Linux

Requires a C++20 compiler (GCC 12+ or Clang 15+), Ninja, Python 3 + jinja2, and the
GL / X11 (and optionally Wayland) dev headers GLFW builds against:

```bash
sudo apt-get install -y ninja-build libgl1-mesa-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libwayland-dev libxkbcommon-dev wayland-protocols python3 python3-pip
python3 -m pip install --user jinja2

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/MeatEngine        # run
```

## Running

Launching with no mode argument opens the ImGui browser menu (single-player, host, LAN
list, internet/master list, direct join). CLI flags bypass the menu for scripting and
dedicated use:

| Flag | Effect |
|---|---|
| `--play` | Straight into single-player (in-process listen server) |
| `--host` | Host a listen server (you play) |
| `--join <addr[:port]>` | Connect to a host |
| `--server` | Headless dedicated server (no window/renderer) |
| `--editor` | Start in the Room Designer editor |
| `--project <dir>` | Load a game project folder (`game.json` + `scripts/` + `assets/`) |
| `--seed <n>` / `--voxelsize <m>` | Override world seed / voxel metric scale |
| `--port <n>` / `--name <s>` / `--master <host[:port]>` | Networking overrides |
| `--load <slot>` | Resume a save on startup |

In-game: **WASD/mouse** move & look, **Space/LShift/LCtrl** jump/sprint/crouch, **LMB/RMB**
fire-or-mine / place, **1–9 / scroll** hotbar, **E** use, **Tab** backpack, **F1** toggle
the Room Designer over the running game, **F5** quicksave, **F6** shader hot-reload.

## Making a game

A game is a project folder — config, Lua, and assets — with no C++ required:

```powershell
python tools/new_project.py MyGame --dir F:\Games        # scaffold a runnable game
./build/MeatEngine.exe --project F:\Games\MyGame --play   # run it
powershell tools/package.ps1 -Project F:\Games\MyGame     # → dist/MyGame + .zip to ship
```

`game.json` sets the name, seed, inventory model, voxel size, and economy/ballistics
rules; `scripts/*.lua` is server-authoritative gameplay. The packaged folder runs
standalone.

## License

Licensed under the **[Apache License 2.0](LICENSE)** — free to use, modify, fork, and
build on, commercially or not.

**Credit is required.** Apache-2.0 §4(c)–(d) obliges you to keep the copyright notice and
to reproduce [`NOTICE`](NOTICE) in anything you distribute, including binaries and hosted
builds. Credit it as `MeatEngine by MysteryMeat`
(https://github.com/MidwestMysteryMeat/MeatEngine) in your credits screen, About box, or
docs. The project and MysteryMeat names are not licensed for endorsement or promotion (§6).

Third-party libraries stay under their own licenses ([THIRD_PARTY.md](THIRD_PARTY.md)).
Bundled art/audio is CC-BY/CC0 with **per-asset authors and titles** in
[assets/ATTRIBUTION.md](assets/ATTRIBUTION.md) — those credits are required separately
(e.g. H4 ships: JamyzGenius, JazOone3D, ABJVNK, Sebastian Sosnowski, Gerardo Justel).

Previously MIT; relicensed to Apache-2.0 on 2026-07-30. Snapshots released under MIT stay MIT.
