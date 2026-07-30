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
- [x] Voxel: chunks, greedy mesher (0fps algorithm, pure/worker-safe), DDA raycast, streaming (agent-built)
- [x] Render: GL wrappers, forward Blinn-Phong, PSX pipeline, shaders (agent-built)
- [x] Physics: Jolt world, chunk colliders, CharacterVirtual controller (agent-built)
- [x] Engine integration: fixed 60 Hz tick, all modules wired, walkable flat world
- [ ] Feel checkpoint #1 — HUMAN PLAYTEST (mouse feel, jump arc, accel)

## Phase 2 — Netcode foundation (before gameplay, so nothing is built wrong)
- [x] Transport interface: LoopbackTransport + EnetTransport (ENet, reliable+unreliable)
- [x] ServerSim / Client split; single-player = in-process listen server over loopback
- [x] Snapshots 20 Hz (full-state MVP), client-side prediction + rewind-replay reconciliation
- [x] Voxel edits as server-validated broadcast ops (remote interp buffers land with player meshes)
- [x] `--host` / `--join <addr>` / `--server` (headless dedicated)
- [x] Two-process smoke test on localhost (host=player 1, joiner=player 2, shared seed)

## Phase 3 — FPS gameplay core (server-authoritative)
- [x] Hitscan weapon (server ray-vs-capsule + voxel DDA, 0.15 s cadence), voxel damage
      (shoot blocks out), RMB block place (anti-entombment check), health/respawn,
      muzzle light pulse (client-predicted), remote players visible w/ 100 ms interp,
      ImGui HUD (HP / players / pos / fps)
- [x] Co-op hit tests server-side; PvP lag compensation deferred to Phase 12
- [ ] Combat feel pass — HUMAN PLAYTEST (fire cadence, hit feedback, flash timing)

## Phase 4 — Inventory + Save/Load
- [x] Slot inventory (36 slots, stacking), THREE dev-selectable models via GameRules
      (hotbar+backpack / grid-only / weapon-slots+counters) + economy flags (finiteAmmo,
      minedBlockDrops); item-driven combat (pistol+ammo, block tool, medkit); server owns
      truth, Inventory msg mirrors to client; hotbar UI (1-9 + scroll) + Tab backpack
- [x] Save/load: JSON save (seed + edit overlay + players w/ inventory), F5 quicksave,
      autosave on graceful exit, `--load <file>`; edit-loss-on-chunk-reload fixed via
      persistent edit overlay
- [ ] World item pickups (ground entities) — lands with the entity-snapshot path in the
      abilities phase

## Phase 5 — Room Designer editor (F1, host/single-player only)
- [x] IEditor interface (engine/core/EditorHost.h): editor injected by main, brushes emit
      VoxelOps through the client→server path — editing replicates in multiplayer
- [x] Free-fly camera (RMB-held look, WASD/QE, Shift, scroll speed), grid snap (1/2/4 voxel)
- [x] Modular tools: place/erase, wall (height param), floor, platform (offset), doorway
      carve; two-click regions w/ preview markers + 4096-op batch cap
- [x] Lights (point/spot) w/ live preview, ImGuizmo translate gizmo, outliner + properties;
      persisted in saves/editor_extras.json and rendered as world lights in game
- [x] Dungeon-seed volumes (A→B + seed), listed/editable, persisted for Phase 6
- [ ] Editor feel pass — HUMAN PLAYTEST (tool ergonomics, picking accuracy, gizmo feel)
- [ ] Ceiling brush + click-to-equip in GridOnly model (minor follow-ups)

## Phase 6 — Procedural dungeons
- [x] v1 core: seeded rooms+corridors carved under the terrain (DungeonGen: rejection
      placement, nearest-chain connectivity + loop edges, 2 stacked levels, vertical
      shafts, surface entrance shaft; SplitMix64 PRNG — cross-platform deterministic;
      chunk-local carving in the shared generator, clients regen from seed, fully
      destructible)
- [ ] v2 template stitching (design locked in docs/BORROWING.md): editor-built rooms as
      templates w/ door sockets, opposite-door mating, layout-graph skeleton
      (TinyKeep-lite; delaunator-cpp MIT if Delaunay wanted), corridor fallback,
      isValidDungeon retry loop; editor seed volumes drive placement (needs volume
      replication in Welcome)
- [ ] Themes/loot rooms; key/lock progression w/ solver acceptance test

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

## Phase 11.5 — Abilities & spawned actors (GAS-lite, see ARCHITECTURE §game/abilities)
- [ ] Effect executors: Damage, AreaDamage (explosives + batch voxel ops), Heal, ApplyModifier
- [ ] Projectiles (server-simulated, explode-on-impact into effects)
- [ ] SpawnEntity behaviors: placeable Turret (target-nearest + LoS), Companion (follow/attack)
- [ ] Lua-defined abilities bound to items (grenade, medkit-as-ability) and to classes
- [ ] Entity states join snapshots (shared path with player states)

## Phase 12 — PvP hardening
- [ ] Lag-compensated hitscan (server rewind), delta-compressed snapshots, interest management
- [ ] 4–8 player arena on editor-built maps
