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
  hitscan; connection auth via a server join password (`--password`).
- **GameMode framework**: Sandbox and Deathmatch (frag-limit win condition, HUD
  banner), selectable via `--mode` / `game.json`.
- **Animation**: cross-convention retargeting bridge so Mixamo, UE4, and UE5
  mannequin clips all drive the canonical (Mixamo) skeleton.
- **Persistence**: save-file schema versioning (refuses newer-engine saves).
- **Packaging**: `cmake --install` + CPack produce a versioned game archive.
- **Tests**: 55 → 121 headless checks across worldgen/dungeon determinism,
  save/load, inventory, effects, reconnection, and packet fuzzing.
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
