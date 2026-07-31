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
- ~~**C9 — Output Log browser (UE5-style)**~~ ✅ — ring buffer in `Log.cpp`; Room Designer
  **Output Log** (All/Messages/Warnings/Errors, search, clear, auto-scroll, double-click copy).
  Captures engine log, ScriptHost failures, node-graph compile.
- **B2 — non-voxel MeshLevel** (Level interface: VoxelWorld vs static-mesh + Jolt MeshShape).
- ~~**B3b — gravity volumes / zoned gravity**~~ ✅ — field + sampling + Space defaults;
  **B3b-e editor volumes** place/save/apply (host/SP). Net sync of custom fields still open.
- ~~**Racer template**~~ ✅ (slice 3) — car + chase cam + predict, 2-car grid, AI pace loop,
  speed HUD, **client lap timer** (pad Z finish). Open: track props, proper car mesh, server
  authority for laps.
- ~~**H4 — Space ship template**~~ ✅ (slice 8) — Fab hulls + station/junkyard; ship cannon;
  New Map genre; AI patrol (lead aim); multi-seat; local-up; **EVA RCS** in low-g;
  **station dock gravity**; ship thrusters/strafe use gravity-local up; hull HP HUD.
- ~~**A2 — directional sun shadow map**~~ ✅ — depth FBO + PCF; chunks/meshes cast; skinned receive.
- ~~**E1 foot-slide**~~ ✅ — walk clip phase rate = worldSpeed / clipSpeed × walkWeight.

## Engine OSS grabs (deferred, low-risk-first)
- **D3 cooked-mesh serializer** (bake FBX→binary; Ogre `MeshSerializer` reference + meshoptimizer).
- **D2 resource archives** (zip/pk3 mounts; feeds packaging/C7).
- **D1 binary greedy mesher** (+AO from its v1 branch) — biggest/riskiest, verify mesh + perf.
- **D4 EnTT** — migrate the registry incrementally behind its API.

## Near-term ordered backlog

**Full research ranking:** [ENGINE_PRIORITY_PLAN.md](ENGINE_PRIORITY_PLAN.md) (highest → lowest need).

### Working list (creation suite track — adjusted)
1. ~~**C9 Output Log**~~ ✅
2. ~~**C6-a reliability**~~ ✅ — compile/import errors open Output Log; editor boot log line.
3. ~~**C6-b high-value nodes**~~ ✅ — announce, damage player, tick / prop count / health.
4. ~~**C5 lite / C4 import path**~~ ✅ — Details panel (world + selection + import); import logs.
5. ~~**A2 sun shadows**~~ ✅ — sun depth map + PCF; editor toggle.
6. **C6-c subgraphs / multi-graph / watches** — after reliability.
7. ~~**B3b editor gravity volumes**~~ ✅
8. ~~**B5 game.json world**~~ ✅ — nested `world` object; see [GAME_JSON.md](GAME_JSON.md).
9. ~~**C6-c multi-graph / watches / subgraphs**~~ ✅
10. ~~**B2 MeshLevel first slice**~~ ✅ — `world.meshLevel` + triangle colliders.
11. ~~**C7 packaging**~~ ✅ — `tools/package.ps1` launchers + zip.
12. ~~**B2 multi-mesh + C2/C3 drag-place**~~ ✅ — `meshLevels[]`, content drag-to-viewport.
13. ~~**ARCHITECTURE reconcile + B3-sky**~~ ✅ — status banner + key sections; procedural sky.
14. **F netcode** / **D OSS** / real asset thumbnails / water plane — as needed.

## Cleanup / debt from last sprint
- ~~**ARCHITECTURE.md is stale**~~ ✅ (partial) — top status + layout + lighting + nodegraph/AI notes.
- **Root `ROADMAP.md`** still historical; points at ROLLOUT/NEXT_SESSION.
- **E1 foot-slide** — verify shipped; re-open only if feel regresses.
