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

- [ ] **ASan + UBSan CI job** running the test suite (catches the exact bug classes above).
- [ ] **Layering guard** — CI fails if anything under `engine/` includes `game/`.
- [ ] **clang-format check** in CI (`.clang-format` exists, currently unenforced).
- [ ] **Worldgen determinism test** — same seed ⇒ byte-identical chunks (client/server parity).
- [ ] **Save/load round-trip test** — world + edits + inventory survive a save→load cycle.
- [ ] **Inventory / combat-math tests** — stacking, ammo, mag reload, penetration, damage mods.
- [ ] **Headless render smoke gate** — `--shot` must succeed in CI so a shader/UBO
      regression fails the build. (Wire the R720 VLM parity gate in as a nightly later.)

## Phase 2 — Scale & performance
- [ ] **F1 interest management** — per-client scope → priority → budget before delta-encode.
      Design fully specced in `NETCODE_DELTA_COMPRESSION.md §8` (Torque3D ghosting model).
      The bandwidth wall: today every client receives every entity.
- [ ] **[doc-only] `AckSystem`/`SequenceBuffer`** rolling ack-bitfield (reliable.io) —
      ~180 lines of finished C++ live only in `ENGINE_REUSE_SURVEY.md`. Adopt or delete;
      the current `ackSnapshotTick` piggyback is simpler but the extracted class is orphaned.
- [ ] **Benchmark harness** — mesher throughput, snapshot size, frame budget. Numbers first.
- [ ] **D1 binary greedy mesher** (cgerikj/binary-greedy-meshing) — only after benchmarks say so.
- [ ] **Entity/projectile interpolation** for smooth remote motion under latency.

## Phase 3 — Security & data durability
- [ ] **Transport connection auth** — challenge/response connect token (netcode.io/yojimbo
      pattern, `BORROWING.md #1`); ENet is currently unauthenticated.
- [ ] **Optional encrypted transport** — GameNetworkingSockets provider behind the
      `Transport` interface (Steam Datagram Relay path).
- [ ] **Save schema versioning + migration** — saves have no version field today; the first
      content change bricks old saves. Mirror the wire's `kProtocolVersion` discipline.
- [ ] **Access control** — server password, kick/ban, `onAuthenticate` hook (ROADMAP 11.7).
- [ ] **[consider] Luau sandbox** — swap sol2/vanilla Lua for Luau (read-only tables, per-VM
      budgets) for a public, scriptable engine (`ENGINE_REUSE_SURVEY` honorable mention).

## Phase 4 — Organization
- [ ] **Split the god-files** by concern: `ServerSim.cpp` (2518), `RoomEditor.cpp` (2396),
      `Engine.cpp` (1984) are ~35% of the codebase.
- [x] engine/→game/ layering fixed (Engine moved to game/); Phase-1 guard locks it in.

## Phase 5 — Feature depth (enough to ship a game)
- [ ] **GameMode framework** — teams, friendly fire, respawn policy, win-condition hooks;
      `--mode` + mode in Welcome. Reference modes: Breach / Horde / Deathmatch / Sandbox.
- [ ] **Unified entity-snapshot path** — turrets/companions/pickups replicate via the player
      path; enables generic `SpawnEntity`.
- [ ] **Effect kinds** — Ignite / Knockback / Slow / Chain / SpawnEntity (only Damage /
      AreaDamage / Heal / ApplyModifier exist today — 4 of 9), plus **Lua-defined effects**.
- [ ] **Dynamic navmesh rebuild on voxel edit** (tiled Detour) — AI silently breaks when
      terrain changes, which is always in a voxel game.
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
- [ ] **CMake `install()` + CPack**; **macOS build/CI** (currently Linux+Windows only).
- [ ] **Tagged semver releases + CHANGELOG**; artifact upload from CI.
- [ ] **D2 resource archives** (zip/pk3 mounts) → feeds packaging.
- [ ] **D3 cooked mesh serializer** (bake FBX→binary; drop runtime Assimp on the hot path).
- [ ] **Developer-UX bar** — dockable single-window layout, command palette, "play here",
      asset/authoring hot-reload, actionable errors, a new-project template that already runs.
- [ ] Music/audio pass; ImGuiColorTextEdit syntax highlighting + Lua REPL + asset previews.

---

## Doc hygiene (fix stale checkboxes found during the survey)
- ROADMAP Phase **8.7 "Visual node scripting — NOT built"** is wrong — node graphs *are*
  shipped (`NodeGraph.cpp`, imnodes→Lua, subgraphs, watches).
- `ANIMATION_BLEND_GRAPH.md` **"not yet integrated"** banner is stale — its §0–§1 sampler
  refactor + 2-clip `blendPose` are integrated; only §2 (additive) and §3 (graph) remain.
- Priority-plan / backlog docs still list **F1/F2 together as deferred**; F2 (lag comp) is done.

---

## Deferred / ideas-only (tracked so they aren't rediscovered)
D4 EnTT migration · D5 `.material` scripts · SSAO (A4) · RGB block light (A5) · cascaded
shadows · frame-graph · UPnP/hole-punch/Steam relay transports · delaunator-cpp · fast-wfc ·
Jolt ragdoll/hit-react · cooked-loader alternatives (Irrlicht/raylib). See `ENGINE_REUSE_SURVEY.md`
for the license-verified source list.
