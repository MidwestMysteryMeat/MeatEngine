#include "engine/asset/SkeletalModel.h"
#include "engine/core/Log.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <glm/common.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace meat {
namespace {

// aiMatrix4x4 stores rows contiguously (row-major); glm stores columns.
// make_mat4 reads the 16 floats straight into columns, so one transpose
// yields the same mathematical matrix on the glm side.
glm::mat4 toGlm(const aiMatrix4x4& m) {
    return glm::transpose(glm::make_mat4(&m.a1));
}

// Uniform-scale conjugation S*M*S⁻¹: rotation/scale block untouched,
// translation multiplied. Because conjugation distributes over products,
// applying it to EVERY factor (offsets, local binds, prefix nodes, position
// keys, vertex positions) keeps the whole skinning chain consistent in meters.
glm::mat4 scaleTranslation(glm::mat4 m, float scale) {
    m[3] = glm::vec4(glm::vec3(m[3]) * scale, m[3].w);
    return m;
}

struct SkeletonInputs {
    std::unordered_map<std::string, glm::mat4> offsets; // weighted bones only (scaled)
    std::unordered_set<std::string> channelNames;       // nodes any clip animates
};

bool subtreeHasWeightedBone(const aiNode& node, const SkeletonInputs& in) {
    if (in.offsets.contains(node.mName.C_Str())) {
        return true;
    }
    for (unsigned c = 0; c < node.mNumChildren; ++c) {
        if (subtreeHasWeightedBone(*node.mChildren[c], in)) {
            return true;
        }
    }
    return false;
}

// Pre-order walk ⇒ parents always precede children in the flat array. A node is
// a bone if it carries vertex weights, or if it is animated and sits above
// weighted bones (Mixamo animates the armature root). We track the TRUE bind
// global (accumulated node transforms from the scene root, scale-conjugated) for
// every bone in `pre` temporarily; a post-pass converts it to the parent-bone-
// relative bind. Deriving the relative transform from true globals telescopes to
// identity at bind exactly — the earlier pre*localBind reconstruction accumulated
// error down the chain (fingers drifted ~100x). `worldSoFar` is that running
// global; it includes intermediate non-bone nodes automatically.
void buildSkeleton(const aiNode& node, int parentBone, const glm::mat4& worldSoFar,
                   float scale, const SkeletonInputs& in, SkeletalModel& out) {
    const std::string name = node.mName.C_Str();
    const glm::mat4 world = worldSoFar * scaleTranslation(toGlm(node.mTransformation), scale);
    const auto offsetIt = in.offsets.find(name);
    const bool isBone = (offsetIt != in.offsets.end() ||
                         (in.channelNames.contains(name) && subtreeHasWeightedBone(node, in))) &&
                        !out.boneByName.contains(name); // duplicate names: first wins
    int childParent = parentBone;
    if (isBone) {
        Bone bone;
        bone.name = name;
        bone.parent = parentBone;
        bone.pre = world; // stash the true bind global; relativized post-walk
        bone.localBind = glm::mat4(1.0f);
        bone.nodeBindLocal = toGlm(node.mTransformation); // raw node local (anim delta ref)
        bone.nodeBindLocalInv = glm::inverse(bone.nodeBindLocal); // constant; used per-frame
        bone.offset = offsetIt != in.offsets.end() ? offsetIt->second : glm::mat4(1.0f);
        childParent = static_cast<int>(out.bones.size());
        out.boneByName.emplace(name, childParent);
        out.bones.push_back(std::move(bone));
    }
    for (unsigned c = 0; c < node.mNumChildren; ++c) {
        buildSkeleton(*node.mChildren[c], childParent, world, scale, in, out);
    }
}

// Reconstruct each bone's bind GLOBAL, then convert it to the parent-bone-relative
// local stored in `localBind` (with `pre` reset to identity). resolve() composes
// global[b] = global[parent] * localBind[b], telescoping back to the bind global,
// so skinning[b] = rootInverse * global[b] * offset[b] collapses to rootInverse
// (identity for this rig) at bind — reproducing the mesh exactly.
//
// The authoritative bind global for a WEIGHTED bone is inverse(offset): the offset
// (inverse-bind) matrix comes straight from the skin deformer, so inverse(offset[b])
// IS that bone's bind global in the mesh's own space, and inverse(offset)*offset is
// identity by construction. The node-transform chain stashed in `pre` is NOT
// reliable here: the armature root node carries a ~100x unit scale that Assimp bakes
// into the offset matrices but distributes differently through node.mTransformation,
// so accumulating nodes drifts — worse the deeper the bone (fingers went ~200x off).
// Promoted bones (armature root: animated, no vertex weights, offset=identity) have
// no deformer bind, so they keep their node-accumulated global from `pre`; they skin
// nothing and telescope out of every weighted child's chain, so their value is inert.
void relativizeSkeleton(SkeletalModel& out, const SkeletonInputs& in) {
    std::vector<glm::mat4> bindGlobal(out.bones.size());
    for (std::size_t b = 0; b < out.bones.size(); ++b) {
        const Bone& bone = out.bones[b];
        bindGlobal[b] = in.offsets.contains(bone.name) ? glm::inverse(bone.offset) : bone.pre;
    }
    for (std::size_t b = 0; b < out.bones.size(); ++b) {
        const int p = out.bones[b].parent;
        out.bones[b].localBind = p >= 0 ? glm::inverse(bindGlobal[static_cast<std::size_t>(p)]) *
                                              bindGlobal[b]
                                        : bindGlobal[b];
        out.bones[b].pre = glm::mat4(1.0f);
    }
}

struct EmitStats {
    glm::vec3 lo{std::numeric_limits<float>::max()};
    glm::vec3 hi{std::numeric_limits<float>::lowest()};
    int zeroWeightVerts = 0;
    int rigidMeshes = 0;
    int skippedMeshes = 0;
};

// Skinned vertices are emitted as-authored (bind space) — the offset matrices
// already account for the mesh node's transform, so baking it in (like the
// static loader does) would double-apply it. Rigid meshes in a skeletal file
// (props parented under a joint) get their bind-global transform baked and a
// single full-weight binding to the nearest ancestor bone: the skin matrix
// G_b * O_b applied to globalBind(node) * v reduces to G_b * bindChain(b→node)
// * v, which is exactly rigid attachment.
void appendMeshes(const aiScene& scene, const aiNode& node, const aiMatrix4x4& parentWorld,
                  int nearestBone, float scale, bool flipUV, SkeletalModel& out,
                  EmitStats& stats) {
    const aiMatrix4x4 world = parentWorld * node.mTransformation;
    if (const auto it = out.boneByName.find(node.mName.C_Str()); it != out.boneByName.end()) {
        nearestBone = it->second;
    }
    aiMatrix3x3 normalMat(world);
    normalMat.Inverse().Transpose();

    for (unsigned m = 0; m < node.mNumMeshes; ++m) {
        const aiMesh& mesh = *scene.mMeshes[node.mMeshes[m]];
        const bool skinned = mesh.HasBones();
        if (!skinned && nearestBone < 0) {
            ++stats.skippedMeshes; // nothing to attach it to; skinning would warp it
            continue;
        }
        if (!skinned) {
            ++stats.rigidMeshes;
        }

        // Gather all influences per vertex, then keep the 4 largest.
        std::vector<std::vector<std::pair<int, float>>> influences(mesh.mNumVertices);
        for (unsigned b = 0; b < mesh.mNumBones; ++b) {
            const aiBone& bone = *mesh.mBones[b];
            const auto boneIt = out.boneByName.find(bone.mName.C_Str());
            if (boneIt == out.boneByName.end()) {
                continue; // name never appeared as a node; nothing can drive it
            }
            for (unsigned w = 0; w < bone.mNumWeights; ++w) {
                const aiVertexWeight& vw = bone.mWeights[w];
                if (vw.mVertexId < mesh.mNumVertices && vw.mWeight > 0.0f) {
                    influences[vw.mVertexId].emplace_back(boneIt->second, vw.mWeight);
                }
            }
        }

        const auto base = static_cast<std::uint32_t>(out.vertices.size());
        for (unsigned v = 0; v < mesh.mNumVertices; ++v) {
            SkinnedVertex vert{};
            if (skinned) {
                const aiVector3D& p = mesh.mVertices[v];
                vert.pos = glm::vec3(p.x, p.y, p.z) * scale;
                const aiVector3D n =
                    mesh.HasNormals() ? mesh.mNormals[v] : aiVector3D(0.0f, 1.0f, 0.0f);
                vert.normal = glm::vec3(n.x, n.y, n.z);
            } else {
                const aiVector3D p = world * mesh.mVertices[v];
                vert.pos = glm::vec3(p.x, p.y, p.z) * scale;
                aiVector3D n =
                    mesh.HasNormals() ? (normalMat * mesh.mNormals[v]) : aiVector3D(0.0f, 1.0f, 0.0f);
                n.NormalizeSafe();
                vert.normal = glm::vec3(n.x, n.y, n.z);
            }
            if (mesh.HasTextureCoords(0)) {
                vert.uv = {mesh.mTextureCoords[0][v].x,
                           flipUV ? 1.0f - mesh.mTextureCoords[0][v].y
                                  : mesh.mTextureCoords[0][v].y};
            }

            vert.bones = glm::ivec4(0);
            vert.weights = glm::vec4(0.0f);
            if (skinned) {
                auto& inf = influences[v];
                std::sort(inf.begin(), inf.end(),
                          [](const auto& a, const auto& b) { return a.second > b.second; });
                const std::size_t count = std::min<std::size_t>(inf.size(), 4);
                float total = 0.0f;
                for (std::size_t i = 0; i < count; ++i) {
                    total += inf[i].second;
                }
                if (total > 0.0f) {
                    for (std::size_t i = 0; i < count; ++i) {
                        vert.bones[static_cast<glm::length_t>(i)] = inf[i].first;
                        vert.weights[static_cast<glm::length_t>(i)] = inf[i].second / total;
                    }
                } else {
                    vert.weights.x = 1.0f; // orphan vertex: pin to bone 0, count it
                    ++stats.zeroWeightVerts;
                }
            } else {
                vert.bones.x = nearestBone;
                vert.weights.x = 1.0f;
            }

            stats.lo = glm::min(stats.lo, vert.pos);
            stats.hi = glm::max(stats.hi, vert.pos);
            out.vertices.push_back(vert);
        }
        for (unsigned f = 0; f < mesh.mNumFaces; ++f) {
            const aiFace& face = mesh.mFaces[f];
            if (face.mNumIndices != 3) {
                continue; // triangulated already; skip degenerates
            }
            for (unsigned i = 0; i < 3; ++i) {
                out.indices.push_back(base + face.mIndices[i]);
            }
        }
    }
    for (unsigned c = 0; c < node.mNumChildren; ++c) {
        appendMeshes(scene, *node.mChildren[c], world, nearestBone, scale, flipUV, out, stats);
    }
}

// Same convention as the static loader (ModelLoader.cpp): absolute path if it
// exists, else look next to the model file — exporters embed odd paths.
std::filesystem::path findAlbedo(const aiScene& scene, const std::filesystem::path& modelPath) {
    for (unsigned m = 0; m < scene.mNumMaterials; ++m) {
        aiString tex;
        if (scene.mMaterials[m]->GetTexture(aiTextureType_DIFFUSE, 0, &tex) == AI_SUCCESS ||
            scene.mMaterials[m]->GetTexture(aiTextureType_BASE_COLOR, 0, &tex) == AI_SUCCESS) {
            std::filesystem::path p(tex.C_Str());
            if (p.is_absolute() && std::filesystem::exists(p)) {
                return p;
            }
            const auto rel = modelPath.parent_path() / p.filename();
            if (std::filesystem::exists(rel)) {
                return rel;
            }
        }
    }
    return {};
}

} // namespace

std::optional<SkeletalModel> loadSkeletalModel(const std::filesystem::path& path,
                                               const ModelImportOptions& opts) {
    Assimp::Importer importer;
    // Collapse FBX pivot pseudo-nodes ($AssimpFbx$_*): with pivots preserved,
    // animation channels target pseudo-node names that never match the bone
    // nodes, and nothing animates. Mixamo exports are unaffected by the
    // collapse; exotic pivot animation would be, so it stays documented here.
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const unsigned flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals |
                           aiProcess_JoinIdenticalVertices | aiProcess_ImproveCacheLocality |
                           aiProcess_LimitBoneWeights | // trims to 4 before we even see them
                           aiProcess_FindDegenerates | aiProcess_FindInvalidData;
    const aiScene* scene = importer.ReadFile(path.string(), flags);
    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        log::error("skeletal model '{}' failed: {}", path.string(), importer.GetErrorString());
        return std::nullopt;
    }

    // Pass 1: weighted bone names → scaled offset matrices, plus animated
    // node names. Both feed the skeleton walk.
    SkeletonInputs inputs;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh& mesh = *scene->mMeshes[m];
        for (unsigned b = 0; b < mesh.mNumBones; ++b) {
            const aiBone& bone = *mesh.mBones[b];
            const glm::mat4 offset = scaleTranslation(toGlm(bone.mOffsetMatrix), opts.scale);
            const auto [it, inserted] = inputs.offsets.emplace(bone.mName.C_Str(), offset);
            if (!inserted) {
                // Two meshes bound at different bind transforms would need
                // per-mesh offsets; first one wins, so surface the drift.
                const glm::mat4 diff = it->second - offset;
                float maxDiff = 0.0f;
                for (glm::length_t col = 0; col < 4; ++col) {
                    for (glm::length_t row = 0; row < 4; ++row) {
                        maxDiff = std::max(maxDiff, std::abs(diff[col][row]));
                    }
                }
                if (maxDiff > 1e-3f) {
                    log::warn("skeletal model '{}': bone '{}' has conflicting offset matrices "
                              "across meshes (max diff {:.4f}) — first wins",
                              path.filename().string(), bone.mName.C_Str(), maxDiff);
                }
            }
        }
    }
    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation& anim = *scene->mAnimations[a];
        for (unsigned c = 0; c < anim.mNumChannels; ++c) {
            inputs.channelNames.insert(anim.mChannels[c]->mNodeName.C_Str());
        }
    }
    if (inputs.offsets.empty()) {
        log::error("skeletal model '{}' has no bones — use loadStaticModel", path.string());
        return std::nullopt;
    }

    // Pass 2: skeleton in pre-order (parents before children), then relativize
    // the stashed bind globals into parent-relative locals.
    SkeletalModel model;
    buildSkeleton(*scene->mRootNode, -1, glm::mat4(1.0f), opts.scale, inputs, model);
    relativizeSkeleton(model, inputs);
    if (model.bones.size() > static_cast<std::size_t>(kMaxBones)) {
        log::error("skeletal model '{}': {} bones exceeds kMaxBones={}", path.string(),
                   model.bones.size(), kMaxBones);
        return std::nullopt;
    }

    // Pass 3: geometry. opts.center is meaningless here — shifting bind-space
    // vertices without adjusting every offset matrix breaks skinning.
    if (opts.center) {
        log::warn("skeletal model '{}': ModelImportOptions.center ignored for skinned meshes",
                  path.filename().string());
    }
    EmitStats stats;
    appendMeshes(*scene, *scene->mRootNode, aiMatrix4x4(), -1, opts.scale, opts.flipUV, model,
                 stats);
    if (model.vertices.empty()) {
        log::error("skeletal model '{}' has no geometry", path.string());
        return std::nullopt;
    }
    model.boundsMin = stats.lo;
    model.boundsMax = stats.hi;
    // Undo the scene-root node transform (scale-conjugated to match the chain).
    model.rootInverse =
        glm::inverse(scaleTranslation(toGlm(scene->mRootNode->mTransformation), opts.scale));
    model.albedo = findAlbedo(*scene, path);
    if (stats.zeroWeightVerts > 0) {
        log::warn("skeletal model '{}': {} vertices had no weights — pinned to bone 0",
                  path.filename().string(), stats.zeroWeightVerts);
    }
    if (stats.skippedMeshes > 0) {
        log::warn("skeletal model '{}': {} rigid meshes above every bone were skipped",
                  path.filename().string(), stats.skippedMeshes);
    }

    // Pass 4: every animation → a clip. Key times stay in ticks; position keys
    // get the same scale treatment as the rest of the transform chain.
    int orphanChannels = 0;
    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation& anim = *scene->mAnimations[a];
        AnimClip clip;
        clip.name = anim.mName.length > 0 ? anim.mName.C_Str() : std::format("clip{}", a);
        clip.duration = static_cast<float>(anim.mDuration);
        clip.ticksPerSec =
            anim.mTicksPerSecond > 0.0 ? static_cast<float>(anim.mTicksPerSecond) : 25.0f;
        for (unsigned c = 0; c < anim.mNumChannels; ++c) {
            const aiNodeAnim& ch = *anim.mChannels[c];
            const auto boneIt = model.boneByName.find(ch.mNodeName.C_Str());
            if (boneIt == model.boneByName.end()) {
                ++orphanChannels; // animates a node that is not part of the skeleton
                continue;
            }
            BoneTrack track;
            track.boneIndex = boneIt->second;
            track.positions.reserve(ch.mNumPositionKeys);
            for (unsigned k = 0; k < ch.mNumPositionKeys; ++k) {
                const aiVectorKey& key = ch.mPositionKeys[k];
                track.positions.push_back(
                    {static_cast<float>(key.mTime),
                     glm::vec3(key.mValue.x, key.mValue.y, key.mValue.z) * opts.scale});
            }
            track.rotations.reserve(ch.mNumRotationKeys);
            for (unsigned k = 0; k < ch.mNumRotationKeys; ++k) {
                const aiQuatKey& key = ch.mRotationKeys[k];
                track.rotations.push_back(
                    {static_cast<float>(key.mTime),
                     glm::normalize(glm::quat(key.mValue.w, key.mValue.x, key.mValue.y,
                                              key.mValue.z))});
            }
            track.scales.reserve(ch.mNumScalingKeys);
            for (unsigned k = 0; k < ch.mNumScalingKeys; ++k) {
                const aiVectorKey& key = ch.mScalingKeys[k];
                track.scales.push_back({static_cast<float>(key.mTime),
                                        glm::vec3(key.mValue.x, key.mValue.y, key.mValue.z)});
            }
            clip.tracks.push_back(std::move(track));
        }
        model.clips.push_back(std::move(clip));
    }
    if (orphanChannels > 0) {
        log::warn("skeletal model '{}': {} animation channels target non-skeleton nodes "
                  "(root motion on a stray node?) — dropped",
                  path.filename().string(), orphanChannels);
    }

    // Scale probe: same law as the static loader — a humanoid arriving ~180
    // units tall means an un-applied cm→m scale.
    const glm::vec3 size = model.boundsMax - model.boundsMin;
    log::info("skeletal model '{}': {} verts, {} bones, {} clips, bounds "
              "{:.2f}x{:.2f}x{:.2f} m{}",
              path.filename().string(), model.vertices.size(), model.bones.size(),
              model.clips.size(), size.x, size.y, size.z,
              size.y > 20.0f ? " [WARN huge — needs scale ~0.01?]" : "");
    for (const AnimClip& clip : model.clips) {
        log::info("  clip '{}': {:.2f} s ({} tracks)", clip.name,
                  clip.duration / clip.ticksPerSec, clip.tracks.size());
    }
    return model;
}

} // namespace meat
