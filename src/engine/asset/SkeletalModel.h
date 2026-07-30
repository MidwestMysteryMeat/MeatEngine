#pragma once
#include "engine/asset/ModelLoader.h" // ModelImportOptions

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace meat {

// Hard cap on skeleton size: the skinned shader's uBones palette is sized to
// this, and Mixamo humanoids are ~65 bones — 128 leaves generous headroom.
inline constexpr int kMaxBones = 128;

// A skinned model loaded via Assimp (FBX/GLB). Unlike the static loader, node
// transforms are NOT baked into vertices: skinned vertices live in bind space
// and the offset (inverse-bind) matrices reproduce them. ModelImportOptions
// .scale is applied to positions AND to every translation in the transform
// chain (offsets, local binds, prefix nodes, position keys) — conjugating each
// factor by the uniform scale keeps the composed chain consistent.
struct SkinnedVertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::ivec4 bones;  // ≤4 influences; padded with bone 0
    glm::vec4 weights; // sums to 1; padded with 0
};

struct Bone {
    std::string name;
    int parent = -1;          // index into SkeletalModel::bones; -1 = root
    glm::mat4 offset{1.0f};   // inverse bind (aiBone::mOffsetMatrix, transposed)
    glm::mat4 localBind{1.0f}; // node's bind-pose local transform; the fallback
                               // when a clip has no track for this bone
    glm::mat4 pre{1.0f};       // product of NON-bone node transforms between this
                               // bone and its parent bone (or scene root); static,
                               // survives when a track replaces localBind
    glm::mat4 nodeBindLocal{1.0f}; // the bone NODE's own raw bind-pose local transform
                               // (what an animation channel's keys replace). localBind
                               // lives in the offset-authoritative space, so a clip key
                               // (node space) is applied as a delta from this:
                               // local = localBind * nodeBindLocalInv * animatedLocal
    glm::mat4 nodeBindLocalInv{1.0f}; // inverse(nodeBindLocal), precomputed (it is
                               // constant); used per-frame in samplePose's delta
};

struct VecKey {
    float time; // ticks
    glm::vec3 value;
};
struct QuatKey {
    float time; // ticks
    glm::quat value;
};

struct BoneTrack {
    int boneIndex = 0;
    std::vector<VecKey> positions;
    std::vector<QuatKey> rotations;
    std::vector<VecKey> scales;
};

struct AnimClip {
    std::string name;
    float duration = 0.0f;     // ticks
    float ticksPerSec = 25.0f; // Assimp default when the file says 0
    std::vector<BoneTrack> tracks;
};

struct SkeletalModel {
    std::vector<SkinnedVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Bone> bones; // topological: parents always precede children
    std::unordered_map<std::string, int> boneByName;
    std::vector<AnimClip> clips;
    std::filesystem::path albedo; // first diffuse texture found, or empty
    glm::vec3 boundsMin{0}, boundsMax{0}; // bind pose, scaled
    // Inverse of the scene-root node transform (scale-conjugated). Fab/UE FBX
    // exports bake a rotated/scaled root here; left-multiply this into the model
    // transform (uModel) so the actor isn't mis-oriented. Identity for clean rigs.
    glm::mat4 rootInverse{1.0f};
};

// Returns nullopt on failure (logged): no bones, >kMaxBones bones, or Assimp
// error. opts.center is ignored (shifting bind vertices would break the offset
// matrices); opts.scale and opts.flipUV behave like the static loader.
std::optional<SkeletalModel> loadSkeletalModel(const std::filesystem::path& path,
                                               const ModelImportOptions& opts = {});

// Load every animation clip from an animation-only file (Mixamo export, MoCap Online
// pack, etc.) and attach it to an existing model by matching bone names. Matching is
// exact first, then namespace-normalized ("mixamorig:Hips" <-> "Hips"), so any file
// whose skeleton shares the model's bone HIERARCHY works with no retargeting. Tracks
// whose bone is not in the model are dropped (counted in a warning). Returns the number
// of clips appended (0 on load failure or no match). opts.scale must match the scale the
// model itself was loaded with.
int appendClipsFromFile(SkeletalModel& model, const std::filesystem::path& animFile,
                        const ModelImportOptions& opts = {});

// Bake every animation from a FOREIGN-skeleton file (UE5 mannequin, MoCap Online, any
// rig with a different REST pose) onto the model's skeleton and append as native clips.
// Unlike appendClipsFromFile (which needs an identical bind pose), this compensates for
// the rest-pose / bone-axis difference by transferring each joint's world-orientation
// motion measured against its OWN rest global (the standard ozz/Assimp global-delta
// retarget), matched by bone name (exact then namespace-normalized). Rotation-only, baked
// at 30fps. Returns the number of clips appended (0 on failure or no mapped bones).
int retargetClipsFromFile(SkeletalModel& model, const std::filesystem::path& animFile,
                          const ModelImportOptions& opts = {});

} // namespace meat
