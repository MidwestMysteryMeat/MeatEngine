# MeatEngine Implementation Roadmap

Single source of truth for sequenced work. Ordering = value × readiness, on-theme first,
dependencies respected. Each item names the license-clean OSS we lean on (verified in
docs/ENGINE_REUSE_SURVEY.md + the memory OSS-grabs reference) and its verification gate.

## Working discipline (every item)
1. **Research first** — read the OSS reference (technique, not blind copy); confirm the license
   allows a copy vs ideas-only (MIT/BSD/Apache/zlib to vendor; GPL/no-license = reference only).
   Cite file:line provenance in code comments.
2. **Small, reviewable commits** — one concern per commit; ARCHITECTURE.md contract stays sacred;
   match surrounding style; no dead code; no AI attribution in commits.
3. **Verify, don't assume** — build clean (no new warnings), run the smoke/MP test, and for anything
   visual **gate on the R720 qwen3vl VLM** (never self-assess). Report the real result.
4. **Debug empirically** — probe (log/marker/isolate) before guessing.
5. **Serialize the tree** — one workstream editing `src/` at a time; commit before the next. Agents
   only ever run read-only research or edit disjoint new files with no git/build. (Learned the hard
   way: concurrent agents on the shared tree checkout-revert each other's uncommitted work.)

---

## ✅ Done (foundation + recent)
- **World/render**: chunked greedy-meshed voxel world (0fps), PSX low-res target + dither + posterize
  + vertex fog, atlas materials, torch block-light flood-fill (0..15), point/spot/dir Blinn-Phong.
- **PSX look** *(2026-07-31)*: vertex snapping + affine (noperspective) texture mapping on
  chunk/mesh/skinned shaders — the two signatures that were missing.
- **Worldgen** *(2026-07-31)*: FastNoiseLite OpenSimplex2 + FBm terrain (deterministic), dungeon gen.
- **Netcode**: delta-compressed snapshots + ack (~97% smaller); **16-bit position quantization**
  *(2026-07-31, MP-verified)*. Host-authoritative, ENet, LAN + master browse.
- **Physics/AI**: Jolt character controller, Recast/Detour navmesh + A* fallback, GAS-lite abilities.
- **Animation** *(2026-07-30/31, the big overhaul)*: skeletal core (offset-authoritative delta),
  cross-skeleton retarget, clip-merge, idle↔walk blend (raw clip-key space), **server-authoritative
  walk weight** (fixed frozen NPCs), **foot-curve grounding**, **deterministic facing** (foot→toe
  geometry), **NpcZombie + armed NpcShooter**, and **ozz-animation vendored + two-bone foot IK**
  (terrain foot planting, VLM-verified no-regression). Auto-rig Blender tool (`tools/autorig/`).
- **Platform**: Linux build + CI (Linux hw unverified), positional 3D audio, Lua+sol2 scripting,
  ImGui room editor + ImGuizmo.

---

## Pillar A — Lighting overhaul  *(active)*
The single biggest visual lift. Do in this order:
- [ ] **A1. Voxel ambient occlusion** — per-vertex corner darkening baked in the greedy mesher (AO
  joins the merge key so per-voxel detail survives; 3-neighbour occlusion, 0fps.net algorithm,
  clean-room). New `ao` vertex attrib; shader multiplies lighting by it. *The defining voxel-lighting
  feature and cheapest.* Gate: VLM sees concave corners/edges darkened, flat faces unaffected.
- [ ] **A2. Directional (sun) shadow map** — one depth pass from the sun + PCF sample in the frag.
  Low-res/hard edges are on-aesthetic for PSX. Add a day/sun option. OSS: McNopper/OpenGL Example28
  (MIT) shadow GLSL, clean-room. Later: LiSPSM/PSSM for depth-resolution (Ogre reference only).
- [ ] **A3. Better ambient / hemisphere term** — the night scenes are near-black; a sky/ground
  hemisphere ambient lifts readability without flattening. Cheap frag change.
- [ ] **A4. SSAO (optional)** — screen-space contact AO in the resolve chain. OSS: McNopper (MIT) /
  lettier 3d-game-shaders (BSD-3). More cost; after A1–A3.
- [ ] **A5. Colored block-light + emissive polish** — extend the torch flood-fill to RGB; night
  scenes read by torch color. Reference: VektorKnight/vektor-voxels (MIT), 0fps (technique).
- [ ] **A6. Character shadows for night** — cheap projected/blob shadow under NPCs (point-light
  shadow cubes are too costly for many torches). Pragmatic pairing with A2.

## Pillar B — Engine generality (voxel-optional)
*Answer to "is forcing voxel obtuse?": voxel-FIRST, not voxel-ONLY.* The renderer already draws
static meshes + skinned characters (Assimp + ozz) and Jolt handles arbitrary collision — so the
engine is closer to general than it looks. Making non-voxel games a first-class choice:
- [ ] **B1. `Level`/`World` abstraction** — an interface with two implementations: `VoxelWorld`
  (current) and `MeshLevel` (a static-mesh scene). Engine/ServerSim talk to the interface.
- [ ] **B2. Static-mesh level path** — load a level as meshes + a Jolt `MeshShape` for collision
  (survey grab: Jolt's under-used `MeshShape`/`CharacterVirtual`); worldgen/navmesh become optional.
- [ ] **B3. Project flag** — `game.json` picks `world: voxel | mesh`; a dev ships a non-voxel game
  with zero C++. Keeps the PSX voxel identity as the *default*, not a cage.

## Pillar C — Editor / GUI overhaul
From "a game" toward "a tool others use." All ImGui add-ons below are MIT.
- [ ] **C1. Content/asset browser** — a dockable panel listing project assets (models/textures/
  scripts/prefabs) with thumbnails + drag-to-place. OSS: ImGuiFileDialog.
- [ ] **C2. Asset import/export** — import FBX/OBJ/PNG into the project (bake to the cooked format,
  see D3), export levels/prefabs. Ties to B + D.
- [ ] **C3. Inspectors** — material editor (drives C4), entity/prefab inspector, block/atlas editor.
  Struct-reflection via Boost.PFR (BSL-1.0) → auto ImGui widgets + JSON.
- [ ] **C4. Visual node graphs → Lua** — weapon/ability/behaviour/material graphs. OSS: imnodes.
- [ ] **C5. Script editor** — in-editor Lua editing w/ syntax highlight. OSS: ImGuiColorTextEdit.
- [ ] **C6. Packaging / build export** — "export game" that bundles the exe + project assets (cooked)
  + a zip/pk3 archive into a shippable folder. Ties to D2 (resource archives).
- [ ] **C7. Profiler panels** — frame/mesh/netcode telemetry. OSS: ImPlot + Tracy (BSD-3).

## Pillar D — Engine OSS grabs (from the survey)
- [ ] **D1. Binary greedy meshing** — swap the mesher hot path; graft the v1-branch per-vertex AO
  (pairs with A1). OSS: cgerikj/binary-greedy-meshing (MIT). *Biggest/riskiest — verify mesh
  correctness + perf before/after.*
- [ ] **D2. Resource system** — ResourceGroup + **zip/pk3 archive mounts** + background loading
  (pattern from Ogre `ResourceGroupManager`, MIT reference; implement clean). Feeds C6 packaging.
- [ ] **D3. Cooked mesh serializer** — bake FBX→binary once, load fast at runtime (vs runtime Assimp
  every load). OSS: Ogre `MeshSerializer` (MIT reference), meshoptimizer (MIT) for vertex/index
  optimization + quantization.
- [ ] **D4. EnTT ECS** — migrate the hand-rolled EntityRegistry incrementally behind its API. OSS:
  EnTT (MIT, vendored). Big; do behind the existing interface, one system at a time.
- [ ] **D5. Material scripts** — data-driven `.material` files instead of hardcoded shader setup
  (Ogre pattern, MIT reference). Feeds C3. Lower urgency given the fixed PSX look.

## Pillar E — Animation polish
- [ ] **E1. Foot-slide fix** ("too fluid") — tie walk-clip playback to move speed (Torque3D
  `dot(vel,dir)/clipSpeed`, MIT) or distance-phased gait. *Last unfixed original NPC complaint.*
- [ ] **E2. Pelvis-lower** for downslopes in the foot-IK; **E3. weapon aim** via ozz `IKAimJob`
  (shooter aims pistol at target); **E4. additive layers / state machine**; **E5. pose/morph faces**
  (ozz is skeletal-only — Ogre `VertexAnimationTrack` reference for facial morphs, later).

## Pillar F — Netcode hardening (PvP)
- [ ] **F1. Interest management** (Torque3D scope→priority→delta, MIT); **F2. lag-compensated
  hitscan** (O3DE NetworkTime pattern). **F3. Full bit-packed codec** with bitsery (vendored) when
  the delta grows past the surgical 16-bit quantization.

## Pillar G — Systems depth (from the original roll, still open)
- [ ] **G1. Destruction depth** — reinforced blocks, radial voxel damage, structural collapse.
- [ ] **G2. Game-mode framework** (Breach/Horde) — WIP stashed; re-dispatch serially.
- [ ] **G3. Lua-bind abilities**; **G4. per-model embedded textures / PSX env staging** (Roll-1 items).

## Not automatable
Human feel-playtests (mouse feel, combat cadence, editor ergonomics), and real Linux-hardware CI.

---

## Recommended near-term order
A1 (voxel AO) → A2/A3 (sun shadow + ambient) → E1 (foot-slide) → C1+C2 (content browser + import)
→ D3 (cooked mesh) → B1–B3 (voxel-optional) → D1 (binary mesher) → C4–C6 (node graphs + packaging)
→ D4 (EnTT). Lighting first (visible payoff), then the editor/generality push (turns the engine into
a tool), with the risky hot-path swaps (D1, D4) deliberately late.
