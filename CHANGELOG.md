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
- **Tests**: 55 → 155 headless checks across worldgen/dungeon determinism,
  save/load, inventory, the effect system (knockback/ignite/chain/spawn), the Lua
  effect-primitive bridge, team deathmatch + friendly fire, reconnection, and
  packet fuzzing.
- `docs/PRODUCTION_PLAN.md` — the phased path to production parity.

### Fixed
- Out-of-bounds server write from an `abs(INT_MIN)` voxel-coordinate bypass.
- Shader UBO layout mismatch that broke all rendering (`uHemiGround` desync).
- Modern-CMake (4.x) configuration failures from vendored deps' old policies.
- Snapshot positions silently clamping past ±2048 m (now signed 24-bit, ±32 km).

### Changed
- `Engine` moved from `engine/` to `game/` (it composes game systems), so the
  engine layer no longer depends on the game layer.

<!-- Add a new "## [x.y.z] - YYYY-MM-DD" section per tagged release. -->
