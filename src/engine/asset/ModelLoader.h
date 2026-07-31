#pragma once
#include "engine/voxel/ChunkMesher.h" // reuse VoxelVertex/ChunkMeshData for the mesh path

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace meat {

// A static model loaded via Assimp (FBX/OBJ/GLB). Geometry is emitted as
// ChunkMeshData so it rides the renderer's existing mesh pipeline unchanged —
// pos/normal/uv are real; the `tex` field is unused (0), the material's albedo
// texture is bound at submit time. Skeletal data is a later phase.
struct StaticModel {
    ChunkMeshData mesh;
    std::filesystem::path albedo; // first diffuse texture found, or empty
    glm::vec3 boundsMin{0}, boundsMax{0};
};

struct ModelImportOptions {
    float scale = 1.0f;   // multiply all positions (FBX cm→m is often 0.01)
    bool flipUV = false;  // some exporters need V flipped
    bool center = false;  // recenter XZ to origin, drop base to y=0
};

// Returns nullopt on failure (logged). Applies triangulation, normal generation,
// and a scale/skeleton probe pass that logs the raw bounds so the caller can spot
// the classic cm/m mismatch before it ships.
std::optional<StaticModel> loadStaticModel(const std::filesystem::path& path,
                                           const ModelImportOptions& opts = {});

// Axis-aligned world-space box that encloses a model's local AABB [localMin,
// localMax] after `transform`. Transforms all 8 corners — exact for a pure
// translation, conservative under rotation/scale. Server and client size a prop's
// static box collider identically with this so prediction matches authority.
void transformedAabb(const glm::mat4& transform, const glm::vec3& localMin,
                     const glm::vec3& localMax, glm::vec3& outCenter, glm::vec3& outHalfExtents);

} // namespace meat
