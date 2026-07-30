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
- [ ] **4. Delta-compressed snapshots + ack** — per-client baseline diff (design in
  docs/NETCODE_DELTA_COMPRESSION.md) on the reliable.io ack buffer. OSS: Cafu (MIT), reliable.io
  (BSD-3). Gate: 2-process MP test still passes; bandwidth drops; fix the `peekType` bound bug.
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
- [ ] **8. Recast/Detour navmesh** (zlib) replacing hand-rolled A*.
- [ ] **9. Abilities / GAS-lite** (effect executors) + **10. game-mode framework** (Breach/Horde).
- [ ] **11. Destruction depth** — reinforced blocks, radial voxel damage, structural collapse.

### Roll 5 — Platform & tooling
- [ ] **12. Linux build + CI** (only 2 socket files, both #ifdef-branched). OSS: GLFW/ezEngine
  platform layer. **13. Positional 3D audio.** **14. Cooked mesh serializer** (Ogre, MIT).

### Roll 6 — Authoring (biggest, most speculative; last)
- [ ] **15. In-editor Design panel** (no-code weapon/ability/item) + **16. visual node graph →
  Lua** (ImNodes MIT) + **17. modular part-weapons / voxel object modeler**.

## Not automatable
Human feel-playtests (mouse feel, combat cadence, editor ergonomics) — flagged at each phase;
must be a human.
