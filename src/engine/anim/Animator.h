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

// Sample a clip at an absolute time (seconds since the clip started; wraps by
// duration, so a monotonically growing accumulator loops for free). Bones the
// clip has no track for hold their bind-pose local transform. PURE function:
// no state, no GL — unit-testable off the render thread.
Pose samplePose(const SkeletalModel& model, const AnimClip& clip, float timeSeconds);

// All locals = bind: skinning matrices come out ~identity. The T-pose
// fallback when a model ships without clips.
Pose bindPose(const SkeletalModel& model);

} // namespace meat
