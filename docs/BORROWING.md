# Open-source borrowing plan

Survey of MIT-compatible projects worth lifting from (licenses verified 2026-07-30
against GitHub API / raw LICENSE files). Full rule: MIT/BSD/zlib/Apache-2 = portable
with notices kept; GPL/LGPL = ideas only, never code.

## Ranked borrowings

1. **yojimbo** (BSD-3, active) — transport layer: client slots, encrypted connect
   tokens, reliable messages. Prediction built on top per Gaffer On Games +
   Gabriel Gambetta article series. ~25–40 h. Alternative: ENet (MIT, simpler, no
   auth/encryption); upgrade path: GameNetworkingSockets (BSD-3, Steam relay).
2. **cgerikj/binary-greedy-meshing** (MIT) — bitwise-mask greedy mesher built for
   32³-padded chunks (50–200 µs/chunk); drop-in upgrade for our face-culling mesher.
   ~8–15 h.
3. **ozz-animation** (MIT, active) — skeleton sampling/blending/IK runtime instead of
   hand-rolled Assimp playback; Mixamo rigs go through its glTF pipeline; we supply
   the matrix palette to our GL skinning shader. ~15–25 h.
4. **PSX shader recipes** — MenacingMecha/godot-psx-style-demo + dsoft20/psx_retroshader
   (both MIT): vertex snap, `noperspective` affine UVs, Bayer dither, banded fog,
   transcribed into our GLSL. ~6–10 h.
5. **Wicked Engine** (MIT, active) — pattern reference for Jolt glue (wiPhysics) and
   Lua binding conventions (wiLua); selective porting with notices. ~10–20 h.

## Ideas-only (copyleft — read, never copy)

- **Luanti/Minetest** (LGPL): MapBlock net serialization, Lua sandbox API surface.
- **ioquake3** (GPL-2): snapshot/delta/prediction flow — covered cleanly by the
  Fiedler/Gambetta articles instead.
- **Veloren** (GPL-3): chunk compression + LoD write-ups.

## Also noted

- **Lumix Engine** (MIT): closest-in-scale reference for editor/gizmo + reflection.
- **raylib** (zlib): API design taste for thin wrappers.
- **fast-wfc** (MIT): vendorable WFC for intra-room decoration; dungeon LAYOUT stays
  hand-rolled BSP/room-stitching (<300 lines, fits the chunk/room pipeline better).
- **Godot** `modules/multiplayer` (MIT): property-replication/spawn-authority design
  reference.

## OpenMW study (GPL-3 — ideas only; evaluated 2026-07-30)

TheLostPantheon/openmw = OpenMW Vita (PS Vita port); no procgen anywhere in the
ecosystem — our dungeon path is already ahead. Takeaways adopted as design guidance:

- **Effect schema three-tier split** (EffectDef → parameterized use → runtime instance
  with remaining duration) + explicit stacking flags (`stackable`, `affectsBase`,
  `temporary`) → adopted into the GAS-lite plan (ARCHITECTURE §game/abilities).
- **Items grant abilities via a trigger enum** (onUse/onStrike/constant) + charge pool —
  one mechanism for consumables, enchanted weapons, and passives.
- **Lua API model**: global scripts (world-mutating = server) vs local scripts
  (own-object-only = client/entity), one-frame-delayed events, capability-scoped
  packages, and a strict onSave value contract (nil/number/string/table, no functions) —
  the blueprint for our sol2 surface.
- **Recast/Detour is zlib** and OpenMW's runtime navmesh-from-physics-per-cell pattern
  maps directly onto navmesh-from-voxel-colliders-per-chunk for future NPC/turret AI —
  directly adoptable code.
- **Dynamic budget scaling** (Vita fork): auto-tune fog/draw distance to hold framerate —
  cheap and on-brand for the PSX renderer.

Every ported file or transcribed routine gets its upstream notice in THIRD_PARTY.md.
