# Visual scripting (C6 Blueprints)

UE-style **node graphs** that compile to the existing sandboxed Lua host
(`ScriptHost` + `game.*` API). Author in the Room Designer; runtime is always
server-authoritative Lua.

## How to use

1. Enter the editor (**F1** / `--editor`).
2. Open **Blueprints** (button on the Room Designer panel, or the Blueprints window).
3. Wire **Event** nodes (`On Init`, `On Tick`, `On Player Join/Death`) to **Action**
   nodes (`Log`, `Set Block`, `Spawn Pickup`) with **Data** / **Math** / **Branch**
   helpers in between.
4. Click **Save + Compile**:
   - Graph JSON → `scripts/blueprints/main.graph.json`
   - Generated Lua → `scripts/zz_blueprint.lua` (loads last alphabetically)
   - Host/SP: scripts hot-reload immediately

## Node palette

| Category | Nodes |
|----------|--------|
| Event | On Init, On Tick, On Player Join, On Player Death |
| Action | Log, Set Block, Spawn Pickup |
| Data | Player Count, Item Id, Random Int, Const Int/Float/String |
| Flow | Branch |
| Math | Add, Greater |

## Design notes

- **Exec pins** (flow) are white-circle style via imnodes; only Event/Action/Branch
  participate in the execution chain.
- **Data pins** are resolved by walking links backward; unwired inputs fall back to
  the node's literal fields (editable on the node body).
- Graphs never execute as a custom VM — they **emit Lua** that reuses instruction
  budgets, sandboxing, and the capability table already used by hand-written
  scripts (`assets/scripts/example.lua`).
- Provenance: UI uses [imnodes](https://github.com/Nelarius/imnodes) (MIT). Graph
  model + codegen are MeatEngine-owned.

## Files

| Path | Role |
|------|------|
| `src/engine/script/NodeGraph.{h,cpp}` | Data model, JSON, Lua emit |
| `src/editor/RoomEditor.cpp` (`drawBlueprints`) | imnodes editor UI |
| `scripts/blueprints/main.graph.json` | Saved graph (project) |
| `scripts/zz_blueprint.lua` | Generated hooks |

## Open follow-ups

More `game.*` API nodes, multi-graph tabs, debug "watch" pins, subgraphs,
ImGuiColorTextEdit for the text Lua path.
