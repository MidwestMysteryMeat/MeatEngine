#include "engine/asset/SkeletalModel.h"
#include "engine/core/Log.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <glm/common.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <unordered_map>
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

// Reduce a bone/channel name to a match key: drop everything up to and including the
// last ':' (FBX namespace like "mixamorig:" or "Armature:"), so the same joint matches
// whether or not the exporter namespaced it. Bone names are otherwise case-sensitive.
std::string normalizeBoneName(const std::string& n) {
    const auto colon = n.rfind(':');
    return colon == std::string::npos ? n : n.substr(colon + 1);
}

// Is this a finger joint? Driving fingers across reduced/variant mixamorig rigs (whose
// finger bind orientations rarely match a Mixamo clip's) collapses them into a shard, so
// clip-merge leaves fingers at their relaxed bind. The wrist (LeftHand/RightHand) MUST stay
// animated — skinning weights straddle wrist+forearm, so freezing the wrist while the
// forearm swings tears the joint. (Variant rigs whose WRIST bind also mismatches Mixamo —
// e.g. the 28-bone PSX rig — can't be clip-merged cleanly and need retargetClipsFromFile.)
bool isFingerBone(const std::string& name) {
    for (const char* f : {"Thumb", "Index", "Middle", "Ring", "Pinky"}) {
        if (name.find(f) != std::string::npos) return true;
    }
    return false;
}

// Pure rotation of a transform (strip translation + per-axis scale). Rigs are
// shear-free, so normalizing the basis columns and quat_cast is exact.
glm::quat rotationOf(const glm::mat4& m) {
    glm::mat3 r(m);
    const float sx = glm::length(r[0]), sy = glm::length(r[1]), sz = glm::length(r[2]);
    if (sx > 0.0f) r[0] /= sx;
    if (sy > 0.0f) r[1] /= sy;
    if (sz > 0.0f) r[2] /= sz;
    return glm::normalize(glm::quat_cast(r));
}

glm::quat aiToQuat(const aiQuaternion& q) {
    return glm::normalize(glm::quat(q.w, q.x, q.y, q.z));
}

// Interpolate an Assimp rotation channel at time t (ticks). Clamps at the ends.
glm::quat sampleChannelRot(const aiNodeAnim& ch, float t) {
    const unsigned n = ch.mNumRotationKeys;
    if (n == 0) return glm::quat(1, 0, 0, 0);
    if (n == 1 || t <= static_cast<float>(ch.mRotationKeys[0].mTime)) {
        return aiToQuat(ch.mRotationKeys[0].mValue);
    }
    if (t >= static_cast<float>(ch.mRotationKeys[n - 1].mTime)) {
        return aiToQuat(ch.mRotationKeys[n - 1].mValue);
    }
    for (unsigned k = 0; k + 1 < n; ++k) {
        const auto& a = ch.mRotationKeys[k];
        const auto& b = ch.mRotationKeys[k + 1];
        if (t < static_cast<float>(b.mTime)) {
            const float span = static_cast<float>(b.mTime - a.mTime);
            const float f = span > 1e-6f ? (t - static_cast<float>(a.mTime)) / span : 0.0f;
            return glm::normalize(glm::slerp(aiToQuat(a.mValue), aiToQuat(b.mValue), f));
        }
    }
    return aiToQuat(ch.mRotationKeys[n - 1].mValue);
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

int appendClipsFromFile(SkeletalModel& model, const std::filesystem::path& animFile,
                        const ModelImportOptions& opts) {
    (void)opts; // rotation-only merge takes no scale; kept for API symmetry
    if (model.bones.empty()) {
        log::error("appendClipsFromFile: target model has no skeleton");
        return 0;
    }

    Assimp::Importer importer;
    // Same pivot collapse as loadSkeletalModel: with pivots preserved, FBX animation
    // channels target $AssimpFbx$ pseudo-nodes and never match the bone names.
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    // Animation-only import: we need the node graph + animations, not geometry.
    const aiScene* scene = importer.ReadFile(animFile.string(), 0);
    if (!scene || !scene->mRootNode) {
        log::error("appendClipsFromFile '{}' failed: {}", animFile.string(),
                   importer.GetErrorString());
        return 0;
    }
    if (scene->mNumAnimations == 0) {
        log::warn("appendClipsFromFile '{}': file has no animations", animFile.string());
        return 0;
    }

    // Normalized-name → bone index, built once from the target. Exact names are already
    // in model.boneByName; this table catches the namespace-mismatch case.
    std::unordered_map<std::string, int> normToBone;
    normToBone.reserve(model.bones.size());
    for (int b = 0; b < static_cast<int>(model.bones.size()); ++b) {
        normToBone.emplace(normalizeBoneName(model.bones[b].name), b); // first wins
    }
    const auto resolveBone = [&](const std::string& channelName) -> int {
        if (const auto it = model.boneByName.find(channelName); it != model.boneByName.end()) {
            return it->second; // exact (mixamorig:Hips → mixamorig:Hips)
        }
        if (const auto it = normToBone.find(normalizeBoneName(channelName));
            it != normToBone.end()) {
            return it->second; // namespace-normalized (Hips → mixamorig:Hips)
        }
        return -1;
    };

    int appended = 0;
    int totalDropped = 0;
    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation& anim = *scene->mAnimations[a];
        AnimClip clip;
        clip.name = anim.mName.length > 0 ? anim.mName.C_Str()
                                          : std::format("{}#{}", animFile.stem().string(), a);
        clip.duration = static_cast<float>(anim.mDuration);
        clip.ticksPerSec =
            anim.mTicksPerSecond > 0.0 ? static_cast<float>(anim.mTicksPerSecond) : 25.0f;

        int droppedThisClip = 0;
        for (unsigned c = 0; c < anim.mNumChannels; ++c) {
            const aiNodeAnim& ch = *anim.mChannels[c];
            const int boneIdx = resolveBone(ch.mNodeName.C_Str());
            if (boneIdx < 0) {
                ++droppedThisClip; // channel targets a bone the model doesn't have
                continue;
            }
            if (isFingerBone(model.bones[boneIdx].name)) {
                continue; // leave fingers at relaxed bind (variant rigs mangle when driven)
            }
            BoneTrack track;
            track.boneIndex = boneIdx;
            // ROTATION-ONLY merge. Bone names can match while the source skeleton's bind
            // scale/proportions differ (MoCap Online's hips sit at a different height/unit
            // than this rig), so copying absolute POSITION keys flings the mesh off-screen
            // and copying SCALE keys distorts it. We keep only rotations and leave position
            // + scale empty; samplePose then gap-fills those from THIS model's nodeBindLocal
            // (identity delta), so every bone keeps its own bind translation/bone-length and
            // only the animated rotation is applied — the standard cross-skeleton rotation
            // retarget, robust to proportion differences. (Root locomotion / hip bob are lost;
            // full-fidelity same-skeleton position transfer is the retargeter's job.)
            track.rotations.reserve(ch.mNumRotationKeys);
            for (unsigned k = 0; k < ch.mNumRotationKeys; ++k) {
                const aiQuatKey& key = ch.mRotationKeys[k];
                track.rotations.push_back(
                    {static_cast<float>(key.mTime),
                     glm::normalize(glm::quat(key.mValue.w, key.mValue.x, key.mValue.y,
                                              key.mValue.z))});
            }
            if (track.rotations.empty()) {
                ++droppedThisClip; // a position/scale-only channel drives nothing here
                continue;
            }
            clip.tracks.push_back(std::move(track));
        }

        if (clip.tracks.empty()) {
            log::warn("appendClipsFromFile '{}': clip '{}' matched 0 of {} channels — "
                      "skeleton mismatch? (needs a cross-skeleton retarget)",
                      animFile.filename().string(), clip.name, anim.mNumChannels);
            totalDropped += droppedThisClip;
            continue;
        }
        totalDropped += droppedThisClip;
        model.clips.push_back(std::move(clip));
        ++appended;
    }

    if (totalDropped > 0) {
        log::warn("appendClipsFromFile '{}': dropped {} channels with no matching bone",
                  animFile.filename().string(), totalDropped);
    }
    log::info("appendClipsFromFile '{}': +{} clips (model now {} clips)",
              animFile.filename().string(), appended, model.clips.size());
    return appended;
}

// WIP (opt-in via --animretarget): the rest-relative global-delta retarget below is the
// standard technique, but cross-skeleton WORLD-FRAME ALIGNMENT is not yet solved — a
// MoCap Online walk still distorts the mixamorig SWAT (the source node space and the
// mesh/offset space differ by an axis baseline the per-joint own-rest measurement doesn't
// fully cancel). appendClipsFromFile (identical-bind, e.g. a true Mixamo clip) is the
// working path; this needs the alignment fix before it renders clean.
int retargetClipsFromFile(SkeletalModel& model, const std::filesystem::path& animFile,
                          const ModelImportOptions& opts) {
    (void)opts;
    if (model.bones.empty()) {
        log::error("retargetClipsFromFile: target model has no skeleton");
        return 0;
    }
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* scene = importer.ReadFile(animFile.string(), 0);
    if (!scene || !scene->mRootNode) {
        log::error("retargetClipsFromFile '{}' failed: {}", animFile.string(),
                   importer.GetErrorString());
        return 0;
    }
    if (scene->mNumAnimations == 0) {
        log::warn("retargetClipsFromFile '{}': file has no animations", animFile.string());
        return 0;
    }

    // --- Source skeleton rest data (iterative pre-order: parents before children) ---
    struct SrcNode {
        std::string name;
        int parent = -1;
        glm::quat localRestRot{1, 0, 0, 0};
        glm::quat restGlobalRot{1, 0, 0, 0};
    };
    std::vector<SrcNode> src;
    std::unordered_map<std::string, int> srcExact, srcNorm;
    std::vector<std::pair<const aiNode*, int>> stack{{scene->mRootNode, -1}};
    while (!stack.empty()) {
        const auto [node, parent] = stack.back();
        stack.pop_back();
        const int idx = static_cast<int>(src.size());
        SrcNode sn;
        sn.name = node->mName.C_Str();
        sn.parent = parent;
        sn.localRestRot = rotationOf(toGlm(node->mTransformation));
        sn.restGlobalRot = parent >= 0 ? src[parent].restGlobalRot * sn.localRestRot
                                       : sn.localRestRot;
        src.push_back(sn);
        srcExact.emplace(sn.name, idx);
        srcNorm.emplace(normalizeBoneName(sn.name), idx); // first wins
        for (unsigned c = 0; c < node->mNumChildren; ++c) {
            stack.emplace_back(node->mChildren[c], idx);
        }
    }
    const auto resolveSrc = [&](const std::string& tgt) -> int {
        if (const auto it = srcExact.find(tgt); it != srcExact.end()) return it->second;
        if (const auto it = srcNorm.find(normalizeBoneName(tgt)); it != srcNorm.end()) {
            return it->second;
        }
        return -1;
    };

    // --- Target rest data (model.bones is already topological) ---
    const std::size_t nb = model.bones.size();
    std::vector<glm::quat> tgtLocalRest(nb), tgtGlobalRest(nb), tgtNodeBind(nb), tgtPreRot(nb);
    for (std::size_t b = 0; b < nb; ++b) {
        const Bone& bone = model.bones[b];
        tgtLocalRest[b] = rotationOf(bone.localBind);
        tgtNodeBind[b] = rotationOf(bone.nodeBindLocal);
        tgtPreRot[b] = rotationOf(bone.pre);
        tgtGlobalRest[b] = bone.parent >= 0
                               ? tgtGlobalRest[bone.parent] * tgtPreRot[b] * tgtLocalRest[b]
                               : tgtPreRot[b] * tgtLocalRest[b];
    }

    int appended = 0;
    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation& anim = *scene->mAnimations[a];
        std::vector<const aiNodeAnim*> srcChan(src.size(), nullptr);
        for (unsigned c = 0; c < anim.mNumChannels; ++c) {
            const aiNodeAnim* ch = anim.mChannels[c];
            if (const auto it = srcExact.find(ch->mNodeName.C_Str()); it != srcExact.end()) {
                srcChan[it->second] = ch;
            }
        }
        const float srcTps =
            anim.mTicksPerSecond > 0.0 ? static_cast<float>(anim.mTicksPerSecond) : 25.0f;
        const float durSec = static_cast<float>(anim.mDuration) / srcTps;
        constexpr int kFps = 30;
        const int frames = std::max(2, static_cast<int>(std::ceil(durSec * kFps)));

        AnimClip clip;
        clip.name = anim.mName.length > 0 ? anim.mName.C_Str()
                                          : std::format("{}#{}", animFile.stem().string(), a);
        clip.ticksPerSec = static_cast<float>(kFps);
        clip.duration = static_cast<float>(frames - 1);

        std::vector<BoneTrack> tracks(nb);
        std::vector<bool> mapped(nb, false);
        for (std::size_t b = 0; b < nb; ++b) tracks[b].boneIndex = static_cast<int>(b);

        std::vector<glm::quat> srcGlobal(src.size()), rtWorld(nb);
        for (int f = 0; f < frames; ++f) {
            const float tTicks = static_cast<float>(f) / kFps * srcTps;
            for (std::size_t i = 0; i < src.size(); ++i) {
                glm::quat local = src[i].localRestRot;
                if (const aiNodeAnim* ch = srcChan[i]; ch && ch->mNumRotationKeys > 0) {
                    local = sampleChannelRot(*ch, tTicks);
                }
                srcGlobal[i] =
                    src[i].parent >= 0 ? srcGlobal[src[i].parent] * local : local;
            }
            for (std::size_t b = 0; b < nb; ++b) {
                const int p = model.bones[b].parent;
                const glm::quat parentW = p >= 0 ? rtWorld[p] : glm::quat(1, 0, 0, 0);
                const int si = resolveSrc(model.bones[b].name);
                glm::quat rtw;
                if (si >= 0 && srcChan[si]) {
                    // World motion the source joint underwent since its own rest, applied
                    // to the target's own rest global — the fixed rest/axis offset cancels.
                    const glm::quat dR = srcGlobal[si] * glm::inverse(src[si].restGlobalRot);
                    rtw = glm::normalize(dR * tgtGlobalRest[b]);
                    mapped[b] = true;
                } else {
                    rtw = parentW * tgtPreRot[b] * tgtLocalRest[b]; // unmapped: hold rest
                }
                rtWorld[b] = rtw;
                const glm::quat rtLocal = glm::inverse(parentW * tgtPreRot[b]) * rtw;
                const glm::quat deltaLocal = glm::inverse(tgtLocalRest[b]) * rtLocal;
                // samplePose does local = localBind * nodeBindLocalInv * compose(key); with
                // key rotation = nodeBind*deltaLocal and pos/scale gap-filled from nodeBind,
                // that reduces to localBind * deltaLocal — bind at rest, retargeted in motion.
                tracks[b].rotations.push_back(
                    {static_cast<float>(f), glm::normalize(tgtNodeBind[b] * deltaLocal)});
            }
        }
        for (std::size_t b = 0; b < nb; ++b) {
            if (mapped[b] && !tracks[b].rotations.empty()) {
                clip.tracks.push_back(std::move(tracks[b]));
            }
        }
        if (clip.tracks.empty()) {
            log::warn("retargetClipsFromFile '{}': clip '{}' mapped 0 bones",
                      animFile.filename().string(), clip.name);
            continue;
        }
        model.clips.push_back(std::move(clip));
        ++appended;
    }

    log::info("retargetClipsFromFile '{}': +{} clips baked at 30fps (model now {} clips)",
              animFile.filename().string(), appended, model.clips.size());
    return appended;
}

} // namespace meat
