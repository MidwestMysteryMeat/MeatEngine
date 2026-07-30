#include "engine/anim/Animator.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>

namespace meat {
namespace {

// Keys are sorted by time (Assimp guarantees it); clamp outside the range,
// interpolate between neighbors inside. Sub-tick spans guard the division.
glm::vec3 sampleVec(const std::vector<VecKey>& keys, float t) {
    if (keys.size() == 1 || t <= keys.front().time) {
        return keys.front().value;
    }
    if (t >= keys.back().time) {
        return keys.back().value;
    }
    const auto it = std::upper_bound(keys.begin(), keys.end(), t,
                                     [](float v, const VecKey& k) { return v < k.time; });
    const VecKey& k0 = *(it - 1);
    const VecKey& k1 = *it;
    const float span = k1.time - k0.time;
    return glm::mix(k0.value, k1.value, span > 0.0f ? (t - k0.time) / span : 0.0f);
}

glm::quat sampleQuat(const std::vector<QuatKey>& keys, float t) {
    if (keys.size() == 1 || t <= keys.front().time) {
        return keys.front().value;
    }
    if (t >= keys.back().time) {
        return keys.back().value;
    }
    const auto it = std::upper_bound(keys.begin(), keys.end(), t,
                                     [](float v, const QuatKey& k) { return v < k.time; });
    const QuatKey& k0 = *(it - 1);
    const QuatKey& k1 = *it;
    const float span = k1.time - k0.time;
    // glm::slerp negates for the shortest arc; normalize kills float drift.
    return glm::normalize(glm::slerp(k0.value, k1.value, span > 0.0f ? (t - k0.time) / span : 0.0f));
}

struct Trs {
    glm::vec3 pos{0.0f};
    glm::quat rot{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scl{1.0f};
};

// Bind-local decompose for tracks missing a sub-channel (rare, but Assimp does
// not guarantee ≥1 key per channel). Assumes no shear — true of rig exports.
Trs decompose(const glm::mat4& m) {
    Trs out;
    out.pos = glm::vec3(m[3]);
    out.scl = {glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])),
               glm::length(glm::vec3(m[2]))};
    glm::mat3 rot(m);
    if (out.scl.x > 0.0f) rot[0] /= out.scl.x;
    if (out.scl.y > 0.0f) rot[1] /= out.scl.y;
    if (out.scl.z > 0.0f) rot[2] /= out.scl.z;
    out.rot = glm::normalize(glm::quat_cast(rot));
    return out;
}

glm::mat4 compose(const Trs& t) {
    return glm::translate(glm::mat4(1.0f), t.pos) * glm::mat4_cast(t.rot) *
           glm::scale(glm::mat4(1.0f), t.scl);
}

// Forward pass over the topologically ordered bones: parents are always
// resolved before their children, so one loop composes every global.
Pose resolve(const SkeletalModel& model, const std::vector<glm::mat4>& locals) {
    const std::size_t count = model.bones.size();
    std::vector<glm::mat4> globals(count);
    Pose pose;
    pose.skinningMatrices.resize(count);
    for (std::size_t b = 0; b < count; ++b) {
        const Bone& bone = model.bones[b];
        const glm::mat4& parentGlobal =
            bone.parent >= 0 ? globals[static_cast<std::size_t>(bone.parent)] : glm::mat4(1.0f);
        globals[b] = parentGlobal * bone.pre * locals[b];
        // rootInverse (GlobalInverseTransform) cancels the scene-root transform
        // that Assimp bakes into every offset matrix — without it the whole rig
        // inherits the FBX unit scale (~100x) and shatters.
        pose.skinningMatrices[b] = model.rootInverse * globals[b] * bone.offset;
    }
    return pose;
}

} // namespace

Pose samplePose(const SkeletalModel& model, const AnimClip& clip, float timeSeconds) {
    const std::size_t count = model.bones.size();
    assert(count <= static_cast<std::size_t>(kMaxBones)); // loader enforces; belt+braces

    float ticks = timeSeconds * (clip.ticksPerSec > 0.0f ? clip.ticksPerSec : 25.0f);
    if (clip.duration > 0.0f) {
        ticks = std::fmod(ticks, clip.duration);
        if (ticks < 0.0f) {
            ticks += clip.duration; // negative time still lands inside the loop
        }
    }

    std::vector<glm::mat4> locals(count);
    for (std::size_t b = 0; b < count; ++b) {
        locals[b] = model.bones[b].localBind; // untracked bones hold their bind
    }
    for (const BoneTrack& track : clip.tracks) {
        if (track.boneIndex < 0 || static_cast<std::size_t>(track.boneIndex) >= count) {
            continue;
        }
        const std::size_t b = static_cast<std::size_t>(track.boneIndex);
        Trs trs;
        if (track.positions.empty() || track.rotations.empty() || track.scales.empty()) {
            trs = decompose(model.bones[b].localBind); // fill gaps from bind
        }
        if (!track.positions.empty()) {
            trs.pos = sampleVec(track.positions, ticks);
        }
        if (!track.rotations.empty()) {
            trs.rot = sampleQuat(track.rotations, ticks);
        }
        if (!track.scales.empty()) {
            trs.scl = sampleVec(track.scales, ticks);
        }
        locals[b] = compose(trs);
    }
    return resolve(model, locals);
}

Pose bindPose(const SkeletalModel& model) {
    std::vector<glm::mat4> locals(model.bones.size());
    for (std::size_t b = 0; b < locals.size(); ++b) {
        locals[b] = model.bones[b].localBind;
    }
    return resolve(model, locals);
}

Pose idlePose(const SkeletalModel& model, float t) {
    // HONEST STATE: returns the BIND pose (booth-VLM-verified clean: a recognizable
    // standing character). Procedural per-bone rotation attempts sheared the mesh
    // into spikes under a close camera (my earlier "verified" idle was an artifact
    // of a too-distant capture that couldn't see the spikes). Correct animation =
    // play a real Mixamo clip with full-TRS keyframes via samplePose; the
    // per-bone-rotation math is being reworked from reference-engine sources
    // before it's re-enabled. Until then a clean static character beats a broken one.
    (void)t;
    std::vector<glm::mat4> locals(model.bones.size());
    for (std::size_t b = 0; b < locals.size(); ++b) locals[b] = model.bones[b].localBind;
    return resolve(model, locals);
}

} // namespace meat
