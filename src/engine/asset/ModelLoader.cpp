#include "engine/asset/ModelLoader.h"
#include "engine/core/Log.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <glm/common.hpp>

#include <limits>

namespace meat {
namespace {

glm::i8vec3 quantizeNormal(const aiVector3D& n) {
    // Normals are unit-length; map [-1,1] → [-127,127] for the i8 attribute.
    return glm::i8vec3(glm::clamp(glm::vec3(n.x, n.y, n.z) * 127.0f, -127.0f, 127.0f));
}

// Walk the node tree so child-mesh transforms bake into world positions (FBX
// rigs nest meshes under bones/nodes with non-identity transforms).
void appendNode(const aiScene& scene, const aiNode& node, const aiMatrix4x4& parent,
                float scale, bool flipUV, ChunkMeshData& out, glm::vec3& lo, glm::vec3& hi) {
    const aiMatrix4x4 world = parent * node.mTransformation;
    aiMatrix3x3 normalMat(world);
    normalMat.Inverse().Transpose();

    for (unsigned m = 0; m < node.mNumMeshes; ++m) {
        const aiMesh& mesh = *scene.mMeshes[node.mMeshes[m]];
        const auto base = static_cast<std::uint32_t>(out.vertices.size());
        for (unsigned v = 0; v < mesh.mNumVertices; ++v) {
            aiVector3D p = world * mesh.mVertices[v];
            const glm::vec3 pos(p.x * scale, p.y * scale, p.z * scale);
            lo = glm::min(lo, pos);
            hi = glm::max(hi, pos);
            aiVector3D n = mesh.HasNormals() ? (normalMat * mesh.mNormals[v]).Normalize()
                                             : aiVector3D(0, 1, 0);
            glm::vec2 uv(0.0f);
            if (mesh.HasTextureCoords(0))
                uv = {mesh.mTextureCoords[0][v].x,
                      flipUV ? 1.0f - mesh.mTextureCoords[0][v].y : mesh.mTextureCoords[0][v].y};
            out.vertices.push_back({pos, quantizeNormal(n), uv, 0});
        }
        for (unsigned f = 0; f < mesh.mNumFaces; ++f) {
            const aiFace& face = mesh.mFaces[f];
            if (face.mNumIndices != 3) continue; // triangulated already; skip degenerates
            for (unsigned i = 0; i < 3; ++i) out.indices.push_back(base + face.mIndices[i]);
        }
    }
    for (unsigned c = 0; c < node.mNumChildren; ++c)
        appendNode(scene, *node.mChildren[c], world, scale, flipUV, out, lo, hi);
}

std::filesystem::path findAlbedo(const aiScene& scene, const std::filesystem::path& modelPath) {
    for (unsigned m = 0; m < scene.mNumMaterials; ++m) {
        aiString tex;
        if (scene.mMaterials[m]->GetTexture(aiTextureType_DIFFUSE, 0, &tex) == AI_SUCCESS ||
            scene.mMaterials[m]->GetTexture(aiTextureType_BASE_COLOR, 0, &tex) == AI_SUCCESS) {
            std::filesystem::path p(tex.C_Str());
            if (p.is_absolute() && std::filesystem::exists(p)) return p;
            const auto rel = modelPath.parent_path() / p.filename();
            if (std::filesystem::exists(rel)) return rel; // exporters embed odd paths
        }
    }
    return {};
}

} // namespace

std::optional<StaticModel> loadStaticModel(const std::filesystem::path& path,
                                           const ModelImportOptions& opts) {
    Assimp::Importer importer;
    const unsigned flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals |
                           aiProcess_JoinIdenticalVertices | aiProcess_ImproveCacheLocality |
                           aiProcess_FindDegenerates | aiProcess_FindInvalidData;
    const aiScene* scene = importer.ReadFile(path.string(), flags);
    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        log::error("model '{}' failed: {}", path.string(), importer.GetErrorString());
        return std::nullopt;
    }

    StaticModel model;
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    appendNode(*scene, *scene->mRootNode, aiMatrix4x4(), opts.scale, opts.flipUV, model.mesh,
               lo, hi);
    if (model.mesh.vertices.empty()) {
        log::error("model '{}' has no geometry", path.string());
        return std::nullopt;
    }

    if (opts.center) { // drop to floor, center on XZ — props/pickups want origin at base
        const glm::vec3 shift((lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f);
        for (VoxelVertex& v : model.mesh.vertices) v.pos -= shift;
        hi -= shift;
        lo -= shift;
    }
    model.boundsMin = lo;
    model.boundsMax = hi;
    model.albedo = findAlbedo(*scene, path);

    // Scale probe: the OneLife "check bounds first" law. A humanoid ~1.8 m tall
    // arriving ~180 units means an un-applied cm→m scale — surface it loudly.
    const glm::vec3 size = hi - lo;
    log::info("model '{}': {} verts, bounds {:.2f}x{:.2f}x{:.2f} m{}", path.filename().string(),
              model.mesh.vertices.size(), size.x, size.y, size.z,
              size.y > 20.0f ? " [WARN huge — needs scale ~0.01?]" : "");
    if (scene->mNumAnimations > 0 || (scene->mNumMeshes > 0 && scene->mMeshes[0]->HasBones()))
        log::info("  (has skeleton/anim — static import drops it; skeletal is Phase 7b)");
    return model;
}

void transformedAabb(const glm::mat4& transform, const glm::vec3& localMin,
                     const glm::vec3& localMax, glm::vec3& outCenter, glm::vec3& outHalfExtents) {
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 p((corner & 1) ? localMax.x : localMin.x,
                          (corner & 2) ? localMax.y : localMin.y,
                          (corner & 4) ? localMax.z : localMin.z);
        const glm::vec3 world = glm::vec3(transform * glm::vec4(p, 1.0f));
        lo = glm::min(lo, world);
        hi = glm::max(hi, world);
    }
    outCenter = (lo + hi) * 0.5f;
    outHalfExtents = (hi - lo) * 0.5f;
}

} // namespace meat
