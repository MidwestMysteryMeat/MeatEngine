# MeatEngine — Open-Source Engine Reuse Survey

**Date:** 2026-07-29
**Scope:** Survey of open-source C++ game engines to identify concrete, license-compatible
components MeatEngine can repurpose. Web + repo reading only; no code was changed.

## License rule (applied throughout)

MeatEngine is **Apache-2.0, PUBLIC**. For **CODE REUSE** (copying/porting source), only these
licenses are eligible: **MIT / BSD / zlib / Apache-2.0 / public-domain**. Anything else
(**GPL / LGPL / custom-restricted**) is **IDEAS ONLY** — study architecture, never copy or
transliterate code. Every engine below is flagged explicitly. When copying MIT/BSD/zlib code,
**retain the upstream copyright + permission notice** on the borrowed file(s) and record it in
`THIRD_PARTY.md`.

> Verification note: GitHub's license auto-detector returned `NOASSERTION` for several genuinely
> permissive engines (Cafu, O3DE, Nebula, Recoil, Defold) because of non-standard preambles or
> dual-license wrappers. Every license below was confirmed by reading the actual LICENSE file, not
> the API metadata.

---

## MeatEngine current state (grounding for the recommendations)

Read from `F:\MeatEngine\src` on 2026-07-29. MeatEngine is further along than a greenfield engine;
the recommendations target **real gaps**, not things already built.

**Already implemented**
- **Netcode:** `engine/net/` has a clean `Transport` abstraction (`Transport.h`), loopback +
  ENet backends (`LoopbackTransport`, `EnetTransport`), a hand-rolled wire protocol
  (`Messages.h/.cpp`, `ByteStream.h`), LAN discovery, and a tiny HTTP client. `game/Client.h`
  already does **client-side prediction with rewind-and-replay reconciliation** (unacked-command
  buffer `m_unacked`, `applySnapshot()`), and **remote-player interpolation 100 ms behind the
  newest snapshot** with bracketed states. Server sim in `game/ServerSim`.
- **Skeletal models:** `engine/asset/SkeletalModel.h` loads skinned FBX/GLB via **Assimp**
  (bones, inverse-bind offsets, per-bone TRS tracks, ≤4 influences, 128-bone palette).
  `engine/anim/Animator.h` samples a **single clip** (`samplePose`, a pure function) plus
  `bindPose`/`idlePose` fallbacks.
- **Cross-platform primitives:** windowing/input via **GLFW** (`engine/platform/Window`,`Input`),
  **glad**, **glm**, **Assimp**, **ENet** — all portable. Only `net/HttpTiny.cpp` and
  `net/LanDiscovery.cpp` contain raw Win32 sockets, and both **already have `#ifdef _WIN32` /
  `#ifndef _WIN32` POSIX branches**.
- Voxel (`Chunk`, `ChunkMesher`, `VoxelWorld`), Jolt physics (`PhysicsWorld`,
  `CharacterController`), sol2/Lua (`script/ScriptHost`), in-engine editor (`editor/RoomEditor`).

**Concrete gaps these recommendations fill**
1. **Animation blending / state machine.** Only single-clip sampling exists; no crossfade, blend
   tree, or controller. `idlePose` is an admitted hack. (FBX *loading* is already solved by Assimp
   — the gap is the runtime animation graph, not the importer.)
2. **Netcode robustness.** `SnapshotMsg` sends the **full** player + entity list every tick (no
   delta compression, no interest management). **No lag compensation / server-side rewind** for
   hit registration — a real problem for an FPS.
3. **Cross-platform (Linux).** Windows-only today. The blockers are small: CMake/CI for Linux and
   validating the existing POSIX socket branches — not a windowing rewrite.
4. **Asset pipeline.** Runtime load re-runs Assimp every time; no cooked/native binary model
   format for fast loads.

---

## Per-engine findings

### 1. Cafu Engine — MIT (COPYABLE)
- **Repo:** canonical `https://bitbucket.org/cafu/cafu` (Git); GitHub mirror `github.com/DNS/Cafu`.
  The `github.com/cafu-engine/cafu` URL in the brief **404s — no such org.**
- **License:** **MIT**, verified from `LICENSE.txt` (verbatim MIT grant). Cafu **relicensed
  GPLv3 → MIT on 2016-06-19.** Copy-eligible.
- **Activity:** **Dormant** (mirror last push 2019-10-29). Treat as a stable reference, not evolving.
- **Netcode model:** server-authoritative client/server over **raw UDP (Berkeley sockets)**; full
  client prediction; **delta compression** of per-entity state; **PVS-based interest management**
  (replicate only what a client can see). Proven at 8 clients over 64 kbit/s.
- **Concrete borrowings:**
  1. **Port `Libs/Network/State.*` (delta / state-diff).** Small, self-contained, MIT. This is
     exactly the baseline+changed-fields layer ENet does *not* provide — the single cleanest copy
     candidate for fixing MeatEngine's full-snapshot-every-tick problem.
  2. **Client prediction/reconciliation flow** in `Ca3DE/Client/ClientWorld.cpp` — a readable
     "store pending inputs, replay on correction" skeleton (MeatEngine already has its own version;
     use Cafu to cross-check edge cases).
  3. **Study** the PVS interest-management hook in `Ca3DE/Server/ServerWorld.cpp`. Swap Cafu's
     BSP-PVS for a voxel/grid visibility test; keep the "compute per-client visible set, replicate
     only visible entities" architecture.

### 2. O3DE — Apache-2.0 OR MIT dual (COPYABLE, but framework-coupled)
- **Repo:** `github.com/o3de/o3de`.
- **License:** **Apache-2.0 OR MIT** (dual), verified from `LICENSE.txt`. Both on the allow-list.
  Note some tooling deps are copyleft (Qt = LGPLv3) — those are editor deps, keep the core clear.
- **Activity:** **Very active** (last push 2026-07-28, ~9.5k stars).
- **Reality check:** ~230 MB of source on the AZ framework (AzCore, EBus, reflection). You **cannot
  drop a gem in wholesale** — every gem transitively needs AzCore. The realistic play is
  **architecture study + reimplement the algorithm**, not linking modules.
- **Concrete borrowings (architecture blueprint):**
  1. **Multiplayer Gem prediction/rollback design** — `Gems/Multiplayer/Code/Source/`:
     `NetworkInput/` (command frames), `NetworkTime/` (server-frame stamping),
     `Components/LocalPredictionPlayerInputComponent`, and the `Autonomous` vs `Authority` role
     split. A more complete "local prediction + backward reconciliation" model than Cafu's.
  2. **Lag compensation** — `NetworkTime` + `NetworkInput` frame-indexed input and time-rewind are
     the concrete pattern to implement **server-side hit rewind**, MeatEngine's clearest netcode gap.
  3. **Study** `Code/Framework/AzNetworking` auto-packet serialization as a *design template*
     (declarative field descriptions → generated pack/unpack) but do **not** port it (AZ-reflection
     bound). Reimplement over ENet/ByteStream.
  - **Skip:** EMotionFX (animation) and Asset Processor — powerful but deeply AZ-coupled; adopting
    either means adopting AzCore. Ideas-only on integration-cost grounds despite the permissive license.

### 3. ezEngine — MIT (COPYABLE)
- **Repo:** `github.com/ezEngine/ezEngine`. **License:** **MIT** (verified `license.spdx_id`).
  **Activity:** very active (2026-07-30).
- **Concrete borrowings:**
  1. **Component/entity framework** — `ezWorld`/`ezComponent`/`ezComponentManager`
    (Code/Engine/Core): data-oriented, block-allocated, message-routed. Good pattern reference for
    MeatEngine's entity layer.
  2. **Animation system** — `Code/Engine/RendererCore/AnimationSystem` (`Skeleton.h`,
    `AnimationPose.h`, `AnimGraph`, `AnimationClipResource.h`). **Key insight:** ez's runtime
    skinning is built on **ozz-animation** (MIT, standalone). For a raw-GL engine, lift **ozz
    directly** rather than ez's wrappers.
  3. **Foundation/Platform** thin platform-abstraction layer — worth studying for the Linux port.
  - Editor is Qt — heavyweight, don't lift.

### 4. Ogre3D — MIT (COPYABLE)
- **Repo:** `github.com/OGRECave/ogre`. **License:** **MIT** (verified). **Activity:** very active
  (2026-07-29).
- **Cleanly liftable despite MeatEngine using raw GL:** Ogre's animation lives in `OgreMain/src`
  and depends on `OgreMath` (Vector3/Quaternion/Matrix4), **not** on Ogre's render systems.
- **Concrete borrowings:**
  1. **Skeletal-animation math + weighted blending** — `OgreSkeleton.cpp`, `OgreBone.cpp`,
    `OgreAnimation.cpp`, `OgreAnimationTrack.cpp`, `OgreKeyFrame.cpp`, **`OgreAnimationState.cpp`**,
    `OgreSkeletonInstance.cpp`. Battle-tested keyframe interpolation + **multi-clip weighted
    blending** — the in-survey answer to MeatEngine's blending gap. Swap Ogre's math types for glm.
  2. **Mesh/skeleton binary serialization** — `OgreSkeletonSerializer.cpp`,
    `OgreMeshSerializerImpl.cpp`, `OgreMeshFileFormat.h`, `OgreSerializer.cpp`. A documented,
    versioned `.mesh`/`.skeleton` format to adopt as MeatEngine's **cooked native format** (faster
    than re-running Assimp at runtime).
  - Caveat: classes lean on Ogre's `Resource`/`SharedPtr`/`StringInterface` plumbing — cut those
    threads when porting. Heavier disentangle than Irrlicht.

### 5. Irrlicht — zlib (COPYABLE)
- **Living fork:** the original SourceForge Irrlicht is dead (~2019) and `minetest/irrlicht`
  (IrrlichtMt) is **archived**. The maintained code is **inside Luanti's repo**:
  `github.com/luanti-org/luanti`, subdir **`irr/`** (last push 2026-07-30). `irr/LICENSE` = **zlib**
  (verified verbatim). This is the tree to pull from.
- **Concrete borrowings (from `irr/src`):**
  1. **Lightweight skinned-mesh animation** — `SkinnedMesh.cpp` (`ISkinnedMesh`/`CSkinnedMesh`) +
    `AnimatedMeshSceneNode.cpp`. The **least entangled** skeletal code of the three anim options —
    self-contained joint hierarchy + weight/keyframe skinning, matches MeatEngine's raw-GL /
    no-scene-graph philosophy.
  2. **Model loaders** — `CB3DMeshFileLoader.cpp` (Blitz3D **.b3d**, skeletal — simple format
    ideal for a PSX pipeline), `CXMeshFileLoader.cpp` (DirectX **.x**, skeletal),
    `COBJMeshFileLoader.cpp` (static). Lift file-by-file.
  3. **No FBX loader exists in any Irrlicht/fork** — keep Assimp for FBX; use Irrlicht loaders only
    for B3D/X/OBJ. (The Luanti fork also dropped MD3; take `CMD3MeshFileLoader` from the original
    zlib tree if MD3 is ever wanted.)
- Nabla/IrrlichtBAW (Apache-2.0, active) is a Vulkan rewrite — **not** classic skinned-mesh code,
  not useful for this gap.

### 6. Godot — MIT (COPYABLE)
- **Repo:** `github.com/godotengine/godot`. **License:** **MIT** (verified). **Activity:** very
  active (2026-07-30).
- **Directly relevant because Godot also runs replication over ENet** (`modules/enet/` provides the
  `MultiplayerPeer`) — the layering is transplantable, not just inspirational.
- **Concrete borrowings:**
  1. **High-level replication design** — `modules/multiplayer/`:
    `scene_replication_config.*` (declarative "which properties replicate" + spawn-vs-sync split),
    `scene_replication_interface.*` (per-tick delta/state diffing),
    `multiplayer_synchronizer.*` / `multiplayer_spawner.*`, `scene_rpc_interface.*` (RPC routing).
    The **config-driven replication-set** model is the highest-value MIT-clean architecture lift for
    MeatEngine's server-authoritative stack. Study `scene_cache_interface` RPC id-caching to cut
    per-packet path overhead.
  2. **C-ABI plugin/mod pattern** — `core/extension/gdextension_interface.h` is a pure **C ABI**
    (function-pointer table + JSON versioning). Reference template if MeatEngine ever wants a stable
    mod ABI independent of C++ name-mangling.

### 7. Luanti / Minetest — LGPL-2.1-or-later (⚠️ IDEAS ONLY — DO NOT COPY CODE)
- **Repo:** `github.com/luanti-org/luanti` (formerly `minetest/minetest`). **License:** the engine
  code is **LGPL-2.1-or-later** (per-file SPDX tags, e.g. `src/mapblock.h`). The root `LICENSE.txt`
  describes only **media** (CC BY-SA) — a red herring; the *code* is LGPL. **Study only, never copy
  or transliterate.** **Activity:** very active (2026-07-30). Highly relevant because it is also
  voxel + Lua.
- **Architecture patterns to study (clean-room reimplement):**
  1. **Versioned, compressed mapblock serialization** — `src/mapblock.cpp` `serialize()`: a 16³
    block packed as separate param planes, zlib/zstd-compressed, tagged with a **format-version
    byte** so old clients degrade gracefully. Adopt the *idea* for MeatEngine's voxel streaming
    (`VoxelOp`/`BatchVoxelOp`).
  2. **Opcode-table packet dispatch** — `src/network/` (`networkpacket.cpp`,
    `clientpackethandler.cpp`, `serverpackethandler.cpp`, opcode tables). Pattern for scaling
    MeatEngine's `MsgType` switch as the protocol grows.
  3. **Threaded dirty-block remeshing** — `src/client/mapblock_mesh.cpp` + mesh-update worker queue:
    decouple receive → remesh so network stalls never stutter render.
  4. **Per-subsystem, server-vs-client Lua API partition** — `src/script/lua_api/` (one `l_*.cpp`
    per subsystem, split into server- and client-context tables). Reference *shape* for organizing
    MeatEngine's sol2 bindings.

### 8. Nebula — BSD-2-clause (COPYABLE)
- **Repo:** `github.com/gscept/nebula`. **License:** **BSD-2-clause** (verified in `license.txt`;
  GitHub shows NOASSERTION only because the file isn't named `LICENSE`). **Activity:** active
  (2026-07-27).
- **Concrete borrowings:**
  1. **Frame-graph / frame-script render-pass system** — data-driven declarative ordering of passes
    + resource barriers. Copy the *pattern* (BSD, copyable) to keep MeatEngine's PSX forward pipeline
    configurable without hardcoding pass order.
  2. **Fiber-based job system + reflection/serialization** — clean, standalone, license-compatible;
    candidate for MeatEngine's editor + threading (MeatEngine already has a `core/JobQueue` — compare).

### 9. Serious Engine 1.10 — GPL-2.0 (⚠️ IDEAS ONLY)
- **Repo:** `github.com/Croteam-official/Serious-Engine`. **License:** GPL-2.0 (verified). **Activity:**
  frozen (2020, shipped-game snapshot). **Ideas only:** its `.es` entity-scripting compiler +
  state-machine entity model (reference for Lua-scripted weapon/actor logic); world/portal-visibility
  architecture (reference for voxel-world partitioning). Nothing copy-eligible.

### 10. Spring RTS → RecoilEngine — GPL-2.0-or-later (⚠️ IDEAS ONLY)
- **Repo:** `beyond-all-reason/spring` now redirects to **`github.com/beyond-all-reason/RecoilEngine`**
  (the living Spring line). **License:** GPL-2.0-or-later (verified). **Activity:** very active
  (2026-07-29). **Ideas only:** deterministic lockstep sim with **per-frame checksum desync
  detection** (a determinism/validation reference for server-authoritative sim); layered
  **synced-vs-unsynced Lua** context model (a strong model for MeatEngine's sol2 server/client boundary).

### 11. FIFE — LGPL-2.1 (⚠️ IDEAS ONLY, and irrelevant)
- **Repo:** `github.com/fifengine/fifengine`. **License:** LGPL-2.1. **Activity:** active (2026-07-29).
  Isometric **2D** tile engine — no 3D renderer, physics, or netcode overlap with a voxel FPS.
  **Nothing standout. Skip.**

### 12. Defold — "Defold License v1.0" (⚠️ IDEAS ONLY — the Apache trap)
- **Repo:** `github.com/defold/defold`. **License:** **Defold License v1.0**, Apache-2.0-*derived*
  but **NOT OSI-approved and NOT on the copy list.** §4(a): *"You do not sell or otherwise
  commercialise the Work or Derivative Works as a Game Engine Product."* Because **MeatEngine IS a
  game engine**, copying Defold code would violate the field-of-use clause. **Ideas only despite the
  Apache lineage.** **Activity:** very active (2026-07-30).
- **Ideas:** flat data-driven **component/GameObject/collection** model addressed by URL (design
  reference for the editor/entity layer); its `bob` build tool + live Lua hot-reload (reference for
  the sol2 iteration loop).

### 13. Torque3D — MIT (COPYABLE) — HIGH RELEVANCE
- **Repo:** `github.com/TorqueGameEngines/Torque3D`. **License:** **MIT** verified from `LICENSE.md`
  (`Copyright (c) 2012-2016 GarageGames, LLC`; GitHub shows NOASSERTION only because appended
  third-party notices follow the MIT block). **Activity:** very active (2026-07-28).
- **Netcode (the famous part, in `Engine/source/sim/`):** `netObject.cpp/h`, `netGhost.cpp`,
  `netConnection.cpp/h`, `netEvent.cpp`, `netInterface.cpp`, plus move-based prediction in
  `T3D/gameBase/std/stdMoveList.*`. Architecture: **ghosting/scoping** (per-connection ghost
  records; objects scoped in/out by relevance, then **priority-ranked against a per-packet bandwidth
  budget** so only the most important objects update each tick); **delta compression** over
  bitstreams (only changed bits); a separate **NetEvent** layer for guaranteed/ordered events; and
  **move-based client prediction** (`stdMoveList`).
- **Concrete borrowing:** reimplement the **NetObject + ghost-manager scope→prioritize→
  bandwidth-budget→delta pipeline** on ENet. This is the proven answer to "which of N objects do I
  replicate this tick" — exactly what MeatEngine's full-list-every-tick `SnapshotMsg` lacks, and
  what raw ENet does not solve. Caveat: tightly coupled to Torque's `BitStream` / `SimObject` RTTI,
  so treat as a strong **architecture reference to port**, not a file-level copy (MIT permits either).

### 14. Esoterica — MIT (COPYABLE) — HIGH RELEVANCE for animation
- **Repo:** `github.com/BobbyAnguelov/Esoterica`. **License:** **MIT** verified from `LICENSE`
  (`Copyright (c) 2022-2026 Bobby Anguelov`). **Activity:** active (2026-07-22). A AAA-style C++20
  engine by an ex-IO Interactive animation programmer.
- **Concrete borrowing:** the **animation graph runtime** — `Code/Engine/Animation/Graph` plus
  `IK`, `AnimationBlender`, `AnimationBoneMask`, `AnimationSyncTrack`, `AnimationRootMotion`,
  `TaskSystem`. A self-contained, purpose-built **state-machine + blend-tree** pose/task pipeline
  with sync tracks, bone masks, root motion and IK (NOT ozz-based). Directly fills MeatEngine's
  animation gap with far more than crossfade. Caveat: C++20 coupled to Esoterica's own math/resource
  types — expect adaptation, not a clean lift.

### 15. Cute Framework — zlib OR public-domain dual (COPYABLE)
- **Repo:** `github.com/RandyGaul/cute_framework`. **License:** dual **zlib / Unlicense-PD** (verified
  from LICENSE: "ALTERNATIVE A zlib / ALTERNATIVE B Public Domain"). **Activity:** active (2026-07-25).
- **Concrete borrowing:** its **`cute_net`-style reliable-UDP + secure connection-token handshake**
  (packet encryption, reliability layer). Dependency-light C, permissive — a reference or direct lift
  to add the **secure connect-token flow ENet lacks** to MeatEngine's transport (a PUBLIC server
  wants authenticated connects).

### 16. Librebox — MIT (COPYABLE)
- **Repo:** `github.com/StayBlue/librebox-demo` (the brief's `librebox-devs/librebox-demo` 404s).
  **License:** **MIT** (verified). **Activity:** stale (2025-08-23). A C++ engine embedding **Luau**
  (Roblox's Lua fork) to replicate the Roblox API; client-side demo, no server replication to study.
- **Concrete borrowing:** its **Luau host-embedding pattern** as a hardened script sandbox. MeatEngine
  runs vanilla Lua via sol2 with **no sandboxing** — Luau (also MIT) adds read-only tables, per-VM
  memory/instruction budgets, and optional typechecking, which a PUBLIC engine wants before running
  untrusted user scripts. Relevant *because MeatEngine is public and scriptable*.

### 17. SFML — zlib (COPYABLE)
- **Repo:** `github.com/SFML/SFML`. **License:** **zlib** (verified). **Activity:** active (2026-07-23).
- **Verdict: mostly SKIP.** Its window/input layer doesn't beat GLFW (already used) and graphics is
  2D-only. Its OpenAL-backed positional-audio module would be a clean zlib drop-in — **but MeatEngine
  already has `engine/audio/AudioEngine`**, so even that is redundant. No action.

### 18. Solarus — GPL-3.0 (⚠️ IDEAS ONLY)
- **Repo:** `gitlab.com/solarus-games/solarus`. **License:** GPL-3.0 (verified on GitLab).
  **Activity:** very active (2026-07-30). 2D Zelda-like ARPG engine. **Ideas only:** its **per-entity
  Lua API design** (every entity is Lua userdata with lifecycle callbacks `on_update`/`on_created`/
  `on_activated` and consistent `entity:get_x()/set_property()` accessors) — a clean *shape* reference
  for structuring MeatEngine's sol2 entity bindings. Reimplement; copy nothing.

### 19. Carimbo — MIT (COPYABLE) — SKIP
- `github.com/willtobyte/carimbo`. MIT (verified), active (2026-06-23). 2D SDL sprite/arcade engine.
  Wrong domain — no 3D/voxel/physics/netcode overlap worth grafting.

### 20. Cocos2d-x — MIT · JNGL — zlib — both SKIP
- **Cocos2d-x** `github.com/cocos2d/cocos2d-x` — MIT (verified from `licenses/LICENSE_cocos2d-x.txt`),
  stale (2025-05-09). 2D-first mobile framework; nothing a desktop GL4.5 voxel FPS needs.
- **JNGL** `github.com/jhasse/jngl` — zlib (verified), active (2026-07-22). 2D cross-platform game
  library; wrong domain, no 3D subsystem to harvest.
- **FIFE** (above) — LGPL, 2D iso — likewise SKIP.

---

## Complete license + activity table (all 20 C++ engines)

| Engine | License (verified at source) | Last push | Copy-eligible | Role |
|---|---|---|---|---|
| Cafu | MIT (LICENSE.txt; GPLv3→MIT 2016) | 2019-10-29 | **Yes** | **Borrow** (netcode delta) |
| Carimbo | MIT (API) | 2026-06-23 | Yes | Skip (2D) |
| Cocos2d-x | MIT (LICENSE file) | 2025-05-09 | Yes | Skip (2D mobile) |
| Cute Framework | zlib / PD dual (LICENSE) | 2026-07-25 | **Yes** | **Borrow** (secure UDP handshake) |
| Defold | Defold License v1.0 (§4a no-engine-use) | 2026-07-30 | **No** | Ideas only |
| Esoterica | MIT (LICENSE) | 2026-07-22 | **Yes** | **Borrow** (animation graph) |
| FIFE | LGPL-2.1 (API) | 2026-07-29 | No | Skip (2D iso) |
| JNGL | zlib (API) | 2026-07-22 | Yes | Skip (2D) |
| Godot | MIT (LICENSE.txt) | 2026-07-30 | **Yes** | **Borrow** (replication config) |
| Irrlicht (Luanti `irr/`) | zlib (irr/LICENSE) | 2026-07-30 | **Yes** | **Borrow** (skinned mesh + B3D) |
| Librebox | MIT (LICENSE) | 2025-08-23 | **Yes** | **Borrow** (Luau sandbox) |
| Luanti | LGPL-2.1+ (SPDX in src/) | 2026-07-30 | No | Ideas only (voxel/Lua) |
| Nebula | BSD-2-clause (license.txt) | 2026-07-27 | **Yes** | **Borrow** (frame graph) |
| O3DE | Apache-2.0 OR MIT (LICENSE.txt) | 2026-07-28 | **Yes**\* | **Borrow arch** (prediction/lag-comp) |
| Ogre3D | MIT (API) | 2026-07-29 | **Yes** | **Borrow** (anim blend + serializer) |
| SFML | zlib (API) | 2026-07-23 | Yes | Skip (2D; audio already covered) |
| Serious Engine 1.10 | GPL-2.0 (API) | 2020-10-31 | No | Ideas only |
| Solarus | GPL-3.0 (GitLab) | 2026-07-30 | No | Ideas only (Lua API shape) |
| Spring → RecoilEngine | GPL-2.0+ (LICENSE) | 2026-07-29 | No | Ideas only (determinism) |
| Torque3D | MIT (LICENSE.md) | 2026-07-28 | **Yes**\* | **Borrow arch** (ghost/scoping netcode) |

\* Permissively licensed but framework-coupled (AZ core / Torque BitStream+SimObject RTTI) — realistic
path is reimplement the pattern, not link/copy modules wholesale.

---

## RANKED SHORTLIST — top 6 concrete borrowings

Prioritized to fill MeatEngine's actual gaps (animation blending, netcode robustness,
cross-platform, asset pipeline). Effort = rough solo-dev hours to a working, tested integration.

| # | Borrowing | Source · License | Gap filled | Effort | License obligation |
|---|-----------|------------------|------------|--------|--------------------|
| 1 | **Snapshot delta compression** — port `Libs/Network/State.*` (baseline + changed-fields diff) into `engine/net/`; diff `SnapshotMsg` against last-acked baseline instead of sending full lists every tick | **Cafu** · MIT | Netcode (bandwidth) | **12–20 h** | Retain MIT notice on ported file(s); add to THIRD_PARTY.md |
| 2 | **Animation blend/state graph** — the #1 player-visible gap. Two routes: (lightweight) integrate **ozz-animation** as a crossfade/blend layer over `Animator::samplePose`, feeding it `SkeletalModel` tracks; (full) port **Esoterica's animation graph** (state machine + blend trees + sync tracks + bone masks + root motion + IK). Start with ozz; graduate to an Esoterica-style graph if AI/locomotion needs it | **ozz-animation** (via ezEngine) or **Esoterica** · both MIT · *alt: `OgreAnimationState.cpp` blend math* | Animation (blend/state machine, IK, root motion) | **16–30 h** (ozz) / 40–70 h (Esoterica graph) | MIT notice; ozz vendored as dep |
| 3 | **Entity scoping + priority replication** — reimplement Torque3D's **scope→prioritize→bandwidth-budget→delta** ghost pipeline on ENet, so the server replicates only relevant entities ranked to a per-packet budget instead of the full `SnapshotMsg` list every tick. The direct fix for scaling past a handful of entities. Pairs with #1 (delta) | **Torque3D** · MIT (reimplement — BitStream/SimObject-coupled) *· Godot `scene_replication_config` as a declarative-field-set companion pattern* | Netcode (interest mgmt + bandwidth) | **24–40 h** | None if clean-room; MIT notice if Torque/Godot code lifted |
| 4 | **Server-side lag compensation** — frame-indexed input + time-rewind for hit registration, modeled on O3DE `NetworkTime`/`NetworkInput`; store a ring of authoritative states and rewind on the server for hitscan | **O3DE** · Apache/MIT (reimplement — AZ-coupled) | Netcode (FPS hit-reg) | **24–40 h** | None if clean-room; Apache/MIT if any snippet lifted |
| 5 | **Cooked native model format** — adopt Ogre's versioned `.mesh`/`.skeleton` serializer as an offline-cook + fast-load path, so runtime stops re-parsing FBX via Assimp | **Ogre3D** · MIT | Asset pipeline (load speed) | **16–24 h** | Retain MIT notice on ported serializer |
| 6 | **Linux port + CI** — finish the existing `#ifdef _WIN32` POSIX branches in `HttpTiny.cpp`/`LanDiscovery.cpp`, add a Linux CMake preset + GitHub Actions job; windowing/input already portable via GLFW. Study **ezEngine `Foundation/Platform`** for the thin abstraction shape | in-house, informed by GLFW (zlib, already vendored) + ezEngine (MIT) | Cross-platform | **10–20 h** | None (own code) |

**Sequencing note:** #1 and #3 are one netcode workstream — Cafu's per-field delta (#1) is the
encoding, Torque's scope/priority (#3) is the "who gets what this tick" selection; build the delta
first, then layer scoping on top (~36–60 h combined). #4 (lag-comp) is independent and can follow.
#2 is the highest player-visible win and is independent of all netcode work — start with ozz. #6 is
low-risk and unblocks the Linux testing hardware already on hand.

**Honorable mentions (below the cut):**
- **Luau script sandbox** (Librebox pattern, MIT) — swap sol2's vanilla Lua for Luau to get read-only
  tables + per-VM memory/instruction budgets before running untrusted user scripts. High value *because
  MeatEngine is public + scriptable*; ~20–30 h. Bumped from the top 6 only because it's security, not a
  stated gap.
- **Secure connect-token handshake** (Cute Framework `cute_net`, zlib/PD) — authenticated UDP connects
  that ENet doesn't provide; ~12–20 h.
- **Nebula frame-graph** (BSD) — data-driven render-pass ordering for the PSX forward pipeline.
- **Irrlicht `CB3DMeshFileLoader`** (zlib) — simple PSX-friendly skeletal format as an Assimp-free path.
- **Ideas-only:** Luanti versioned/compressed mapblock serialization (voxel streaming); Recoil
  synced/unsynced Lua split and Solarus per-entity Lua API (sol2 binding shape); O3DE `AzNetworking`
  declarative auto-packet pattern.

---

## For the reviewer (Codex)

- **Licenses were verified from actual LICENSE files, not GitHub metadata.** Five permissive engines
  (Cafu, O3DE, Nebula, Recoil, Defold) return `NOASSERTION` from GitHub's auto-detector; naive
  `gh api .../license` checks would wrongly exclude the usable ones and wrongly include Defold.
- **Two traps flagged:** (1) **Defold's** Apache lineage is misleading — its §4(a) "no
  commercialising as a Game Engine Product" clause makes it copy-**ineligible specifically for
  MeatEngine**; ideas only. (2) **Luanti's** root LICENSE.txt (CC BY-SA) covers media only; the engine
  code is **LGPL-2.1** per per-file SPDX tags — ideas only.
- **Brief correction:** the `github.com/cafu-engine/cafu` URL 404s. Cafu lives on Bitbucket
  (`bitbucket.org/cafu/cafu`) with a GitHub mirror at `github.com/DNS/Cafu`; it is genuinely MIT
  (relicensed from GPLv3 in 2016) and dormant since 2019.
- **Fork correction:** the maintained Irrlicht is not the SourceForge original or `minetest/irrlicht`
  (archived) but the `irr/` subtree inside `luanti-org/luanti` (zlib, active).
- **Coverage:** all **20 engines** in the source list's C++ section were evaluated (not just the 8
  named in the brief); licenses for every one were verified at source. Torque3D and Esoterica were
  NOASSERTION on the API but plainly MIT on inspection — both are high-value and would have been
  missed by a metadata-only pass.
- **Two more finds worth calling out:** **Torque3D's** ghost/scoping netcode is arguably the single
  best MIT-licensed reference for the "which of N entities do I replicate" problem (now shortlist #3);
  **Esoterica's** animation graph (state machine + IK + root motion, by an ex-IO Interactive animator)
  is the most complete MIT animation runtime found (shortlist #2, full route).
- **Grounding:** recommendations were checked against MeatEngine's actual source. Notably, the brief's
  framing understates the netcode — prediction/reconciliation/interpolation already exist in
  `game/Client.h`; the true netcode gaps are **delta compression**, **entity scoping/interest
  management**, and **lag compensation**. Likewise **FBX loading is already solved by Assimp** — the
  animation gap is the runtime **blend/state graph**, not the importer — and MeatEngine **already has
  an audio stack** (`engine/audio/AudioEngine`), so SFML's audio module is redundant. Effort estimates
  assume a single developer already fluent in the codebase and include test coverage but not design
  iteration.
