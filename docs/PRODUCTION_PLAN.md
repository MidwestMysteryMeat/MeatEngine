# MeatEngine — Production-Parity Plan

**Purpose:** the ordered path from "playable vertical slice" to an engine other people
can ship games on. Phases are ordered by leverage: each one makes the next safer or
faster. Items tagged **[doc-only]** have a written design (often full code) in another
doc but no implementation — captured here so they don't get lost.

Status legend: `[ ]` open · `[~]` partial · `[x]` done.

---

## Phase 0 — Verified foundation ✅
The bedrock: the tree provably builds and tests run, on every push, on modern toolchains.

- [x] Multi-platform CI (Ubuntu/GCC + Windows/MSVC): configure, build, run the test suite.
- [x] Modern-CMake compatibility (`CMAKE_POLICY_VERSION_MINIMUM=3.5`) — deps with an
      ancient `cmake_minimum_required` no longer hard-fail on CMake 4.x.
- [x] Warning-free clean build (project code at `/W4 -Wall -Wextra`; deps silenced).
- [x] CI status badge + build-prerequisite docs.

## Phase 1 — Safety net (make regressions un-mergeable)
The shader-UBO bug broke *all* rendering and shipped undetected; the `abs(INT_MIN)` OOB
write reached the authoritative server. Both were invisible because nothing tested for
them. Close that first — everything after this is riskier without it.

- [x] **ASan + UBSan CI job** — Debug + `MEAT_SANITIZE`; already caught a real UBSan
      finding (FastNoiseLite's intentional hash wraparound, scoped-suppressed).
- [x] **Layering guard** — CI fails if anything under `engine/` includes `game/`.
- [x] **Build-tree caching** — skips the ~15-min dep rebuild on CI.
- [ ] **clang-format check** in CI — deferred: needs a one-time format pass across the tree
      first (else it fails on all existing files), then enforce.
- [x] **Worldgen determinism test** — same seed ⇒ byte-identical chunks.
- [x] **Dungeon determinism test** — same seed ⇒ identical rooms/entrance; carve queries.
- [x] **Save/load round-trip test** — edits + tick survive save→load; corrupt saves rejected.
- [x] **Inventory tests** — stacking/spill, cross-slot removal, magazines, wire codec.
- [ ] **Combat-math / effects integration test** — penetration, damage mods, and effect
      execution (Heal/ApplyModifier/AreaDamage) via a two-peer ServerSim harness. The last
      Phase-1 coverage gap; lower ROI (Damage is covered by the lag-comp test).
- [~] **Headless render smoke gate** — CI `render-smoke` job builds the engine and runs
      `--shot` under xvfb + mesa llvmpipe, requiring a non-trivial screenshot (a shader/UBO
      regression fails engine init or yields a blank frame). Remaining: wire the R720 VLM
      parity gate as a nightly for actual visual-correctness grading.
- [x] **Bonus: cross-convention animation retargeting** (Mixamo/UE4/UE5) — see Phase 5.

**Coverage: 55 → 99 checks this session** across 6 new suites (net-permissions, delta-codec,
entity-registry, worldgen, dungeon, save-load, inventory, bone-retarget).

## Phase 2 — Scale & performance
- [~] **F1 interest management** — first slice done: per-client entity scoping by radius
      (`GameRules.interestRadius`, default 0 = disabled) with per-client delta baselines.
      Remaining: distance/importance PRIORITY + per-snapshot budget when scoped sets are large.
- [ ] **[doc-only] `AckSystem`/`SequenceBuffer`** rolling ack-bitfield (reliable.io) —
      ~180 lines of finished C++ live only in `ENGINE_REUSE_SURVEY.md`. Adopt or delete;
      the current `ackSnapshotTick` piggyback is simpler but the extracted class is orphaned.
- [ ] **Benchmark harness** — mesher throughput, snapshot size, frame budget. Numbers first.
- [ ] **D1 binary greedy mesher** (cgerikj/binary-greedy-meshing) — only after benchmarks say so.
- [ ] **Entity/projectile interpolation** for smooth remote motion under latency.

## Phase 3 — Security & data durability
- [x] **Transport connection auth + encryption** — join-password gate
      (`NetPolicy.serverPassword`, HelloMsg v4, `--password`) PLUS wire encryption:
      `EncryptedTransport` seals every payload with XChaCha20-Poly1305 (Monocypher) under
      `BLAKE2b(password)`, so traffic (incl. the Hello password) is confidential + authenticated
      and a peer without the password can't read or forge packets. Auto-enabled when a password
      is set. Remaining: X25519 connect tokens for **forward secrecy** + a salted KDF.
- [ ] **GameNetworkingSockets provider** — Steam Datagram Relay / NAT-punch transport (a
      deployment option, orthogonal to the crypto now in place).
- [x] **Save schema versioning** — saves carry `kSaveVersion`; the loader refuses a
      newer-engine save and accepts a legacy versionless one as v0. Migration hooks land
      with the first real schema bump.
- [ ] **Access control** — server password, kick/ban, `onAuthenticate` hook (ROADMAP 11.7).
- [ ] **[consider] Luau sandbox** — swap sol2/vanilla Lua for Luau (read-only tables, per-VM
      budgets) for a public, scriptable engine (`ENGINE_REUSE_SURVEY` honorable mention).

## Phase 4 — Organization
- [ ] **Split the god-files** by concern: `ServerSim.cpp` (2518), `RoomEditor.cpp` (2396),
      `Engine.cpp` (1984) are ~35% of the codebase.
- [x] engine/→game/ layering fixed (Engine moved to game/); Phase-1 guard locks it in.
- [x] **Unified player-death path** — eight duplicated drop+respawn blocks (hitscan, blast,
      effect Damage/Chain/Ignite, NPC melee, AI ship, Lua kill) collapsed into one
      `killPlayer(victim, victimPeer, killer)`. Fixed real inconsistencies in the process:
      blast/effect kills now credit a frag, every death (not just Lua's) ejects the victim
      from a ship seat and fires `on_player_death`, and respawn clears lingering DoT/modifiers
      so a lethal burn kills once. This is also the single seam a respawn policy will hook.

## Phase 5 — Feature depth (enough to ship a game)
- [~] **GameMode framework** — `GameRules.gameMode` = Sandbox / Deathmatch /
      **TeamDeathmatch**. FFA per-player frags + fragLimit; TeamDeathmatch auto-balances two
      teams, scores per team, ignores teammate kills, and ends on the team limit. **Friendly
      fire** (`--friendly-fire` / `friendlyFire`) gates teammate damage across every PvP path
      — hitscan, blast, effect Damage/Chain/Ignite. Match-over fires a HUD announce. Wired to
      `--mode team|tdm` / `game.json`. Remaining: respawn policy (timed/wave), team replicated
      in Welcome/PlayerState for client HUD colouring, more modes (Breach/Horde).
- [x] **Unified entity-snapshot path** — ships, NPCs, turrets, companions, projectiles,
      deployables, and pickups all replicate through `SnapshotMsg.entities` tagged by
      `EntityArchetype` (per-client interest scoping applies to the whole set). This was
      already in place; `SpawnEntity` rides it with no new wire work.
- [x] **Effect kinds** — Damage / AreaDamage / Heal / ApplyModifier (enforced
      speed = **Slow/haste**) / **Knockback** / **Ignite** (DoT, stacks + kill-credit) /
      **Chain** (arc to N nearest targets, kill-credit) / **SpawnEntity** (summons an
      owned turret/companion at the target point, replicated via the entity path) — eight
      built-in kinds, each with a public `applyDamageOverTime` / `applyChainDamage` /
      `spawnTurret` / `spawnCompanion` entry. **Lua-defined effects**: those primitives
      are exposed to scripts (`game.ignite` / `game.chain_damage` / `game.spawn_turret` /
      `game.spawn_companion`) so a Lua script composes its own effects from the same
      server-authoritative building blocks; the bridge is arg-for-arg tested. Optional
      follow-up: a first-class `EffectKind::Scripted` that dispatches to a named Lua
      handler carried in the effect itself (the composable primitives already cover the
      real use case).
- [ ] **Dynamic navmesh rebuild on voxel edit** (tiled Detour) — AI silently breaks when
      terrain changes, which is always in a voxel game.
- [x] **Cross-convention animation retargeting** — Mixamo, UE4-mannequin, and UE5-mannequin
      clips all bridge onto the canonical (Mixamo) skeleton via `canonicalBoneName`, and the
      autorig rigs meshes to that same canonical skeleton — so any of the three animation
      sources drives any autorigged character. Remaining: a UE-mannequin autorig *template*
      (Epic's asset can't ship in a public repo, but a UE bone-name fallback table can);
      finger-joint mapping (body/limbs done, fingers stay at rest).
- [ ] **[doc-only] Animation blend graph + state machine (`AnimGraph`)** — full drop-in code
      in `ANIMATION_BLEND_GRAPH.md §3` (states, 1D blend spaces, transitions, cross-fade);
      no `AnimGraph`/`BlendSpace1D`/`AnimState` in `src/`. The most detailed lost design.
- [ ] **[doc-only] Additive animation layers** — aim-offset / hit-react, per-bone masks
      (`ANIMATION_BLEND_GRAPH.md §2`). Enables **E3 weapon aim IK** (ozz `IKAimJob`).
- [ ] **[doc-only] Dungeon v2 template stitching** — editor rooms as templates with door
      sockets, opposite-door mating, layout-graph skeleton, corridor fallback, `isValidDungeon`
      retry; full architecture in `BORROWING.md`. Current `DungeonGen` is v1 rejection-placement.
- [ ] **Modular part-based weapons** — parts (body/barrel/grip/sight) + sockets + modifiers,
      snap-assembly → ItemDef, seeded loot (ROADMAP 8.6). Shares the dungeon socket system.
- [ ] **In-engine object/voxel modeler** — shape library + re-editable nodes → object prefab.
- [ ] **No-code editor "Design" panel** — weapon/ability/item authoring → project data defs,
      live-test, power-budget balance scorer (ROADMAP 8.6).
- [ ] Living-world AI: cover-seek, spawner volumes/waves, NPC schedules/factions/world clock.
- [ ] Destruction depth: structural-integrity collapse, reinforced blocks, radial voxel damage.

## Phase 6 — Distribution & DX
- [~] **Distribution**: `cmake --install` + **CPack** → a versioned game ZIP (runtime
      component only, no dep pollution); **CHANGELOG.md**; **release workflow** builds +
      uploads per-platform archives on a `v*` tag. **macOS** needs a Metal/MoltenVK backend
      (the engine targets GL 4.5; macOS caps at GL 4.1), so no macOS job yet. Observability:
      structured logging exists; crash telemetry intentionally omitted for privacy.
- [ ] **Tagged semver releases + CHANGELOG**; artifact upload from CI.
- [ ] **D2 resource archives** (zip/pk3 mounts) → feeds packaging.
- [ ] **D3 cooked mesh serializer** (bake FBX→binary; drop runtime Assimp on the hot path).
- [ ] **Developer-UX bar** — dockable single-window layout, command palette, "play here",
      asset/authoring hot-reload, actionable errors, a new-project template that already runs.
- [ ] Music/audio pass; ImGuiColorTextEdit syntax highlighting + Lua REPL + asset previews.

## Phase 7 — AI-native systems
Turn the deterministic, server-authoritative sim into a first-class home for AI: many
agents, learned behaviour, and an agent-facing control surface. **Determinism law:**
the netcode is lockstep delta-snapshot, so *anything on the authoritative tick must be
bit-for-bit reproducible across platforms* (fixed tick, seeded RNG, ordered containers,
integer/quantised math — never vendor-BLAS float or wall-clock). Non-deterministic ML
(ONNX/GGML on the GPU) is allowed **only** in tooling/editor/offline bakes, never the tick.

- [~] **AI crowds** — many lightweight agents that flow as a group instead of each paying
      full A*. First slice (in progress): a deterministic **boids** core (separation /
      alignment / cohesion + goal seek) for open-field crowds — seeded, fixed-step, order-stable.
      Next: promote to **DetourCrowd** (`dtCrowd`) on the Recast navmesh the engine already
      bakes (`m_navmesh`), for shared RVO avoidance through doorways/corridors; replicate via a
      new `EntityArchetype::Crowd` on the existing entity-snapshot path; crowd **flow fields**
      feed spawner waves. Target: hundreds of civilians/zombies at server budget.
- [ ] **Neural policies (embedded inference)** — the *runtime* side: drive NPC behaviour (and
      later animation) with a trained model as a drop-in `NpcBrain` beside the scripted one.
      A **tiny quantised MLP** evaluated in integer/fixed-point so it stays deterministic on
      the tick (perception vector → action logits). Models ship as data. Heavy runtimes (ONNX
      Runtime / GGML) are tooling-only (dataset gen, eval), never authoritative.
- [ ] **ML / learning agents** — the *training* side (distinct from running a net): agents that
      **learn** a policy rather than execute a hand-authored one. Reinforcement learning
      (self-play deathmatch, navigation, crowd flow) and imitation/behaviour-cloning from the
      existing A* NPCs and from recorded human play. The engine is the environment — stepped
      headless and in parallel through the **MCP bridge** below (observation = snapshot vector,
      action = the gated `ScriptApi` tools, reward = frags/objective/shaping). Output feeds the
      neural-policy runtime above. Deterministic tick = reproducible episodes = stable training.
- [ ] **MCP bridge (agent I/O)** — an out-of-process **Model Context Protocol** server exposing
      the engine's *existing* capability surface as MCP tools + resources: read-only resources
      (snapshot, entity list, match/team state, `playerHealth`) and gated tools (the same
      `ScriptApi` primitives — `spawn_turret`/`ignite`/`chain_damage`/… — that Lua scripts call,
      so agents act through the identical server-authoritative, permission-checked path, no new
      trust boundary). Talks to the server over a local socket. Turns the engine into an **agent
      environment**: RL training loops, LLM-driven NPCs/directors, and automated playtest agents.
- [ ] **AI director** — encounter pacing + spawn flow over the crowd flow fields, authored in the
      already-shipped node-graph (`NodeGraph.cpp`) extended with perception/steering/spawn nodes;
      composes with Phase-5 living-world AI (schedules/factions/world clock).

---

## Doc hygiene (fix stale checkboxes found during the survey)
- [x] ROADMAP Phase **8.7** header corrected — node graphs *are* shipped (`NodeGraph.cpp`).
- [x] `ANIMATION_BLEND_GRAPH.md` banner corrected — §0–§1 integrated; §2/§3 remain.
- [ ] Priority-plan / backlog docs still list **F1/F2 together as deferred**; F2 (lag comp) is done.

---

## Deferred / ideas-only (tracked so they aren't rediscovered)
D4 EnTT migration · D5 `.material` scripts · SSAO (A4) · RGB block light (A5) · cascaded
shadows · frame-graph · UPnP/hole-punch/Steam relay transports · delaunator-cpp · fast-wfc ·
Jolt ragdoll/hit-react · cooked-loader alternatives (Irrlicht/raylib). See `ENGINE_REUSE_SURVEY.md`
for the license-verified source list.
