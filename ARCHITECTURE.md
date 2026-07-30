# MeatEngine Architecture

A voxel FPS engine, **multiplayer-native**: the simulation always runs in a server
(in-process for single-player, listen server for co-op/PvP, headless for dedicated),
and the client is a renderer + predictor talking to it through a transport. One
executable, modes: **Game** (default: loopback listen server), **Host** (`--host`),
**Join** (`--join <addr>`), **Dedicated** (`--server`, headless), **Editor** (F1,
Room Designer), **Capture** (`--capture`, headless render for asset QA).
C++20, OpenGL 4.5 core, CMake + FetchContent.

This file is the **contract**. Code that doesn't match the signatures, ownership rules,
or threading rules here is wrong even if it works.

## Units & coordinates

- Right-handed, **Y-up**, meters. -Z is "forward" at yaw 0 (OpenGL convention).
- **1 voxel = 0.5 m**. Chunks are **32³ voxels = 16 m cubes**.
- Voxel coords are `glm::ivec3` (world-voxel space); chunk coords `ChunkPos {int x,y,z}`;
  `worldToVoxel(p) = floor(p / 0.5f)`.
- Player capsule: radius 0.35 m, height 1.80 m (crouch 0.95 m), eye at 1.62 m (crouch 0.82 m).
- Fixed simulation tick: **60 Hz**. Rendering interpolates with accumulator alpha.

## Directory layout

```
src/engine/core/       Engine spine: loop, time, jobs, log, entity registry, events
src/engine/platform/   Window (GLFW), Input (raw mouse), PlayerCommand sampling
src/engine/voxel/      Blocks, chunks, meshing, DDA raycast, streaming, edit ops
src/engine/render/     GL wrappers, forward renderer, PSX pipeline, camera, HUD
src/engine/physics/    Jolt wrapper: world, chunk colliders, character controller
src/engine/anim/       Skeletal animation (canonical Mixamo-named skeleton)
src/engine/asset/      Assimp model loading (FBX/OBJ/GLB), textures (stb), audio (miniaudio)
src/engine/script/     Lua (sol2) host + bindings
src/engine/save/       Save/load (meta.json + chunks.bin)
src/engine/net/        Transport (loopback + ENet UDP), messages, snapshots, replication
src/game/              Gameplay: player, weapons, inventory, items, enemies, dungeon gen
src/editor/            Room Designer. NOTHING in src/engine or src/game includes this.
tools/                 autorig CLI, asset staging/audit scripts (Python)
assets/                shaders/, textures/, models/, scripts/ (Lua), ATTRIBUTION.md
```

Dependency direction: `editor → game → engine`. `engine` never includes `game` or `editor`.

## Ownership

`Engine` (core/Engine.h) owns every subsystem as a value member, constructed in
declaration order, destroyed in reverse. No singletons, no globals except the logger.
Subsystems get references, never pointers they could outlive.

```
Engine
├── Window, Input            (platform)
├── JobQueue                 (core, workers for meshing/gen ONLY)
├── Renderer                 (render)
├── PhysicsWorld             (physics)
├── AssetCache               (asset)
├── VoxelWorld               (voxel)
├── EntityRegistry           (core)
├── ScriptHost               (script)
├── SaveSystem               (save)
├── GameState                (game: player, inventory, weapons)
└── Editor                   (editor; only ticked in editor mode)
```

## Core loop (core/Engine.cpp)

```cpp
while (!window.shouldClose()) {
  window.pollEvents();
  jobQueue.drainMainThread();            // completed worker results land here
  PlayerCommand cmd = input.sampleCommand(tick);   // ONE place input becomes intent
  accumulator += frameDt;
  while (accumulator >= kFixedDt) {      // 60 Hz sim
    simulate(cmd, kFixedDt);             // physics, gameplay, scripts
    ++tick; accumulator -= kFixedDt;
  }
  render(accumulator / kFixedDt);        // interpolation alpha
  window.swap();
}
```

## Networking model (in the MVP)

Server-authoritative, client-predicted — the Quake/Source lineage:

- **The sim is the server.** Voxel world, physics, gameplay systems tick at 60 Hz inside
  `ServerSim`. Single-player runs `ServerSim` in-process behind a `LoopbackTransport`
  (zero-copy queue) — there is no separate single-player code path, ever.
- **Client sends `PlayerCommand`s** (stamped with tick), server applies them, and
  broadcasts **snapshots at 20 Hz** (full state per relevant entity for MVP; delta
  compression is a later optimization slot).
- **Client predicts its own movement**: it runs the same `CharacterController` locally,
  keeps a ring buffer of unacked commands, and on each snapshot rewinds to the server
  state and replays — divergence corrections smooth over 100 ms.
- **Remote entities interpolate** 100 ms behind the newest snapshot.
- **Voxel edits are server-applied ops** broadcast to clients; chunks themselves never
  travel when clean — clients regenerate identical chunks from `(params, seed)` and the
  server sends only modified-chunk deltas (RLE, same encoding as the save format).
- **Transport**: `Transport` interface with two impls — `LoopbackTransport` and
  `EnetTransport` (ENet UDP, reliable + unreliable channels). Nothing above the
  transport knows which is in use.
- Shooting is lag-compensated later (PvP phase); co-op MVP uses server-side hit tests
  against interpolated positions.

Shape rules this rests on (were true from day one):
1. Identity is `EntityId` (u64, generation in high 16 bits). Never a pointer.
2. All input funnels through `PlayerCommand` — nothing reads GLFW state in gameplay.
3. Gameplay state lives in components serialized by the save system. A save, a snapshot,
   and a net update are the same serialization.
4. World mutations are ops (`VoxelEdit`, damage, pickup) via `EventBus`, applied in one
   place each — that place is the server.
5. Dungeon generation is a pure function of `(DungeonParams, seed)`.

## Key contracts (namespace `meat`)

### core/EntityRegistry.h — hand-rolled, ~300 lines, no third-party ECS
```cpp
using EntityId = std::uint64_t;                    // 0 = invalid; high 16 bits = generation
class EntityRegistry {
  EntityId create();
  void     destroy(EntityId);
  bool     alive(EntityId) const;
  template <typename T> T&   add(EntityId, T component);
  template <typename T> T*   get(EntityId);        // nullptr if absent
  template <typename T> void remove(EntityId);
  template <typename... Ts, typename Fn> void each(Fn&&);  // fn(EntityId, Ts&...)
};
```
Storage: per-type dense `std::vector<T>` + sparse index map; iteration order = dense order.

### core/JobQueue.h
```cpp
class JobQueue {
  void start(unsigned workers);            // hardware_concurrency clamped to [2,4]
  void stop();                             // joins; called from ~JobQueue
  void enqueue(std::function<void()>);     // runs on a worker. PURE WORK ONLY.
  void post(std::function<void()>);        // queued for main thread
  void drainMainThread();                  // Engine calls once per frame
};
```
**Threading law:** workers touch only their job's inputs/outputs. All GL, all Jolt writes,
all registry access happen on the main thread. Workers hand results back via `post`.

### platform/Window.h, Input.h
```cpp
struct WindowDesc { int width = 1600, height = 900; const char* title; bool vsync = true; };
class Window {
  bool init(const WindowDesc&); void pollEvents(); bool shouldClose() const;
  void swap(); void setRelativeMouse(bool); glm::ivec2 framebufferSize() const;
  GLFWwindow* handle() const;
};
struct PlayerCommand {
  std::uint64_t tick = 0;
  glm::vec2 move{0};                       // x strafe, y forward, unit-clamped
  float yaw = 0, pitch = 0;                // absolute radians, pitch clamped ±89°
  bool jump=false, crouch=false, sprint=false, fire=false, use=false, reload=false;
};
class Input {
  void attach(Window&);                    // installs GLFW callbacks
  void beginFrame();                       // clears per-frame deltas/presses
  bool down(int glfwKey) const; bool pressed(int glfwKey) const;
  glm::vec2 mouseDelta() const;            // raw, unscaled
  float sensitivity = 0.0022f;             // radians per count
  PlayerCommand sampleCommand(std::uint64_t tick);  // integrates yaw/pitch internally
};
```
Raw mouse motion (`GLFW_RAW_MOUSE_MOTION`) when captured. Yaw/pitch integrate in `Input`
so look latency never depends on tick timing.

### voxel/
```cpp
using BlockId = std::uint16_t;             // 0 = air
struct BlockDef { std::string name; std::array<std::uint16_t,6> faceTex; bool solid = true; };
class BlockRegistry { BlockId add(BlockDef); const BlockDef& get(BlockId) const; };

inline constexpr int   kChunkSize = 32;
inline constexpr float kVoxelSize = 0.5f;
struct ChunkPos { int x, y, z; auto operator<=>(const ChunkPos&) const = default; };

class Chunk {                              // flat array, x + z*32 + y*32*32
  BlockId at(int x, int y, int z) const;
  void    set(int x, int y, int z, BlockId);
  bool    dirty() const;                   // needs remesh
};

struct VoxelVertex { glm::vec3 pos; glm::i8vec3 normal; glm::vec2 uv; std::uint16_t tex; };
struct ChunkMeshData { std::vector<VoxelVertex> vertices; std::vector<std::uint32_t> indices; };

// PURE, thread-safe: runs on workers. Neighbors may be null (treated as air).
ChunkMeshData buildChunkMesh(const Chunk&, const std::array<const Chunk*,6>& neighbors,
                             const BlockRegistry&);

class VoxelWorld {
  BlockId blockAt(glm::ivec3 voxel) const;
  void    setBlock(glm::ivec3 voxel, BlockId);          // marks dirty, records VoxelEdit
  void    update(glm::vec3 playerPos, JobQueue&);       // stream in/out, enqueue remesh
  struct RayHit { glm::ivec3 voxel; glm::ivec3 normal; float t; BlockId block; };
  std::optional<RayHit> raycast(glm::vec3 origin, glm::vec3 dir, float maxDist) const; // DDA
  // Streaming radius: 6 chunks horizontal, 2 vertical. Meshing on workers,
  // upload + collider sync on main thread when the job posts back.
};
```

### render/
```cpp
struct Camera { glm::vec3 pos; float yaw=0, pitch=0; float fovY = glm::radians(75.f);
                glm::mat4 view() const; glm::mat4 proj(float aspect) const; };
struct PsxOptions { bool nearestFiltering = true; float internalScale = 0.5f;   // 800x450 target
                    bool dither = true; bool fog = true; glm::vec3 fogColor; float fogStart, fogEnd; };
using MeshHandle = std::uint32_t; using TextureHandle = std::uint32_t;
class Renderer {
  bool init(Window&);                      // loads GL via glad, builds pipelines
  MeshHandle    uploadChunkMesh(const ChunkMeshData&);   // main thread only
  void          destroyMesh(MeshHandle);
  TextureHandle loadTexture(const std::filesystem::path&);  // PNG/JPG via stb
  void beginFrame(const Camera&, float alpha);
  void submitChunk(MeshHandle, glm::vec3 originWorld);
  void submitMesh(MeshHandle, const glm::mat4&, TextureHandle albedo);
  void setDirectionalLight(glm::vec3 dir, glm::vec3 color);
  void submitPointLight(glm::vec3 pos, glm::vec3 color, float radius);   // ≤ 32/frame
  void submitSpotLight(glm::vec3 pos, glm::vec3 dir, glm::vec3 color, float radius, float angle);
  void drawCrosshair();
  void endFrame();                         // resolves PSX target to backbuffer
  PsxOptions psx;                          // toggleable at runtime
};
```
Forward Blinn-Phong, one directional + point/spot array in a UBO. PSX look = render to a
half-res target, nearest upscale, ordered dither in the resolve shader, vertex fog.
Shaders live in `assets/shaders/*.{vert,frag}`, hot-reloadable (F6).

### physics/
```cpp
class PhysicsWorld {
  bool init(); void step(float fixedDt);   // main thread
  void syncChunkCollider(ChunkPos, const ChunkMeshData&);  // MeshShape from render verts
  void removeChunkCollider(ChunkPos);
  struct RayHit { bool hit=false; glm::vec3 pos, normal; EntityId entity = 0; };
  RayHit raycast(glm::vec3 from, glm::vec3 dir, float maxDist) const;
};
class CharacterController {                // Jolt CharacterVirtual
  void update(const PlayerCommand&, float fixedDt, PhysicsWorld&);
  glm::vec3 position() const; glm::vec3 velocity() const; bool onGround() const;
  // Tuning (constants, all in one struct): walk 4.5 m/s, sprint 7.0, crouch 2.2,
  // jump 4.6 m/s up, air control 0.3, step-up 0.35 m, max slope 46°, gravity -18.
};
```

### Save format (save/)
`saves/<slot>/meta.json`: player transform, health, inventory, equipped, dungeon seed,
tick. `saves/<slot>/chunks.bin`: `[ChunkPos][u32 rleCount][(BlockId,u16 run)...]` for every
player-modified chunk only. F5 save, F9 load, `--load <slot>` on startup.

### Anim (anim/) — after the slice
Canonical skeleton = **Mixamo bone names**. Loaders map source skeletons (Mixamo direct,
UE mannequin via name table) onto it at import. One shared clip set plays on every
conforming character. Viewmodel = separate simple 2-clip player (idle/fire).

## Style rules

- C++20. `/W4 /permissive-` clean. No exceptions on hot paths; `std::optional` + log.
- RAII everywhere; `std::unique_ptr` only for pimpl/polymorphism; value members otherwise.
- Files/types `PascalCase`, methods/vars `camelCase`, members `m_`, constants `kName`.
- No `using namespace` in headers. Includes: own header first, then engine, then third-party, then std.
- `.clang-format` at repo root is law. Comments explain *why*, never *what*.
