#include "engine/level/MeshLevel.h"

#include "engine/asset/ModelLoader.h"
#include "engine/core/Log.h"
#include "engine/voxel/ChunkMesher.h" // VoxelVertex

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace meat {

void MeshLevelRuntime::clear(PhysicsWorld& physics) {
    for (Part& p : parts) {
        if (p.body != PhysicsWorld::kInvalidBody) {
            physics.removeStaticBox(p.body);
            p.body = PhysicsWorld::kInvalidBody;
        }
        // Mesh/material handles live in Renderer caches; leave them (leaked on
        // reseed is acceptable for v1; destroyMesh optional later).
    }
    parts.clear();
}

int MeshLevelRuntime::load(const MeshLevelDesc& desc, Renderer& renderer, PhysicsWorld& physics) {
    clear(physics);
    int loaded = 0;
    for (const MeshLevelInstance& inst : desc.instances) {
        if (inst.assetPath.empty()) continue;
        ModelImportOptions opts;
        opts.scale = inst.scale > 1e-4f ? inst.scale : 1.0f;
        opts.center = false;
        const auto model = loadStaticModel(inst.assetPath, opts);
        if (!model) {
            log::error("MeshLevel: failed to load '{}'", inst.assetPath);
            continue;
        }

        Part part;
        part.assetPath = inst.assetPath;
        part.transform = inst.transform;
        part.mesh = renderer.uploadChunkMesh(model->mesh);
        MaterialDesc mat;
        if (!model->albedo.empty()) {
            mat.albedo = renderer.loadTexture(model->albedo);
        }
        if (mat.albedo == 0) {
            // Fallback white-ish tint so untextured meshes still read.
            mat.tint = glm::vec3(0.75f, 0.78f, 0.82f);
        }
        part.material = renderer.createMaterial(mat);

        // Triangle collider: transform verts into world space.
        std::vector<glm::vec3> worldPos;
        worldPos.reserve(model->mesh.vertices.size());
        for (const VoxelVertex& v : model->mesh.vertices) {
            const glm::vec4 w = part.transform * glm::vec4(v.pos, 1.0f);
            worldPos.emplace_back(w.x, w.y, w.z);
        }
        part.body = physics.addStaticTriangleMesh(worldPos, model->mesh.indices);
        if (part.body == PhysicsWorld::kInvalidBody)
            log::warn("MeshLevel: no collider for '{}' (render only)", inst.assetPath);

        parts.push_back(part);
        ++loaded;
        log::info("MeshLevel: loaded '{}' ({} tris)", inst.assetPath,
                  model->mesh.indices.size() / 3);
    }
    return loaded;
}

void MeshLevelRuntime::submit(Renderer& renderer) const {
    for (const Part& p : parts) {
        if (p.mesh != 0) renderer.submitMesh(p.mesh, p.transform, p.material);
    }
}

glm::mat4 meshLevelTransform(glm::vec3 pos, float yawRadians) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), pos);
    if (std::abs(yawRadians) > 1e-6f) m = m * glm::rotate(glm::mat4(1.0f), yawRadians, glm::vec3(0, 1, 0));
    return m;
}

} // namespace meat
