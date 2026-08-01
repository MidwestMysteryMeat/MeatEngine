# MeatEngine — Agent backlog complete

**Date:** 2026-07-31  
**Scheduler:** durable 30m fire (`docs/OPEN_SCHEDULE.md`) — **stop**; only deferred /
human items remain.

## What this pass shipped (creation-suite track)

| ID | Item | Notes |
|----|------|--------|
| B3-water | Underwater water plane | `water.vert`/`water.frag`, env-driven PsxOptions |
| A6 | Blob shadows | Thin discs under local / remote / NPC feet |
| C8 | Lite profiler | F3 ImGui panel (no Tracy) |
| A2-s | Skinned shadow casters | `shadow_skinned.*` bone palette depth pass |
| C5+ | Details material/blocks | Prop material edit + block registry panel |
| B3b-net | Gravity volume net sync | `GravityVolumesMsg` join + live broadcast |
| ARCH | ARCHITECTURE reconcile | Status banner + net/physics/editor/anim sections |

Prior suite work (already on `main` before this scheduler): C6/C7/C9, B2 MeshLevel,
B5 game.json, A1–A3, B3-sky, H1/H4 templates, etc. See [ROLLOUT.md](ROLLOUT.md).

## Intentionally not done (deferred / human)

| Item | Why |
|------|-----|
| D1 binary greedy mesher | Risk; measure first |
| D4 EnTT migration | Large, no feature unlock alone |
| F1/F2 interest + lag-comp | Scale / PvP depth |
| SSAO / RGB block light | Visual polish after suite |
| Real content-browser thumbnails | Needs bake pipeline |
| Steam / UPnP / hole punch | Platform / hosting |
| Linux hardware CI, feel playtests | Human-only |
| Full GAS, game modes, in-engine modeler | Genre / content systems |

## How to continue later

1. Pick from deferred table or a new ROLLOUT item.  
2. One shippable slice: implement → `scripts/build.ps1` → docs → commit as **MysteryMeat-G**.  
3. Re-enable a scheduler only if a new multi-item agent queue is defined.

**Author:** MysteryMeat-G. No AI commit attribution.
