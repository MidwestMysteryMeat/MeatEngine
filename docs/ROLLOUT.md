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

> See [NEXT_SESSION.md](NEXT_SESSION.md) for the current sprint plan.

## ✅ Done (foundation + recent)
- **Creation suite** *(this sprint)*: world templates (Normal/Superflat/Void) × environment presets
  (Surface/Underwater/Space — gravity+fog+ambient); editor **content browser** + UE5-style tile grid +
  dark theme; **drag-drop asset placement** with **server-authoritative props** (Jolt box collision,
  MP-synced, saved); **weapon fire modes** (SemiAuto trigger-discipline / Auto / Burst) + magazines
  (mag/reserve/reload, HUD); **per-vertex voxel ambient occlusion**.
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

## Pillar B — World creation: templates + environments (UE5-style "New Map")
The dev picks a starting **template**, then hand-authors/fills it — like UE5's New Level (landscape /
blank / …), but spanning voxel AND non-voxel AND themed worlds. Physics is already Jolt/world-agnostic
(it just consumes collider meshes), so this is bounded, not a rewrite. Voxel is the *default*, not a cage.
- [x] **B1. Terrain modes** — Normal / Superflat / Void (`game.json "terrain"` or `--terrain`),
  synced to clients in the rules flags byte; Void gets a spawn pad so you don't fall through.
- [ ] **B2. `Level` abstraction** — a `Level` interface: `VoxelWorld` (current) + `MeshLevel`
  (static mesh + Jolt `MeshShape` collider). Worldgen/navmesh become optional; the client renders the
  level mesh instead of chunks. True non-voxel games.
- [ ] **B3. Environment presets** — per-world **gravity + fog + ambient + skybox + water level**, so
  a dev builds a **space** game (low/zero gravity, black void, no fog, starfield) or an **underwater**
  game (buoyant gravity, blue fog, caustic ambient, water plane). Composes with any template. *(Scoped:
  base gravity = `PhysicsWorld::SetGravity` + controller `t.gravity`; fog/ambient = `PsxOptions` +
  `setAmbientLight` — all small, bounded hooks.)*
- [ ] **B3b. Gravity volumes / zoned gravity** ⭐ — gravity is a *field*, not a global: per-region
  volumes (0-g in open space, normal inside a ship, radial "planet" gravity). The controller samples
  the active volume's gravity vector each tick. Unlocks **space games**, **ship interiors vs. void**,
  and **ship builders** (build a voxel/mesh craft that carries its own g-field). Big but on-vision.
- [ ] **B4. "New Map" dialog** (editor) — pick template (Landscape / Superflat / Blank-Void / Mesh) +
  environment (Surface / Underwater / Space / custom) + name + seed, then create and fill it.
- [ ] **B5. `game.json` `world: {template, environment}`** so a project ships its map choice; no C++.

## Pillar C — Editor: a usable, UE5-inspired creation suite (USABILITY FIRST)
The through-line is **usability** — UE5's ergonomics (New Map → drag assets in → tweak → script →
light → ship) without the bloat/missing pieces that don't fit a PSX voxel FPS. All ImGui add-ons MIT.
- [x] **C1. Content browser** — recursive `assets/` scan, grouped + filtered + selectable + details.
- [ ] **C2. Content-browser styling + editor theme** — UE5-style thumbnail **grid**, folders +
  breadcrumbs, per-type icons, drag-source handles; a polished dark editor theme (one ImGui style
  pass) applied engine-wide. This is the "feels like a real tool" pass.
- [ ] **C3. Drag-drop asset placement** ⭐ — drag an asset from the browser into the viewport to place
  it (raycast to surface), then move/rotate/scale with the existing **ImGuizmo** gizmos; a **world
  outliner** of placed instances. This is the core "fill the map" loop and the highest-usability win.
- [ ] **C4. Import/export** — import FBX/OBJ/PNG into the project (bake, see D3); export map/prefab.
  OSS: ImGuiFileDialog.
- [ ] **C5. Inspectors** — material, entity/prefab, block/atlas, and **environment settings** (drives
  B3). Struct-reflection via Boost.PFR (BSL-1.0) → auto widgets + JSON.
- [ ] **C6. Node scripting / Blueprints + live coding** ⭐ — visual graphs (imnodes) for behaviour/
  weapon/ability/material, compiled to Lua; paired with an in-editor Lua script editor
  (ImGuiColorTextEdit). **Live coding** is the killer UE5 ergonomic: Lua already hot-reloads
  (`loadDir`) + shaders reload on F6 — extend to **edit-a-blueprint/script-and-see-it-live** while the
  game runs (no restart). C++ live-reload for engine systems is a later stretch (OSS: cr.h, MIT).
- [ ] **C7. Packaging / export** — bundle exe + cooked assets + a zip/pk3 archive into a shippable
  build (ties to D2 resource archives + `tools/package.ps1`).
- [ ] **C8. Profiler panels** — frame/mesh/netcode telemetry. OSS: ImPlot + Tracy (BSD-3).

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

## Pillar H — Genre & weapons (FPS is OPTIONAL)
The engine must not force "FPS shooter." A project picks its genre/game-mode; the FPS/combat layer
(guns, hotbar, hitscan, enemy NPCs) is a MODULE a game opts into, so a space ship-builder or an
underwater explorer needn't ship guns. Composes with world templates (B) + environments (B3).
- [ ] **H1. Game templates (UE5-style)** ⭐ — a project picks a TEMPLATE that presets camera +
  controls + core mechanics, like UE5's FPS/TPS/Racer/etc. project templates: **FPS** (first-person
  cam, gun module — today's default), **TPS** (third-person over-shoulder cam, same combat), **Racer**
  (vehicle controller + chase cam, no guns), and room to add more (top-down, platformer, sandbox/
  builder). `game.json "template"` + a New-Map choice (B4). The camera mode (first/third/chase) + the
  active control+mechanic module are gated on it; combat/HUD/loadout become opt-in so a Racer or a
  builder ships no guns. Default `fps` so nothing regresses. First tractable slice: a **perspective
  option** (first- vs third-person camera) — the FPS↔TPS split — then the vehicle/chase (Racer).
- [ ] **H2. Weapon fire modes** ⭐ — **Semi / Auto / Burst / Shotgun that FEEL distinct.** Semi-auto
  **requires a trigger release between shots** (server tracks the fire-press EDGE — holding fire does
  NOT auto-repeat a semi); Auto fires at the weapon's cadence while held; Burst = N rounds per press;
  Shotgun = multiple pellets with spread per shot. Per-weapon `fireMode` + `pelletCount` + `spread`
  in `ItemDef`; the client must send a press-edge/held bit so the server can gate semi.
- [ ] **H3. Ammo + magazines** — per-weapon `magSize` + reserve ammo; a shot consumes a round, an
  empty mag blocks fire, **reload (R)** refills from reserve on a timer; HUD shows mag/reserve. Extends
  the existing `finiteAmmo` rule, which becomes the on/off switch for the whole ammo model.

## Not automatable
Human feel-playtests (mouse feel, combat cadence, editor ergonomics), and real Linux-hardware CI.

---

## Recommended near-term order  *(usability-first, per the UE5-style creation-suite vision)*
**C3 drag-drop asset placement + world outliner** ⭐ (the core "fill the map" loop) → **C2 content-
browser styling + editor theme** (the UE5 feel) → **B3 environments** (space / underwater) + **B4 New
Map dialog** → **A1 voxel AO + A2 sun shadow** (lighting payoff) → **C5 inspectors** → **C6 node
scripting / blueprints** → **B2 non-voxel mesh levels** → **C4/C7 import-export + packaging** → the
engine OSS grabs (**D** — cooked mesh, binary mesher, EnTT) and anim polish (**E1 foot-slide**) folded
in as they unblock. Rationale: make it *feel* like a tool a dev wants to build in (place, style, light)
before the deeper generality (mesh levels) and the risky hot-path swaps (mesher, ECS) that come late.
