# MeatEngine — Priority Plan (research synthesis)

**Date:** 2026-07-31  
**Sources:** `docs/ROLLOUT.md`, `docs/NEXT_SESSION.md`, recent `main` commits, `ARCHITECTURE.md`  
**Audience:** pick what to implement next, highest need first.

---

## 0. Reality check (roadmap is partly stale)

These are **already shipped** (or mostly shipped) even if older checklist lines still say open:

| Item | Status on disk |
|------|----------------|
| C1 content browser + grid/folders | ✅ |
| C2 dark editor theme (Slate-like) | ✅ first pass |
| C3 props + outliner + gizmo move/delete | ✅ server-authoritative |
| B1/B3/B4 New Map + env + terrain | ✅ |
| A1 AO, A3 hemi ambient | ✅ |
| C6 node graphs (imnodes → Lua) + ScriptFx | ✅ slices 1–3 |
| H1 FPS/TPS/Space/Racer templates | ✅ first slices |
| H2/H3 weapons, H4 ships (multi-seat, AI, EVA) | ✅ first slices |
| B3b GravityField + local-up | ✅ first slice |
| E1 foot-slide / gait rate | ✅ |

**Implication:** the old ROLLOUT order  
`C3 → C2 → B3/B4 → A1/A2 → C5 → C6 → B2…`  
is no longer the correct sequence. Usability-first still wins — but the **next** usability gaps changed.

---

## 1. Priority ranking (highest → lowest)

Scoring: **creator value** × **engine leverage** × **dependency readiness** − **risk/size**.  
⭐ = do soon. Size: S/M/L.

### P0 — Unblock creators (do next)

| # | ID | Item | Why highest | Size | Deps |
|---|-----|------|-------------|------|------|
| 1 | **C9** | **Output Log browser (UE5-style)** ⭐ | ✅ **Shipped** (`Log.cpp` ring + Room Designer panel). | S–M | — |
| 2 | **C6-a** | **Node-graph reliability + compile feedback** | Compile/reload must dump to C9; surface Lua parse errors on Compile; optional “open generated lua”. Makes C6 trustworthy. | S | C9 ideal |
| 3 | **Debt-doc** | **Reconcile ROLLOUT / NEXT_SESSION / ARCHITECTURE** | Wrong checkboxes cause wrong prioritization forever. Mark shipped C2/C3/B4/A1/C6; rewrite “Recommended near-term”. | S | none |

### P1 — Creation suite completeness (UE5 feel)

| # | ID | Item | Why | Size | Deps |
|---|-----|------|-----|------|------|
| 4 | **C6-b** | **More `game.*` nodes (combat/world hooks)** ✅ | Announce + damage player + tick/prop count/health. Spawn light later. | M | C9 |
| 5 | **C5** | **Inspectors (prop/env/material lite)** | UE Details-panel depth for selected prop/env/rules. Outliner already exists; needs richer fields. | M | C3 done |
| 6 | **C4** | **Import polish + path UX** | Paste-path import exists; file dialog (ImGuiFileDialog MIT) + clearer reject reasons. | S–M | C1 |
| 7 | **B3b-e** | **Editor gravity volumes** ✅ | Gravity tool + extras save + host apply. Net Welcome sync later. | M | GravityField done |
| 8 | **C6-c** | **Subgraphs / multi-graph / watches** ✅ | Multi-graph, watches, Call Subgraph. | L | C6-a, C9 |

### P2 — Visual & world payoff (player-facing quality)

| # | ID | Item | Why | Size | Deps |
|---|-----|------|-----|------|------|
| 9 | **A2** | **Directional sun shadow map** ✅ | Shipped: depth pass + PCF; skinned casters later. | M | A1 done |
| 10 | **B3-sky** | **Skybox / simple water plane** | Environment presets still fog-only; Space/Underwater read better with sky/water. | M | B3 |
| 11 | **H4-p** | **Ship systems polish** | Dock latch, shield/HP UX, hangar — only if Space is the showcase game. | M | H4 |
| 12 | **H1-r** | **Racer track mesh + server laps** | Fun genre sibling; lower than ship/log unless racing is the pitch. | M | Racer skeleton |

### P3 — Engine generality (new game shapes)

| # | ID | Item | Why | Size | Deps |
|---|-----|------|-----|------|------|
| 13 | **B2** | **`Level` / MeshLevel (non-voxel)** | Single + multi-mesh ✅; Level interface later. | L | physics ready |
| 14 | **B5** | **`game.json` world: {template, environment}** ✅ | Nested `world` + GAME_JSON.md. | S | B4 |
| 15 | **C7** | **Packaging / shippable game** ✅ | package.ps1: launchers, credits, zip. | M | D2 later |
| 16 | **G2** | **Game-mode framework** | Breach/Horde — gameplay, not engine core. | L | G rules |

### P4 — Systems depth & scale (later / riskier)

| # | ID | Item | Why defer | Size |
|---|-----|------|-----------|------|
| 17 | **F1/F2** | Interest management + lag-comp hitscan | Need for large PvP; not for creation-suite UX. | L |
| 18 | **D3** | Cooked mesh serializer | Load-time win; after import pipeline pain shows. | M |
| 19 | **D2** | Resource archives (zip/pk3) | Feeds C7; not blocking editor loop. | M |
| 20 | **D1** | Binary greedy mesher | Perf risk; measure first. | L |
| 21 | **D4** | EnTT migration | Incremental, no feature unlock alone. | L |
| 22 | **A4/A5/A6** | SSAO / RGB light / blob shadows | After A2. | M–L |
| 23 | **E2–E5** | Anim polish (pelvis, aim IK, faces) | After combat/template polish if FPS is showcase. | M–L |
| 24 | **G1/G3** | Destruction depth / Lua abilities | Content systems, not suite blockers. | L |
| 25 | **F3** | Full bitsery codec | When bandwidth becomes real. | M |

### P5 — Explicitly human / non-agent

- Mouse/combat/editor **feel playtests**
- Real Linux hardware CI
- Art direction / VLM gates for visuals

---

## 2. Mapping your current creation-suite backlog

Your list:

1. C6 continue (API / ScriptFx)  
2. C9 Output Log ⭐  
3. C6 subgraphs / multi-graph / watches  
4. A2 / B2 as capacity allows  

**Adjusted order (recommended):**

| Rank | Item | Change vs your list |
|------|------|---------------------|
| **1** | **C9 Output Log** | **Promote above further C6 features.** ScriptFx + many nodes already landed (`3c9920d`). Log is the missing half of “usable node graphs.” |
| **2** | **C6-a reliability** (compile → log, error surfacing) | Thin slice right after or with C9. |
| **3** | **C6-b selective API nodes** | Only nodes that unblock real graphs (not infinite node catalog). |
| **4** | **C5 lite inspectors / C4 import dialog** | Suite ergonomics; parallel-safe with small C6. |
| **5** | **A2 sun shadows** | ✅ Shipped (depth + PCF + editor toggle). |
| **6** | **C6-c subgraphs / multi-graph / watches** | After log + reliability. |
| **7** | **B3b editor gravity volumes** | ✅ Place/save/apply (host/SP). |
| **8** | **B2 MeshLevel** | Capacity allows — large, do after suite feels solid. |
| **9** | H4/H1 polish, packaging, OSS grabs | Genre polish and infrastructure. |

---

## 3. Suggested execution tracks (parallel-safe)

Work is still **one tree at a time** on `src/`, but *planning* can split:

```
Track A — Creator loop (DEFAULT)
  C9 Output Log → C6-a compile/log → C6-b high-value nodes → C5 inspectors lite

Track B — Look (after A or when A blocked)
  A2 sun shadow → optional A6 blob / B3 sky

Track C — Generality (when suite stable)
  B5 game.json world → B2 MeshLevel → C7 packaging

Track D — Scale (late)
  F1 interest → F2 lagcomp → D1/D3/D2
```

**Do not start C6-c (subgraphs) before C9.**  
**Do not start D1/D4 before measuring pain.**

---

## 4. C9 Output Log — acceptance criteria (ready to implement)

1. `meat::log::{info,warn,error}` append to a fixed-size ring (e.g. 2000 lines) with timestamp + level.  
2. Editor window **Output Log**: filter All | Messages | Warnings | Errors; substring search; Clear; auto-scroll toggle; copy selected.  
3. ScriptHost load/dispatch failures already use `log::error` — they appear automatically.  
4. Node Graph Save+Compile posts success/fail via `log::info` / `log::error`.  
5. Optional: `game.log` already goes through server `[lua]` → `log::info` — shows as Messages.  
6. Hotkey (e.g. `` ` `` or **Window → Output Log**) opens panel even outside full editor if desired.

Hook point: `src/engine/core/Log.h` (`write()`). UI: `RoomEditor` or a small always-available ImGui window in `Engine::render` when editor active.

---

## 5. What not to prioritize right now

- Infinite node-graph catalogs without log  
- EnTT / binary mesher “because OSS is cool”  
- Full PSSM/LiSPSM before basic A2 sun map  
- Game-mode framework before creators can place + script + see errors  
- Multi-graph tabs before single-graph reliability  

---

## 6. One-page ordered backlog (canonical)

Use this as the working list until the next research pass:

1. ⭐ **C9 — Output Log browser**  
2. **C6-a — node-graph compile/runtime → log**  
3. **Docs debt — mark shipped items in ROLLOUT**  
4. ~~**C6-b — High-value game API nodes only**~~ ✅  
5. **C5 lite — Prop/env inspectors**  
6. **C4 — Import file dialog**  
7. ~~**A2 — Sun shadow map**~~ ✅  
8. ~~**B3b-e — Editor gravity volumes**~~ ✅  
9. ~~**B5 — game.json world defaults**~~ ✅  
10. ~~**C6-c multi-graph + watches + subgraphs**~~ ✅  
11. ~~**B2 — MeshLevel first slice**~~ ✅  
12. ~~**C7 — Packaging**~~ ✅  
13. **H4/H1 polish** (if showcase genre needs it)  
14. **F1/F2 netcode**  
15. **D3 → D2 → D1 → D4** engine infrastructure  

---

## 7. Decision log

| Decision | Rationale |
|----------|-----------|
| C9 before more C6 features | Node-graph authoring without error visibility is not UE-like; log is S–M and high leverage |
| C6-c demoted | Subgraphs amplify complexity; need log + stable single graph first |
| A2 before B2 | Visual payoff for all templates; B2 is a large architecture cut |
| Keep H/G genre work below suite | Engine vision is creation suite first; genres already have playable slices |
| Measure before D1 | Hot-path mesher swap is high risk / medium reward until profiling says otherwise |
