#pragma once
#include "engine/platform/Input.h"
#include "engine/render/Camera.h"
#include "engine/render/Renderer.h"
#include "engine/voxel/VoxelWorld.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace meat {

// The engine's view of "an editor" — src/editor implements IEditor and main.cpp
// injects it, so engine/game never include editor code (ARCHITECTURE layering).
// Everything the editor does to the world goes through requestVoxelOp, which the
// engine routes to the server like any other client intent: editing therefore
// works over the network with no extra machinery.
struct EditorLight {
    int type = 0; // 0 point, 1 spot
    glm::vec3 pos{0};
    glm::vec3 color{1};
    float radius = 8.0f;
    glm::vec3 dir{0, -1, 0};
    float angle = 0.6f; // spot half-angle, radians
};

struct SeedVolume { // marks a region for procedural dungeon generation (Phase 6)
    glm::ivec3 min{0}, max{0}; // inclusive voxel bounds
    std::uint32_t seed = 0;
};

// B3b-e: editor-authored local gravity AABB (world metres). Applied on top of the
// env default field (habitat / planetoid still come from configureDefaultGravityField).
// Higher priority wins when volumes overlap.
struct EditorGravityVolume {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    glm::vec3 gravity{0.0f, -12.0f, 0.0f};
    int priority = 20; // above Space habitat (10) / station dock (9) by default
};

// A mesh asset placed into the world. As of the prop-sync pass this list is the
// engine's mirror of the SERVER-authoritative world props: entries arrive only
// from the server (PropAddedMsg / join replay), carry the server-assigned id, are
// rendered every frame, back a client-mirror box collider (for prediction), and
// persist in the world save — not the editor extras. `id` is 0 only for a
// transient not-yet-acked local copy (none are created that way now).
struct EditorProp {
    std::uint32_t id = 0;      // server-assigned world-prop id (0 = unsynced)
    std::string assetPath;     // project-relative model path (e.g. "assets/models/prop_crate.obj")
    glm::mat4 transform{1.0f}; // world TRS, manipulated by the ImGuizmo transform gizmo
};

struct EditorContext {
    Camera& camera;    // editor-owned free-fly camera; engine renders with it
    VoxelWorld& voxels; // client mirror — read/pick only, never setBlock directly
    Input& input;
    Renderer& renderer; // for preview submits (sprites/lights); engine owns passes
    std::vector<EditorLight>& lights;      // shared: engine renders these always
    std::vector<SeedVolume>& seedVolumes;  // shared: consumed by dungeon gen
    std::vector<EditorGravityVolume>& gravityVolumes; // B3b-e habitat boxes
    std::vector<EditorProp>& props;        // shared: engine renders these always
    std::function<void(glm::ivec3, BlockId)> requestVoxelOp; // → server, validated
    std::function<void(bool)> setRelativeMouse;              // capture for fly-look
    std::function<void()> requestWorldSave;                  // server save + extras
    // Place a mesh prop as a server-authoritative world object (editor intent).
    // Engine sends PlacePropMsg; the prop returns via PropAddedMsg and lands in
    // `props` with a real id + collider. Replaces the old local push_back.
    std::function<void(std::string asset, glm::mat4 transform)> requestPlaceProp;
    // Gizmo / outliner intents for an already-synced prop (id != 0). Engine sends
    // MovePropMsg / RemovePropMsg; server rebuilds collider or deletes and echoes.
    std::function<void(std::uint32_t id, glm::mat4 transform)> requestMoveProp;
    std::function<void(std::uint32_t id)> requestRemoveProp;
    // B4 New Map: host/SP only. Terrain/env/template are GameRules enum ordinals
    // so engine/core stays free of game includes (ARCHITECTURE layering).
    // template: 0 fps, 1 tps, 2 space (may override terrain/env when creating).
    std::function<bool(int terrain, int environment, int gameTemplate, std::uint32_t seed)>
        requestNewMap;
    int currentTerrain = 0;      // GameRules::Terrain ordinal
    int currentEnvironment = 0;  // GameRules::Environment ordinal
    int currentGameTemplate = 0; // GameRules::Template ordinal
    std::uint32_t currentSeed = 1337;
    bool hemisphereAmbient = true;
    std::function<void(bool)> setHemisphereAmbient;
    // Rebuild client (+ host server) gravity field after B3b-e volume edits.
    std::function<void()> applyGravityVolumes;
    // Asset/code panels: enumerate files under a project-relative dir, read/write
    // text, and hot-reload scripts into the running server (host/single-player).
    std::function<std::vector<std::string>(const std::string& dir)> listFiles;
    std::function<std::string(const std::string& path)> readFile;
    std::function<bool(const std::string& path, const std::string& text)> writeFile;
    std::function<bool()> reloadScripts;
    // Import a file into the project: validates by type, copies into assets/<subdir>,
    // returns a human-readable result string ("imported models/foo.fbx (1.8m, 65 bones)"
    // or "rejected: <reason>"). Empty return = not attempted.
    std::function<std::string(const std::string& sourcePath)> importAsset;
    BlockId buildBlock = 1; // what brushes place
    // C5: shared material for a prop asset path (all instances of that mesh). Empty handle
    // or missing callbacks → Details shows material as unavailable.
    std::function<MaterialHandle(const std::string& assetPath)> propMaterialHandle;
};

class IEditor {
public:
    virtual ~IEditor() = default;
    // Called once per frame while editor mode is active, inside the ImGui frame
    // and inside the renderer's begin/endFrame — UI, input, previews all go here.
    virtual void update(EditorContext& ctx, float dt) = 0;
};

} // namespace meat
