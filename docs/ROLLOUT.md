# MeatEngine Rollout Plan

Sequenced execution order for the remaining work (see ROADMAP.md for the full checklist).
Ordering = value × readiness, on-theme first, dependencies respected. Each item names the
OSS we lean on (license-verified in docs/ENGINE_REUSE_SURVEY.md) and its verification gate.

## Working discipline (every item)
1. **Research first** — read the OSS reference (technique, not blind copy); confirm license
   allows a copy vs ideas-only. Cite file:line provenance in code comments.
2. **Small, reviewable commits** — one concern per commit; keep the ARCHITECTURE.md contract
   sacred; match surrounding style; no dead code.
3. **Verify, don't assume** — build clean (no new warnings), run the relevant test/smoke, and
   for anything visual **gate on the R720 qwen3vl VLM** (never self-assess). Report the actual
   result, good or bad.
4. **Debug empirically** — when something's wrong, probe (log/marker/isolate) before guessing;
   the invisible-NPC bug (untextured skinned draw skipped) and the retarget T-pose reference
   were both found by probing, not theory.

## Sequence

### Roll 1 — Character & world fidelity (finish the visible layer)
- [ ] **1. Embedded / per-model textures** — extract FBX-embedded textures so characters render
  in their real colors (blue guy blue, PSX clothed) instead of fallback grey. OSS: stb_image
  (from-memory decode, already vendored) + Assimp `GetEmbeddedTexture`. Gate: VLM sees a
  *colored/clothed* character.
- [ ] **2. PSX environments/props staging** — load Killhouse/warehouse/props via the existing
  static loader as world content; per-model textures (from #1) apply. Gate: VLM sees a built
  environment, no magenta/missing.

### Roll 2 — Animation depth
- [x] **3. Animation blend graph** (commits 848de4d + 2a9c378): blendPose in local TRS space
  (slerp rot, lerp pos/scale, resolve once — never lerp skinning matrices); NPCs blend
  idle↔walk by client-derived speed. VLM-verified clean; samplePose bit-identical. OSS:
  ozz/Esoterica (MIT). Follow-ups: additive aim layers + a full state machine (design in
  docs/ANIMATION_BLEND_GRAPH.md §2–3); run clip for a 3-way idle↔walk↔run blend space.

> **Execution note (learned the hard way):** running multiple agents on the SHARED working
> tree corrupts uncommitted work — their build/commit git operations checkout-revert each
> other's (and my) in-progress edits. Only COMMITTED work survives. Do the roll SERIALLY
> (one workstream at a time, commit before the next) or give each agent an isolated worktree.

### Roll 3 — Netcode hardening (PvP path)
- [x] **4. Delta-compressed snapshots + ack** — DONE. Per-client baseline ring (last 32 emitted
  snapshots) + per-field changed-bitmask codec (src/engine/net/DeltaSnapshot.{h,cpp}); snapshot ack
  piggybacked on CommandMsg (client→server), baseline = last ACKED snapshot so unreliable-channel
  loss is safe, keyframe (baselineTick 0) = cold-start/recovery. Client reconstructs a full
  SnapshotMsg then calls the unchanged applySnapshot (prediction/rewind/interp/prune untouched).
  Fixed the `peekType` upper-bound bug (rejected >VoxelOp → Inventory/BatchVoxelOp/DeltaSnapshot
  un-peekable). Gate met: 2-process MP test passes (joiner welcomed as player 2, renders host's
  seed-777 world + remote player via delta reconstruction); steady-state ~1099B full → ~33B delta
  (~97% smaller). OSS: Cafu (MIT), technique-only (typed bitmask, not XOR+RLE). Interest management
  + lag comp stay in item 5 (separate).
- [ ] **5. Interest management** (Torque3D scope→priority→delta, MIT) and **6. lag-compensated
  hitscan** (O3DE NetworkTime pattern). Gate: rewind hits register; distant entities culled.

### Roll 4 — Systems depth
- [x] **7. Voxel light propagation** (torch flood-fill): per-voxel block-light (0..15) in
  Chunk; a main/edit-thread BFS floods emissive blocks at level-1 per air voxel and stops at
  solids, with an un-light + re-light BFS on edits and neighbor bleed-in on chunk load. The
  mesher stays pure — it reads a by-value light snapshot and keys greedy merges on (tile,
  light); the shader darkens terrain away from sources (skylight out of scope). An emissive
  "lamp" block (emission 15, atlas tile 5) seeds it near spawn. OSS: Luanti/Minetest light BFS
  (ideas only, original code). VLM-verified (qwen3vl 0.95: localized glow, darker with distance).
- [x] **8. Recast/Detour navmesh** (zlib) — added as an OPTIONAL, fallback-safe path provider
  (not a replacement): recastnavigation v1.6.0 via FetchContent (demo/tests/examples OFF, links
  Recast+Detour). New `game/NavMesh.{h,cpp}` runs a solo-mesh Recast build (rasterize → compact
  heightfield → regions → contours → poly mesh → Detour) over the world's chunk collision meshes
  — the SAME triangle soup ServerSim hands the physics colliders, captured off the mesh-ready
  callback; build is LAZY + throttled (≤1 bounded build per 2 s, off the streaming path).
  ServerSim::planPath tries `NavMesh::queryPath` (world corners → snapped to standable voxel
  cells so the existing cell-follow logic is reused verbatim) and falls back to the voxel A*
  (`findPath`) on ANY miss, so NPC/companion behaviour never regresses. Determinism: NPC pathing
  is host-authoritative (runs only on the server; clients get snapshots), so Detour adds no
  cross-peer nondeterminism — the simplest safe choice. STATIC navmesh this pass (rebuild-on-edit
  deferred; A* stays instantly edit-aware). Build clean under MSVC (no new engine-source warnings).
  Smoke (`--play --seed 777`): alive, 15 NPCs, world ready, no crash; instrumented run showed the
  navmesh build (434 polys / 28216 tris / 322 chunks, then a throttled rebuild at 416 chunks),
  32 Detour path hits + A* fallback engaging, and NPCs stepping toward the player (aggro+path OK).
  OSS: RecastDemo Sample_SoloMesh (zlib), technique-only.
- [x] **9. Abilities / GAS-lite** (effect-composition core) — server-authoritative, deterministic
  EFFECT system so weapons/abilities/items compose from reusable effects instead of bespoke code.
  A POD `Effect` (`enum Kind + float params[4] + radius/duration`, no RTTI/virtuals) in
  `game/Effects.h`; `ServerSim::runEffects`/`applyEffect` is the switch. Executors: **Damage**
  (single target), **AreaDamage** (radial — reuses `applyBlast` falloff + batched voxel-crater
  ops verbatim), **Heal** (clamped restore), **ApplyModifier** (timed per-player damage/speed
  mult, stored on the Player and ticked down in the fixed-tick `processCombat`). Wiring: `ItemDef`
  gained an `EffectList effects`; **medkit → Heal 50** (replaced the inline health bump),
  **rpg/grenade blast → AreaDamage** (projectiles carry an `onImpact` list; detonation runs it
  instead of calling `applyBlast` inline — behaviour-equal), **claymore → AreaDamage** too. New
  composed consumable **"stim"** = `[Heal 25, ApplyModifier(dmg x1.5, spd x1.3, 8s)]`, added to
  the loadout. The damage mult folds into hitscan (`marchBullet`) and blast magnitude; **speed
  mult is stored but not enforced** (CharacterController tuning is engine/physics-owned) —
  follow-up. Build clean (no new warnings), `--play --seed 777` smoke: alive 7 s, no crash, world
  ready, stim registered, 15 NPCs + dungeon up. AreaDamage NPC-kill confirmed by equivalence
  (same `applyBlast` args). **Lua-binding of abilities is the FOLLOW-UP** (this pass is the C++
  effect core + wiring). Item **10. game-mode framework** (Breach/Horde) still open.
- [ ] **11. Destruction depth** — reinforced blocks, radial voxel damage, structural collapse.

### Roll 5 — Platform & tooling
- [x] **12. Linux build + CI**: CMakeLists now guards MSVC-only flags behind `if(MSVC)`,
  drops NOMINMAX/WIN32_LEAN_AND_MEAN off-Windows, and links `Threads::Threads` + `${CMAKE_DL_LIBS}`
  on Linux (GL/X11/Wayland/m arrive transitively via the GLFW target). The two socket files
  (HttpTiny.cpp, LanDiscovery.cpp) were already fully `#ifdef _WIN32`/POSIX-branched — no changes
  needed; ENet handles its own platform sockets. `.github/workflows/ci.yml` builds Ubuntu/Clang +
  Windows/MSVC Release. Windows build re-verified clean (exit 0); **Linux path is unverified on
  real hardware** (authored on a Windows box) — test on a Linux PC. OSS: GLFW/ezEngine platform layer.
- [x] **13. Positional 3D audio** — `AudioEngine::setListener(pos,fwd,right)` (updated each frame
  from the active camera) + `playAt(sound, worldPos, vol)`: deterministic, hand-rolled gain/pan
  (no ma 3D engine, no new deps, no audio-callback-thread work) — linear distance falloff (full
  ≤1.5 u, silent ≥40 u; past the radius no voice is taken) and stereo pan = dot(dir, listenerRight)
  scaled 0.85 so the far ear never fully drops. 2D `play()` retained for first-person/HUD SFX and
  now resets pan (voice pool shared). Wired remote-player footsteps through playAt (speed derived
  client-side from interpolated position deltas — net/* untouched). Remote gunshots need a fire bit
  in PlayerState (net/* + ServerSim, out of scope) — deferred. Build clean (exit 0, no new warnings);
  `--play --seed 777` smoke: alive 6 s, no crash, "audio: engine up (6 synthesized sounds)". Audible
  result is human-verify-only (headless has no speakers). OSS: miniaudio (ma_sound_set_pan/volume),
  technique-only manual spatialization. **14. Cooked mesh serializer** (Ogre, MIT) still open.

### Roll 6 — Authoring (biggest, most speculative; last)
- [ ] **15. In-editor Design panel** (no-code weapon/ability/item) + **16. visual node graph →
  Lua** (ImNodes MIT) + **17. modular part-weapons / voxel object modeler**.

## Not automatable
Human feel-playtests (mouse feel, combat cadence, editor ergonomics) — flagged at each phase;
must be a human.
