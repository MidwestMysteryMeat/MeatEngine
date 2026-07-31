#pragma once
#include "engine/asset/SkeletalModel.h"

#include <glm/glm.hpp>

#include <vector>

namespace meat {

// One evaluated frame of a skeleton, ready for the GPU. Entry b is
//   skinningMatrices[b] = globalTransform(b) * bones[b].offset
// where globalTransform composes parent-first:
//   global(b) = global(parent) * bones[b].pre * local(b)
// so applying it to a bind-space vertex yields a model-space (meters) vertex.
struct Pose {
    std::vector<glm::mat4> skinningMatrices;
};

// Local bone transform as translation / rotation / scale. The blend layer works in this
// space because slerp+lerp of TRS is rotation-correct, whereas lerping the composed mat4
// shears. No shear is assumed (true of rig exports; matches the sampler's gap-fill path).
struct Trs {
    glm::vec3 pos{0.0f};
    glm::quat rot{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scl{1.0f};
};
glm::mat4 compose(const Trs& t);
Trs decompose(const glm::mat4& m);

// Per-bone LOCAL transforms for a clip at a time (samplePose's body minus the final
// resolve): sample two clips with this, blend the arrays, resolve once.
std::vector<Trs> sampleLocalTrs(const SkeletalModel& model, const AnimClip& clip, float t);
Pose resolveLocalTrs(const SkeletalModel& model, const std::vector<Trs>& locals);

// Per-bone local blend (slerp rot, lerp pos/scale) and a two-clip blend at weight w (0→A,
// 1→B) in local TRS space, resolved once — never lerps final skinning matrices (that shears).
Trs blendTrs(const Trs& a, const Trs& b, float w);
Pose blendPose(const SkeletalModel& model, const AnimClip& clipA, float tA,
               const AnimClip& clipB, float tB, float w);

// The per-bone LOCAL matrices of a two-clip blend (blendPose's body minus the resolve). Exposed
// so a post-pass (e.g. foot IK) can edit joint locals before the final resolve.
std::vector<glm::mat4> blendLocalMatrices(const SkeletalModel& model, const AnimClip& clipA,
                                          float tA, const AnimClip& clipB, float tB, float w);

// Resolve per-bone locals to skinning matrices; if outGlobals is non-null it also receives each
// bone's model-space global transform (needed by IK, which works in model space).
Pose resolveLocals(const SkeletalModel& model, const std::vector<glm::mat4>& locals,
                   std::vector<glm::mat4>* outGlobals);

// Sample a clip at an absolute time (seconds since the clip started; wraps by
// duration, so a monotonically growing accumulator loops for free). Bones the
// clip has no track for hold their bind-pose local transform. PURE function:
// no state, no GL — unit-testable off the render thread.
Pose samplePose(const SkeletalModel& model, const AnimClip& clip, float timeSeconds);

// All locals = bind: skinning matrices come out ~identity. The T-pose
// fallback when a model ships without clips.
Pose bindPose(const SkeletalModel& model);

// A looping idle computed by post-multiplying each bone's EXACT bind local by a
// rotation delta (matrix multiply, not TRS decompose) — precise at rest, so it
// works on deep chains (arms/fingers) that the decompose gap-fill path warps.
// Lowers the arms from the T-pose and adds a breathing sway. Used to demonstrate
// animation when a rig ships no usable clip.
Pose idlePose(const SkeletalModel& model, float timeSeconds);

} // namespace meat
