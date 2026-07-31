#pragma once
#include "engine/platform/Input.h"
#include "engine/render/Camera.h"
#include "engine/render/Renderer.h"
#include "engine/voxel/VoxelWorld.h"

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

// A mesh asset placed into the world from the editor's Content Browser. Like
// EditorLight it is editor-owned, shared into the context, rendered by the
// engine every frame (mesh cached by assetPath) and saved via the editor
// extras. Decoration only for now: no server sync, no collision (see
// EditorProp handling in Engine.cpp).
struct EditorProp {
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
    std::vector<EditorProp>& props;        // shared: engine renders these always
    std::function<void(glm::ivec3, BlockId)> requestVoxelOp; // → server, validated
    std::function<void(bool)> setRelativeMouse;              // capture for fly-look
    std::function<void()> requestWorldSave;                  // server save + extras
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
};

class IEditor {
public:
    virtual ~IEditor() = default;
    // Called once per frame while editor mode is active, inside the ImGui frame
    // and inside the renderer's begin/endFrame — UI, input, previews all go here.
    virtual void update(EditorContext& ctx, float dt) = 0;
};

} // namespace meat
