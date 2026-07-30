#pragma once
#include "engine/voxel/ChunkMesher.h" // reuse VoxelVertex/ChunkMeshData for the mesh path

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

} // namespace meat
