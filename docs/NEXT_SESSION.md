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

1. **Game templates — perspective option (H1, first slice)** ⭐ — the FPS↔TPS split. Add a
   camera `perspective: first | third` to the config (`game.json "template"` presets it: `fps`→first,
   `tps`→third) and a third-person over-shoulder camera in `Engine::render` (offset behind/above the
   player + collision-aware pullback). Gate the FPS HUD/crosshair on it. First concrete proof that the
   engine isn't first-person-only. *Files:* GameRules (template enum), Engine (camera), main.cpp.
2. **A3 — hemisphere ambient (toggle)** — surface the voxel AO world-wide by adding a sky/ground
   hemisphere ambient term that is NOT gated by the block-light floor. Make it an **environment/game
   setting** (don't force it — the dark PSX-night look stays available). Pairs with A1. *Files:*
   chunk.frag ambient, Renderer/Environment.
3. **Prop authoring polish** — the two documented deferrals from prop-sync: (a) **gizmo-move-after-
   place sync** (send a MoveProp intent when the gizmo edits a synced prop) and (b) **Delete sync**
   (`RemovePropMsg` is already wired end-to-end — just emit it from the outliner Delete). Makes the
   placement loop fully authoritative. *Files:* Messages (MoveProp), ServerSim, Engine, RoomEditor.
4. **New Map dialog (B4)** — an editor dialog to pick template + environment + seed and (re)create the
   world, tying templates/environments into a UE5-style flow instead of CLI flags. *Files:* RoomEditor,
   an engine hook to rebuild the world.
5. **Networked world-config sync** — pack `terrain` + `environment` (and revisit `voxelSize`) into
   `Welcome` so a networked joiner gets the host's world/gravity/fog instead of defaulting to
   Surface/Normal (the documented same-config limitation). *Files:* Messages (Welcome), Client, ServerSim.

## Stretch / bigger pieces (pull in as the sprint allows)
- **C6 — node scripting / blueprints + live coding** (imnodes → Lua; the no-code layer + edit-live).
- **B2 — non-voxel MeshLevel** (Level interface: VoxelWorld vs static-mesh + Jolt MeshShape).
- **B3b — gravity volumes / zoned gravity** (0-g void vs ship interiors → ship builders).
- **Racer template** (vehicle controller + chase cam) — the second template after TPS.
- **A2 — directional sun shadow map** (McNopper MIT; +LiSPSM later).

## Engine OSS grabs (deferred, low-risk-first)
- **D3 cooked-mesh serializer** (bake FBX→binary; Ogre `MeshSerializer` reference + meshoptimizer).
- **D2 resource archives** (zip/pk3 mounts; feeds packaging/C7).
- **D1 binary greedy mesher** (+AO from its v1 branch) — biggest/riskiest, verify mesh + perf.
- **D4 EnTT** — migrate the registry incrementally behind its API.

## Cleanup / debt from last sprint
- **ARCHITECTURE.md is stale** — still frames delta-compression, navmesh, torch-light, ozz as
  "planned" when shipped; do a reconciliation pass (design contract, so review carefully).
- **Root `ROADMAP.md`** duplicates `docs/ROLLOUT.md` — retire or cross-link to a single source.
- **E1 foot-slide** ("too fluid") — tie walk-clip playback to move speed (Torque3D pattern); the last
  open item from the animation overhaul.
