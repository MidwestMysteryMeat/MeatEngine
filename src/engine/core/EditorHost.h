#pragma once
#include "engine/platform/Input.h"
#include "engine/render/Camera.h"
#include "engine/render/Renderer.h"
#include "engine/voxel/VoxelWorld.h"

#include <functional>
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

struct EditorContext {
    Camera& camera;    // editor-owned free-fly camera; engine renders with it
    VoxelWorld& voxels; // client mirror — read/pick only, never setBlock directly
    Input& input;
    Renderer& renderer; // for preview submits (sprites/lights); engine owns passes
    std::vector<EditorLight>& lights;      // shared: engine renders these always
    std::vector<SeedVolume>& seedVolumes;  // shared: consumed by dungeon gen
    std::function<void(glm::ivec3, BlockId)> requestVoxelOp; // → server, validated
    std::function<void(bool)> setRelativeMouse;              // capture for fly-look
    std::function<void()> requestWorldSave;                  // server save + extras
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
