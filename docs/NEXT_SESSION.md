# MeatEngine — Next Sprint Plan

Picks up after the "creation suite" sprint. Full pillar list + license-clean OSS per item lives in
[ROLLOUT.md](ROLLOUT.md); this is the focused, ordered next-sprint slice. Working discipline unchanged
(serial tree edits or sole-builder agents; VLM-gate visuals; small verified commits; no AI attribution).

## Where we are
The engine is now a UE5-style creation suite: **world templates** (Normal/Superflat/Void) ×
**environments** (Surface/Underwater/Space), a **content browser** + dark theme, **drag-drop asset
placement** with server-authoritative collision/MP-sync/save, **voxel AO** lighting, a proper
**weapon system** (fire modes + magazines), on top of the ozz animation + foot-IK + auto-rig +
PSX/netcode/worldgen work. README + THIRD_PARTY + repo description are current.

## Sprint goals (in order)

1. ~~**Game templates — perspective option (H1, first slice)**~~ ✅ — `GameRules::Perspective`
   (`first`/`third`), `game.json "template"` (`fps`/`tps`) + `"perspective"`, CLI `--template` /
   `--perspective`. Third-person over-shoulder cam with physics raycast pullback; crosshair gated
   to first-person only.
2. ~~**A3 — hemisphere ambient (toggle)**~~ ✅ — sky/ground hemi ambient outside the block-light
   gate; env presets set strength (Surface on, Space off); `game.json "hemisphereAmbient"`, F7,
   editor checkbox. Dark PSX-night still available when toggled off.
3. ~~**Prop authoring polish**~~ ✅ — `MovePropMsg` on gizmo release; outliner / Del key send
   `RemovePropMsg`; server rebuilds colliders / deletes and broadcasts; client treats re-`PropAdded`
   as transform update.
4. ~~**New Map dialog (B4)**~~ ✅ — editor "New Map..." modal: template (Normal/Superflat/Void) +
   environment (Surface/Underwater/Space) + seed; `ServerSim::reseedWorld` + client mesh clear.
5. ~~**Networked world-config sync**~~ ✅ — `Welcome` carries `voxelSize` + `environment` (terrain was
   already in flags). Join clients apply host scale before world gen; spawn height scales with
   `kVoxelSize` (voxel cell 16,16,16).

## Stretch / bigger pieces (pull in as the sprint allows)
- ~~**C6 — node scripting / Node graphs + live coding**~~ ✅ (slice 2) — UE5-Slate editor theme;
  imnodes graph with RMB place menu, double-click/F open Node Details, object highlight from
  Outliner↔Get World Object; compiles to sandboxed Lua + live reload. Open: more API, subgraphs.
- **C9 — Output Log browser (UE5-style)** ⭐ *planned* — in-editor panel that mirrors UE's
  **Output Log**: ring buffer of engine/script messages with **verbosity filters** (All /
  Messages / **Warnings** / **Errors**), search/filter text, clear, auto-scroll, click-to-copy.
  Must capture `meat::log` (info/warn/error), ScriptHost Lua failures (`script: … failed`),
  node-graph compile/reload status, and optional `[lua]` `game.log` lines. Hook: ring sink in
  `engine/core/Log.h` → ImGui "Output Log" window in Room Designer (and optionally always-on
  with `~` / Window menu). Without this, graph authors cannot catch graph/Lua errors the
  way UE5 does. **Do next after C6 API nodes** (or in parallel with ScriptFx polish).
- **B2 — non-voxel MeshLevel** (Level interface: VoxelWorld vs static-mesh + Jolt MeshShape).
- ~~**B3b — gravity volumes / zoned gravity**~~ ✅ (first slice) — `GravityField` with base + AABB
  volumes + radial orbital bodies; CharacterController samples full gravity vector each tick;
  Space env seeds habitat box + planetoid; editor-authored volumes / net sync of custom fields later.
- ~~**Racer template**~~ ✅ (slice 3) — car + chase cam + predict, 2-car grid, AI pace loop,
  speed HUD, **client lap timer** (pad Z finish). Open: track props, proper car mesh, server
  authority for laps.
- ~~**H4 — Space ship template**~~ ✅ (slice 8) — Fab hulls + station/junkyard; ship cannon;
  New Map genre; AI patrol (lead aim); multi-seat; local-up; **EVA RCS** in low-g;
  **station dock gravity**; ship thrusters/strafe use gravity-local up; hull HP HUD.
- **A2 — directional sun shadow map** (McNopper MIT; +LiSPSM later).
- ~~**E1 foot-slide**~~ ✅ — walk clip phase rate = worldSpeed / clipSpeed × walkWeight.

## Engine OSS grabs (deferred, low-risk-first)
- **D3 cooked-mesh serializer** (bake FBX→binary; Ogre `MeshSerializer` reference + meshoptimizer).
- **D2 resource archives** (zip/pk3 mounts; feeds packaging/C7).
- **D1 binary greedy mesher** (+AO from its v1 branch) — biggest/riskiest, verify mesh + perf.
- **D4 EnTT** — migrate the registry incrementally behind its API.

## Near-term ordered backlog

**Full research ranking:** [ENGINE_PRIORITY_PLAN.md](ENGINE_PRIORITY_PLAN.md) (highest → lowest need).

### Working list (creation suite track — adjusted)
1. ⭐ **C9 Output Log** — UE5 Output Log panel (errors/warnings/messages). **Promote above more C6 features.**
2. **C6-a reliability** — compile/reload errors → log; ScriptFx + many API nodes already shipped (`3c9920d`).
3. **C6-b high-value nodes only** — finish hooks creators need, not infinite catalog.
4. **C5 lite / C4** — prop-env inspectors + import dialog (suite ergonomics).
5. ⭐ **A2 sun shadows** — biggest remaining visual lift.
6. **C6-c subgraphs / multi-graph / watches** — *after* log + reliability (demoted).
7. **B2 MeshLevel / B3b editor volumes / B5 game.json** — as capacity allows.
8. H4/H1 polish, C7 packaging, F/D pillars — later (see full plan).

## Cleanup / debt from last sprint
- **ARCHITECTURE.md is stale** — still frames delta-compression, navmesh, torch-light, ozz as
  "planned" when shipped; do a reconciliation pass (design contract, so review carefully).
- **Root `ROADMAP.md`** duplicates `docs/ROLLOUT.md` — retire or cross-link to a single source.
- **E1 foot-slide** ("too fluid") — tie walk-clip playback to move speed (Torque3D pattern); the last
  open item from the animation overhaul.
