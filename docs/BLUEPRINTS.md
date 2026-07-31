# Visual scripting (C6 Blueprints)

UE-style **node graphs** that compile to the existing sandboxed Lua host
(`ScriptHost` + `game.*` API). Author in the Room Designer; runtime is always
server-authoritative Lua.

## How to use

1. Enter the editor (**F1** / `--editor`).
2. Open **Blueprints** (Room Designer bar or the Blueprints window).
3. **UE-style graph UX**
   - **Right-click empty graph** → searchable place-node menu (Events / Actions / …)
   - **Double-click a node** (or press **F**, or **Open Node**) → **Node Details** panel
   - **Right-click a node** → open Details
   - **Outliner → prop → Create Blueprint Node** → `Get World Object` + highlight
   - Selecting / opening an object node **highlights** the prop in the viewport (cyan markers)
4. Wire **Event** nodes to **Action** nodes; use **Data** / **Math** / **Object** pins.
5. **Compile** / **Save**:
   - Graph JSON → `scripts/blueprints/main.graph.json`
   - Generated Lua → `scripts/zz_blueprint.lua` (loads last alphabetically)
   - Host/SP: scripts hot-reload immediately

The editor chrome uses a **UE5 Slate-inspired** dark theme (charcoal panels, blue selection).

## Node palette

| Category | Nodes |
|----------|--------|
| Event | Event BeginPlay, Event Tick, Player Join/Death |
| Action | Print String, Set Block, Spawn Pickup, Highlight/Print Object |
| Data | Player Count, Item Id, Random Integer, Integer/Float/String, Get Block |
| Object | Get World Object, Get Prop Position (runtime `game.prop_pos`) |
| Flow | Branch, Sequence |
| Math | Add, Subtract, Multiply, Greater, Equal |

**Runtime highlight:** `Highlight Object` emits `game.highlight_prop(id, seconds)` → server
`ScriptFxMsg` → all clients show cyan prop markers (not editor-only).

## Design notes

- **Exec pins** are triangles (UE-style); data pins are colored circles (int/float/string/bool/object).
- Only Event/Action/Branch participate in the execution chain.
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

- More `game.*` API nodes, multi-graph tabs, debug "watch" pins, subgraphs,
  ImGuiColorTextEdit for the text Lua path.
- **Output Log browser (C9)** — UE5-style in-editor log: Info / Warning / Error filters,
  search, clear, auto-scroll. Must surface ScriptHost errors (`script: … failed`),
  blueprint compile/reload status, and `game.log` lines so graph authors can catch
  mistakes without an external console. Tracked in [NEXT_SESSION.md](NEXT_SESSION.md)
  and [ROLLOUT.md](ROLLOUT.md) as **C9**.
