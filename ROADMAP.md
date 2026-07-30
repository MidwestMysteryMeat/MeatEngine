# Roadmap

Vertical slice goal: host a game, friend joins → spawn in a hand-built room → pick up
weapons + items → shoot (server-authoritative, client-predicted) → enter a procedural
dungeon → manage inventory → save and reload. Single-player is the same flow over a
loopback transport.

## Phase 0 — Scaffold ✅
- [x] Repo, license, docs, CMake + FetchContent (GLFW, glad, glm, Assimp, Jolt, Lua+sol2, ImGui, ImGuizmo, stb, miniaudio, nlohmann-json)
- [x] Dependencies compile on MSVC/Ninja (ImGuizmo layout + CRT fixes)

## Phase 1 — Core loop & modules ⏳
- [x] Core spine: EntityRegistry, JobQueue, EventBus, logging
- [x] Platform: window, raw mouse, PlayerCommand (agent-built, integrated)
- [x] Voxel: chunks, face-culled mesher (pure/worker-safe), DDA raycast, streaming (agent-built)
- [x] Render: GL wrappers, forward Blinn-Phong, PSX pipeline, shaders (agent-built)
- [x] Physics: Jolt world, chunk colliders, CharacterVirtual controller (agent-built)
- [x] Engine integration: fixed 60 Hz tick, all modules wired, walkable flat world
- [ ] Feel checkpoint #1 — HUMAN PLAYTEST (mouse feel, jump arc, accel)

## Phase 2 — Netcode foundation (before gameplay, so nothing is built wrong)
- [ ] Transport interface: LoopbackTransport + EnetTransport (ENet, reliable+unreliable)
- [ ] ServerSim / Client split; single-player = in-process listen server over loopback
- [ ] Snapshots 20 Hz (full-state MVP), client-side prediction + reconciliation for own player
- [ ] Remote entity interpolation (100 ms); voxel edits as broadcast ops
- [ ] `--host` / `--join <addr>` / `--server` (headless dedicated)
- [ ] Two-process smoke test on localhost

## Phase 3 — FPS gameplay core (server-authoritative)
- [ ] Hitscan weapon, voxel damage, health/death/respawn, crosshair, muzzle light pulse
- [ ] Co-op hit tests server-side; PvP lag compensation deferred to Phase 12

## Phase 4 — Inventory + Save/Load
- [ ] Slot inventory, pickups, equip/use, Tab UI; server owns inventory truth
- [ ] Save/load: meta.json + RLE chunks.bin (same encoding as net chunk deltas), F5/F9

## Phase 5 — Room Designer editor (F1, host/single-player only)
- [ ] Free-fly camera, grid snap, modular tools (wall/floor/ceiling/doorway/platform)
- [ ] Lights w/ live preview, outliner, properties, ImGuizmo gizmos
- [ ] Dungeon-seed volumes; save room/world; exit → sensible player spawn

## Phase 6 — Procedural dungeons
- [ ] Seeded rooms+corridors (size range, corridor width, branching, loops, verticality)
- [ ] Room templates/themes; blend with authored areas; workers; clients regen from seed

## Phase 7 — Models & animation
- [ ] Assimp FBX/OBJ/GLB static + PNG/JPG materials
- [ ] Skeletal: canonical Mixamo skeleton, shared clips, one animated NPC; viewmodel idle/fire

## Phase 8 — Scripting
- [ ] Lua (sol2) server-side gameplay: entity spawn, voxel edit, player/inventory/weapons,
      events, dungeon params; weapons/pickups/dummy enemy in assets/scripts/

## Phase 9 — Audio + polish
- [ ] miniaudio: footsteps, gunshot, UI; README/build docs pass

## Phase 10 — Packaging & game-project SDK
- [ ] Game-as-project model: engine exe + `game/` folder (Lua, assets, config) via `--project <dir>`
- [ ] `tools/package` — one command: engine build + project → shippable dist/ zip
- [ ] New-game template (`tools/new_project`) so users import assets, script, and push a game fast

## Phase 11 — Tools
- [ ] tools/autorig: Pinocchio core + Mixamo map CLI
- [ ] tools/audit_assets.py: skeleton/scale/texture/attribution gate; --capture + VLM QA
- [ ] Asset staging from verified CC-BY sources + ATTRIBUTION.md automation

## Phase 12 — PvP hardening
- [ ] Lag-compensated hitscan (server rewind), delta-compressed snapshots, interest management
- [ ] 4–8 player arena on editor-built maps
