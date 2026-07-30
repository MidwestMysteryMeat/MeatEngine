# Roadmap

Vertical slice goal: spawn in a hand-built room → pick up weapon + items → shoot →
enter a procedural dungeon → manage inventory → save and reload.

## Phase 0 — Scaffold ✅ (in progress)
- [x] Repo, license, docs, CMake + FetchContent (GLFW, glad, glm, Assimp, Jolt, Lua+sol2, ImGui, ImGuizmo, stb, miniaudio, nlohmann-json)
- [ ] Dependencies compile on MSVC/Ninja; stub window opens

## Phase 1 — Core loop & feel foundation
- [ ] Engine spine: fixed 60 Hz tick, JobQueue, EntityRegistry, EventBus, logging
- [ ] Window + raw mouse input + PlayerCommand + FPS camera (feel checkpoint #1)

## Phase 2 — Voxel world
- [ ] Chunks (32³, 0.5 m voxels), block registry, face-culled meshing (greedy later)
- [ ] Worker-thread meshing via JobQueue; streaming around player
- [ ] DDA raycast; place/break blocks

## Phase 3 — Physics
- [ ] Jolt world; chunk MeshShape colliders synced with remesh
- [ ] CharacterVirtual capsule controller (feel checkpoint #2 — HUMAN PLAYTEST)

## Phase 4 — Rendering
- [ ] Forward Blinn-Phong: directional + point/spot UBO
- [ ] PSX pipeline: half-res target, nearest, dither, vertex fog (runtime toggleable)
- [ ] Crosshair, texture atlas, shader hot-reload

## Phase 5 — FPS gameplay core
- [ ] Hitscan weapon, voxel damage, player health, muzzle flash (light pulse)

## Phase 6 — Inventory + Save/Load
- [ ] Slot inventory (weapons/ammo/consumables/keys), pickup, equip, use, Tab UI (ImGui)
- [ ] Save/load: meta.json + RLE chunks.bin, F5/F9, --load

## Phase 7 — Room Designer editor (F1)
- [ ] Free-fly camera, grid snap, modular tools (wall/floor/ceiling/doorway/platform brushes over voxels)
- [ ] Lights placement w/ live preview, outliner, properties, gizmos (ImGuizmo)
- [ ] Dungeon-seed volumes; save room/world; exit spawns player sensibly

## Phase 8 — Procedural dungeons
- [ ] Seeded rooms+corridors gen (size range, corridor width, branching, loops, verticality)
- [ ] Room templates/themes; blend with authored areas; runs on workers

## Phase 9 — Models & animation
- [ ] Assimp FBX/OBJ/GLB static meshes + PNG/JPG materials
- [ ] Skeletal: canonical Mixamo skeleton, shared clip set, one animated NPC
- [ ] Viewmodel (idle/fire)

## Phase 10 — Scripting
- [ ] Lua via sol2: entity spawn, voxel edit, player/inventory/weapons, events, dungeon params
- [ ] Weapons + pickups + a dummy enemy defined in assets/scripts/

## Phase 11 — Audio + polish
- [ ] miniaudio: footsteps, gunshot, UI clicks
- [ ] README build docs, demo content pass

## Phase 12 — Tools (post-slice)
- [ ] tools/autorig: Pinocchio core + Mixamo skeleton map CLI (auto-rig humanoid meshes)
- [ ] tools/audit_assets.py: skeleton naming, scale, textures, attribution checks
- [ ] --capture headless mode → external VLM QA gate
- [ ] Asset staging from CC-BY sources + ATTRIBUTION.md automation
