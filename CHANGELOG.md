# Changelog

All notable changes to MeatEngine are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project aims at
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- **CI**: multi-platform build (Ubuntu/GCC + Windows/MSVC), an ASan+UBSan
  sanitizer job, a layering guard (engine/ must not depend on game/), a headless
  render smoke gate (xvfb + mesa llvmpipe), and build-tree caching for fast runs.
- **Netcode**: F1 interest management (per-client entity scoping); lag-compensated
  hitscan; connection auth via a server join password (`--password`); **wire
  encryption** (XChaCha20-Poly1305 via Monocypher, auto-enabled with a password);
  a concurrent-player cap; and periodic autosave for crash-safety.
- **GameMode framework**: Sandbox, Deathmatch, and **Team Deathmatch** (two
  auto-balanced teams, per-team scoring, team frag-limit win), plus a
  **friendly-fire** toggle (`--friendly-fire`) that gates teammate damage across
  hitscan, blasts, and effects. Selectable via `--mode` / `game.json`.
- **Data-driven UI / HUD system (Phase 5 first slice)**: `engine/ui` — a retained
  widget tree (Panel / Bar / Label / Image) with UMG-style fractional anchors +
  pixel-inset layout, recursive resolution to pixel rects, a JSON HUD-definition
  loader (author HUDs as data, not code), and a binding context so a bar's fill or
  a label's value tracks a named signal (e.g. `player.health`), plus the editor
  primitives — JSON serialize (round-trips parse, for saving) and draw-order-aware
  hit-testing (`widgetAt`) + a UIInput hover/press/click state machine. Groundwork for a
  renderer pass and a visual UI editor (the UE5-UMG equivalent) — see Phase 5.
- **Access control**: `NetPolicy.onAuthenticate` — a host hook that rejects a
  joining peer by Hello name/token before admission (ban/allow-list/account check,
  no engine-side identity model) — plus `ServerSim::kick(peer)` to remove a live
  peer and all its state. Refused joins now drop their pre-Hello state cleanly.
- **AI crowds (Phase 7)**: a deterministic boids `CrowdSim` (separation /
  alignment / cohesion + goal-seek) — seeded, fixed-step, order-stable — now
  owned by the server, stepped each tick, and replicated through the entity
  snapshot as `Crowd` entities with stable ids (`spawnCrowd`/`setCrowdGoal`).
  Neighbour queries use a uniform spatial hash grid: **O(n) instead of O(n²)** and
  bit-for-bit identical to brute force, so a 400-agent crowd runs at server budget.
- **Neural-policy runtime seed (Phase 7)**: `engine/ai/MLP`, a self-contained
  feed-forward net (dense layers, ReLU/tanh/sigmoid, `forward` + `argmax` action
  selection) that fails closed on a malformed model — the inference core a learned
  `NpcBrain` will use. Groundwork alongside DetourCrowd, ML/learning agents, and an
  MCP agent bridge — see `docs/PRODUCTION_PLAN.md` Phase 7.
- **Effect system**: eight composable effect kinds — Damage, AreaDamage, Heal,
  ApplyModifier (enforced damage + move-speed, i.e. slow/haste), Knockback,
  Ignite (stacking damage-over-time with kill credit), Chain (arc to the nearest
  targets), and SpawnEntity (summon an owned turret/companion). The primitives
  are exposed to Lua (`game.ignite` / `game.chain_damage` / `game.spawn_turret` /
  `game.spawn_companion`) so scripts author their own effects.
- **Animation**: cross-convention retargeting bridge so Mixamo, UE4, and UE5
  mannequin clips all drive the canonical (Mixamo) skeleton.
- **Persistence**: save-file schema versioning (refuses newer-engine saves).
- **Packaging**: `cmake --install` + CPack produce a versioned game archive.
- **Tests**: 55 → 210 headless checks across worldgen/dungeon determinism,
  save/load, inventory, the effect system (knockback/ignite/chain/spawn), the Lua
  effect-primitive bridge, team deathmatch + friendly fire + access control, crowd
  determinism + flocking + replication + grid/brute equivalence at scale, MLP
  inference, the UI layout/binding/parse core, reconnection, and packet fuzzing.
- `docs/PRODUCTION_PLAN.md` — the phased path to production parity.

### Fixed
- Out-of-bounds server write from an `abs(INT_MIN)` voxel-coordinate bypass.
- Shader UBO layout mismatch that broke all rendering (`uHemiGround` desync).
- Modern-CMake (4.x) configuration failures from vendored deps' old policies.
- Snapshot positions silently clamping past ±2048 m (now signed 24-bit, ±32 km).

### Changed
- `Engine` moved from `engine/` to `game/` (it composes game systems), so the
  engine layer no longer depends on the game layer.
- **Unified player death**: every damage source (bullets, blasts, effects, NPCs,
  scripts) now routes through one `killPlayer` path — so blast/effect kills credit
  a frag, all deaths eject from ship seats and fire `on_player_death`, and respawn
  clears lingering damage-over-time and buffs.
- **`ServerSim.cpp` decomposed** 2780 → 1883 (−32%, under the god-file bar): the
  effect system, combat/hitscan/blast/projectiles, and enemy/ally AI moved to
  `ServerSimEffects.cpp` / `ServerSimCombat.cpp` / `ServerSimAI.cpp` (same class,
  own translation units, behaviour byte-identical), shared helpers in
  `ServerSimInternal.h`.

<!-- Add a new "## [x.y.z] - YYYY-MM-DD" section per tagged release. -->
