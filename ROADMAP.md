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

## Phase 6.95 — Session discovery ✅ (server lists / LAN / host anywhere)
- [x] LAN: UDP broadcast beacon (1 Hz, MEAT/v1 wire) + browser-side discovery w/ 5 s TTL
- [x] Master server: self-hostable tools/master_server.py (announce/list, source-IP
      authoritative, 60 s TTL) + HttpTiny client + 30 s host heartbeat thread
- [x] Server-browser main menu (no-args launch): Singleplayer / Host / live LAN list /
      master list w/ refresh / direct ip:port; CLI flags bypass (--play/--host/--join/
      --server/--name/--master)
- [x] Verified: beacon on the wire (raw UDP listener), master round trip (host announce
      → HTTP /servers lists it), menu stability
- [ ] Reachability diagnostics + access control (Phase 11.7), UPnP/punch/Steam providers

## Phase 7 — Models & animation
- [x] 7a static: Assimp FBX/OBJ/GLB loader → existing mesh pipeline (node-baked transforms,
      normal gen, scale/skeleton probe w/ cm→m warning, albedo discovery); world props;
      self-authored OBJ smoke prop committed, vault binaries gitignored (license-pending)
- [x] TEXTURE FIX (found via new VLM screenshot gate): chunk atlas sampled the wrong
      row (load-flip vs authored top-left) — ALL terrain was debug-magenta since phase 1;
      now correct grass/dirt/stone
- [x] Screenshot tooling: Renderer::captureScreenshot (glReadPixels→PNG), F12 key,
      `--shot <png>` auto-capture-and-exit — enables visual validation of each step
- [x] 7b skeletal WORKING (VLM-verified): loader parses Mixamo rigs (65 bones/40k verts),
      pose sampler (LERP/SLERP), skinned render path + shader, upright auto-fit. Bind-pose
      skinning FIXED — bind globals derived from inverse(offset) (skin-deformer-authoritative)
      instead of the node chain, which drifted ~100x on the armature's baked FBX unit scale;
      now skinning≡I at bind for all weighted bones. SWAT operator renders as a clean T-pose.
- [x] VLM CAPTURE BOOTH: --animshot locks a fixed close, lit camera on the character so
      the R720 gate sees it clearly (the SIE-booth idea). CRITICAL: the earlier distant
      auto-spawn camera was too far to see mesh spikes, so "verified" grades were false.
- [x] Static skeletal character CLEAN (booth-VLM ok): bind pose renders as a recognizable
      armored humanoid — skinning is correct. idlePose currently returns bind pose (honest).
- [x] ANIMATION FIXED (commit 313736c, R720 qwen3vl-verified): clip playback is shear-free.
      Root cause (2 OSS-engine research agents + empirical probes vs Ogre/Irrlicht/ozz): the
      loader's inverse(offset) bind reconstruction put localBind in a different space than the
      node-native animation keys — a 179-unit gap at the wrist flung extremities into spikes.
      Fix (Ogre reset-to-bind + delta): keep the offset-authoritative clean bind, store each
      bone's raw nodeBindLocal, apply keys as a delta: local = localBind * nodeBindLocalInv *
      animatedLocal (identity at bind ⇒ clean bind preserved). Ruled out the naive raw-node-
      chain + aiProcess_GlobalScale approach empirically (still exploded ~100x; the unit scale
      lives on the SWAT MESH node with an identity scene root, so nothing cancels it).
- [x] Real multi-second clip playback PROVEN: booth-loaded an external 12s raptoid glTF (112
      bones, 4 clips) via --animmodel; R720 graded 3/3 frames clean with visible motion (head
      elevated→mid-height across frames). --animmodel <path> booth-checks any FBX/glTF.
- [ ] Author a mixamorig locomotion clip for the SWAT (its shipped clip is a 1-tick static
      reference pose); procedural sway idle (localBind * deltaQuat — now shear-free); replace
      NPC/remote boxes with animated meshes; first-person viewmodel

## Phase 8 — Scripting
- [x] Lua (sol2) server-side host: sandboxed stdlib (base/math/table/string only — no
      io/os/package/debug), capability `game` table (log/set_block/get_block/spawn_pickup/
      player_count/randi/tick/item_id), hooks (on_init/on_player_join/on_player_death/
      on_tick @ 3 Hz), protected calls (script errors logged, never crash server),
      deterministic seeded RNG, pimpl (sol2 headers don't leak); example.lua verified live
- [ ] Expand API (player state/inventory/weapons, dungeon params, world events); wire the
      no-code authoring/Design panel to emit Lua defs (Phase 8.6)

## Phase 8.7 — Visual node scripting (ARCHITECTURE §editor/nodegraph) — NOT built
- [ ] ImGui node-graph panel (ImNodes MIT or hand-rolled): event nodes → action nodes
- [ ] Graph saved per-project as JSON; COMPILES TO LUA (reuses ScriptHost sandbox/budget,
      no second VM); drop-to-code path; slots in as an editor panel

## Phase 8.6 — Authoring & modeling (ARCHITECTURE §game/authoring, §game/modeling)
- [ ] Effect-composition core: EffectList on items/abilities, server executors
      (Damage/AreaDamage/Heal/ApplyModifier/Ignite/Knockback/Slow/Chain/SpawnEntity)
- [ ] In-editor "Design" panel: no-code weapon/ability/item authoring → project data
      defs, live-test, optional power-budget balance scorer
- [ ] Lua authoring path (same defs as tables; custom effects as functions)
- [ ] Voxel/primitive object modeler: shape library (box/cyl/sphere/cone/wedge/torus/
      lathe/extrude), re-editable shape nodes (scale/taper/bevel/boolean), color/material
      palette, mirror, → object prefab; object CLASS (Prop/Item/Melee/Gun/Ranged/
      Throwable/Deployable/Wearable/Ammo/Block) drives Design-panel fields + behavior
- [ ] Modular part-based weapons: parts (body/barrel/grip/sight/…) w/ sockets +
      stat/effect modifiers, snap-assembly → concrete ItemDef, seeded loot generation,
      runtime attachments (suppressor/scope) — one socket system shared with dungeons

## Cross-cutting: Developer UX bar (ARCHITECTURE §Developer UX)
- [ ] Dockable single-window panel layout (persisted), command palette, tooltips/first-run
- [ ] "Play here" spawn-at-editor-camera; hot-reload of scripts/assets/authoring into live world
- [ ] Actionable errors everywhere (import/rig/balance/reachability say why)
- [ ] New-project template that already runs; every authored thing starts from an example

## Phase 8.5 — Editor IDE panels (ARCHITECTURE §editor/ IDE)
- [x] In-engine IDE (VLM-verified): Assets browser (cached dir tree over assets/) + Code
      editor (InputTextMultiline w/ resize-callback, dirty flag) in the Room Designer.
      Edit Lua → Save → writes file + hot-reloads into the running server (ScriptHost::
      reload → ServerSim::reloadScripts via EditorContext file/reload callbacks). No alt-tab.
- [ ] Syntax highlighting (ImGuiColorTextEdit), Lua console/REPL, texture/model previews,
      file-watcher refresh
- [x] Import GUI (VLM-verified): asset-browser Import button + path field + OS drag-drop
      (glfwSetDropCallback) for FBX/OBJ/GLB/PNG/JPG — validates on import (loadStaticModel/
      loadSkeletalModel parse + bounds/bone probe w/ scale warning, stbi_info decode check),
      copies into assets/, status line, auto-refreshes tree. Manifest/hash/attribution-gate +
      orbit previews + rig-conformance report are the follow-on.

## Phase 9 — Audio + polish
- [x] Audio (miniaudio): AudioEngine w/ 6 PROCEDURALLY-SYNTHESIZED sounds (gunshot,
      footstep, pickup, hit, explosion, ui-click — no bundled files, license-clean),
      voice-pool overlap, best-effort init (silent if no device); wired to fire/movement/
      damage/inventory/UI from client-side state deltas; pimpl (miniaudio out of headers)
- [ ] Positional 3D audio (remote gunshots/footsteps by distance), music, README polish

## Phase 10 — Packaging & game-project SDK ✅
- [x] Game-as-project model: `--project <dir>` loads game.json (name/seed/GameRules) +
      scripts/ (per-project Lua) + assets/; composable with --play/--host/--server
- [x] tools/new_project.py — scaffolds a runnable game (game.json, scripts/main.lua, README)
- [x] tools/package.ps1 — bundles exe + engine assets + project + Play.bat → dist/ + zip;
      runs standalone. Verified: scaffold → run → package → 15.9 MB shippable zip that boots

## Phase 11 — Tools
- [ ] tools/autorig: Pinocchio core + Mixamo map CLI
- [ ] tools/audit_assets.py: skeleton/scale/texture/attribution gate; --capture + VLM QA
- [ ] Asset staging from verified CC-BY sources + ATTRIBUTION.md automation

## Phase 6.5 — Entities on the wire (foundation for everything below)
- [x] EntityState in snapshots (id/archetype/pos/yaw/anim/health/data, cap 256,
      hostile-count rejection); absence = despawn; bobbing box render proxies
- [x] Item pickups: 16 seeded loot spawns in dungeon rooms (ammo/medkits), E to take
      (pickup beats consume), partial-stack pickup, inventory sync
- [ ] Drop-on-death; interpolation for moving entities (with projectiles/NPCs)

## Phase 6.6 — Ballistics & destruction ("Siege but further", ARCHITECTURE §game/ballistics)
- [x] Material props on blocks (hp, penCost: stone 100/45, dirt 40/15, grass 30/12)
- [x] Penetrating hitscan: ray-march w/ penetration budget (weapon penBudget), 0.65×
      damage attenuation per block, flesh stops bullets; GameRules toggles
      (penetration, blockDamage → wire flags bits 2/3)
- [x] Sparse voxel damage map (chip destruction — pistol needs 4 hits through stone)
- [ ] Reinforced block variants; explosives radial voxel damage (lands with projectiles)
- [ ] Structural integrity flood-fill collapse (GameRules toggle)

## Phase 6.7 — Weapon archetypes (ARCHITECTURE §game/weapons)
- [x] ItemDef weapon spec: FireMode (semi/auto/burst), DeliveryKind
      (hitscan/projectile/deployable), pellets/spread/blast fields
- [x] Hitscan variants via pellets+spread+budget: shotgun (8 pellets/6°), sniper
      (90 budget, drills stone), SMG (auto, 2.2° bloom), pistol; deterministic
      hashed spread cone (peer/tick/pellet)
- [x] Projectiles as server entities: RPG (fast, 4.5 m blast + crater), grenade
      (gravity/fuse, 3.5 m blast); radial player + voxel damage; entity-path render
- [x] Deployables: claymore (proximity trap, arm time, owner-safe while arming)
- [x] Reference arsenal all spawned at loadout: pistol/SMG/shotgun/sniper/RPG/
      grenade/claymore + 4 ammo types + medkit + blocks
- [ ] Ammo types modifying ballistics (AP/HP); recoil/bloom feel pass (playtest)

## Phase 6.8 — AI & navigation (ARCHITECTURE §game/ai)
- [x] v1 voxel-native pathing: 3D A* over live voxels (step-up 1 / drop 4, node budget,
      snap-to-standable) — destruction-aware by construction, no navmesh rebuilds
- [x] Chaser (melee rush) + Shooter (LoS ranged, holds distance) NPCs: aggro w/ wall
      check, 0.6 s repath, kinematic waypoint follow; seeded dungeon-room spawns;
      NPCs take bullet + blast damage, drop ammo on death, ride entity snapshots
- [x] Turret: placeable auto-defense (deployable item) — targets nearest hostile NPC in
      range + LoS, fires hitscan on cadence, faces target, has health, rides entity snapshots
- [ ] Companion behavior; voxel-sampled cover seek; spawner volumes + waves
- [ ] NPC schedules/factions/world clock (living-world layer); Recast/Detour v2 swap
- [ ] World clock (day cycle in snapshots) + NPC schedules (timeRange→activity@location,
      interrupt stack, factions/aggro matrix, editor spawn+location markers, far-LOD
      coarse ticking) — the living-world layer for Sandbox/PvE

## Phase 6.9 — Game modes (ARCHITECTURE §game/modes)
- [ ] GameMode framework: teams, friendly fire, respawn policy, win-condition hooks
- [ ] Reference modes: Breach (attack/defend + destruction), Horde (PvE waves),
      Deathmatch, Sandbox (open-world creative/survival: no win condition, per-player
      streaming centers, persistent dedicated-server worlds)
- [ ] `--mode <name>`, mode in Welcome; Lua-defined once scripting lands

## Phase 11.5 — Abilities & spawned actors (GAS-lite, see ARCHITECTURE §game/abilities)
- [ ] Effect executors: Damage, AreaDamage (explosives + batch voxel ops), Heal, ApplyModifier
- [ ] Projectiles (server-simulated, explode-on-impact into effects)
- [ ] SpawnEntity behaviors: placeable Turret (target-nearest + LoS), Companion (follow/attack)
- [ ] Lua-defined abilities bound to items (grenade, medkit-as-ability) and to classes
- [ ] Entity states join snapshots (shared path with player states)

## Phase 11.7 — Transport providers (dev matrix, ARCHITECTURE §net/discovery)
- [ ] UPnP port mapping via miniupnpc (MIT) at host time
- [ ] Master-coordinated UDP hole punching (simultaneous-open through ENet)
- [ ] GameNetworkingSockets provider (BSD-3): encrypted transport + Steam Datagram
      Relay path for Steam builds — alternate Transport impl, dev-selected
- [ ] Reachability diagnostics: master probe-back + LAN self-check, host UI verdict
      ("LAN-only — forward UDP <port>" vs "reachable"); registry outage degrades loudly
- [ ] Access control: server password, kick/ban by address, onAuthenticate hook

## Phase 12 — PvP hardening
- [ ] Lag-compensated hitscan (server rewind), delta-compressed snapshots, interest management
- [ ] 4–8 player arena on editor-built maps

## Phase 13 — OSS engine borrowings (docs/ENGINE_REUSE_SURVEY.md)
All 20 engines on the bobeff C++ list evaluated; licenses verified at source
(GitHub API's NOASSERTION is unreliable — Cafu/O3DE/Nebula/Torque3D are genuinely
permissive). COPY only MIT/BSD/zlib/Apache/PD; GPL/LGPL = ideas-only. Two traps:
Defold's Apache derivative forbids reselling as an engine (ideas-only for us), and
Luanti's permissive LICENSE covers media only — engine code is LGPL (ideas-only).
Ranked, effort-estimated borrowings (feed the phases above, not net-new work):
- [ ] Cafu `Libs/Network/State.*` delta compression — MIT, self-contained lift → Phase 12
- [ ] ozz-animation / Esoterica blend-state graph — both MIT → Phase 7 (blend, not importer)
- [ ] Torque3D ghost scope→priority→delta interest management — MIT, reimpl on ENet → Phase 12
- [ ] O3DE NetworkTime/NetworkInput lag-compensation pattern — Apache/MIT, reimpl → Phase 12
- [ ] Ogre3D cooked `.mesh`/`.skeleton` serializer for fast loads — MIT → Phase 7
- [ ] Linux CMake + CI (only 2 files have Win32 sockets, both already #ifdef-branched;
      GLFW windowing already portable) — informed by GLFW/ezEngine platform layer
- Honorable mentions: Librebox Luau sandbox (security for public scriptable engine),
  Cute Framework connect-token handshake, Nebula BSD frame-graph.
