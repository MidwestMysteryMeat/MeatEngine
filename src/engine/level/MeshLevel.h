#pragma once

#include "engine/physics/PhysicsWorld.h"
#include "engine/render/Renderer.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace meat {

// B2 first slice: a non-voxel "level" is one or more static meshes with triangle
// MeshShape colliders. Voxel terrain can still exist (usually Void). Full Level
// interface / multi-map streaming later.

struct MeshLevelInstance {
    std::string assetPath; // project-relative, e.g. "assets/models/hangar.obj"
    glm::mat4 transform{1.0f};
    float scale = 1.0f; // multiplied into model load options
};

struct MeshLevelDesc {
    std::vector<MeshLevelInstance> instances;
    // When non-empty and forceVoid, prefer Void terrain so the mesh is the floor.
    bool forceVoidTerrain = true;
};

// Runtime handles after load (render + physics). Engine owns these.
struct MeshLevelRuntime {
    struct Part {
        MeshHandle mesh = 0;
        MaterialHandle material{MaterialHandle::Invalid};
        glm::mat4 transform{1.0f};
        PhysicsWorld::BodyHandle body = PhysicsWorld::kInvalidBody;
        std::string assetPath;
    };
    std::vector<Part> parts;

    void clear(PhysicsWorld& physics);
    // Load desc into renderer + physics. Returns number of parts loaded.
    int load(const MeshLevelDesc& desc, Renderer& renderer, PhysicsWorld& physics);
    void submit(Renderer& renderer) const;
};

// Helper: single-asset desc (common game.json form).
inline MeshLevelDesc makeMeshLevelDesc(const std::string& assetPath, float scale = 1.0f) {
    MeshLevelDesc d;
    if (!assetPath.empty()) {
        MeshLevelInstance inst;
        inst.assetPath = assetPath;
        inst.scale = scale;
        d.instances.push_back(std::move(inst));
    }
    return d;
}

} // namespace meat
