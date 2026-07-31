#include "engine/anim/Animator.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>

namespace meat {

// TRS <-> mat4 (external linkage; declared in Animator.h so the blend layer reaches them).
// Assumes no shear — true of rig exports (matches the sampler's gap-fill assumption).
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

// The per-bone LOCAL matrices for a clip at time t — samplePose's body minus the final
// resolve. Untracked bones hold their bind local. Reused by samplePose and the blend layer.
std::vector<glm::mat4> sampleLocalMatrices(const SkeletalModel& model, const AnimClip& clip,
                                           float timeSeconds) {
    const std::size_t count = model.bones.size();
    assert(count <= static_cast<std::size_t>(kMaxBones));
    float ticks = timeSeconds * (clip.ticksPerSec > 0.0f ? clip.ticksPerSec : 25.0f);
    if (clip.duration > 0.0f) {
        ticks = std::fmod(ticks, clip.duration);
        if (ticks < 0.0f) ticks += clip.duration;
    }
    std::vector<glm::mat4> locals(count);
    for (std::size_t b = 0; b < count; ++b) locals[b] = model.bones[b].localBind;
    for (const BoneTrack& track : clip.tracks) {
        if (track.boneIndex < 0 || static_cast<std::size_t>(track.boneIndex) >= count) continue;
        const std::size_t b = static_cast<std::size_t>(track.boneIndex);
        Trs trs;
        // Gap-fill from nodeBindLocal (the clip-key space) so a missing sub-channel is a zero
        // delta, not a jump into offset space.
        if (track.positions.empty() || track.rotations.empty() || track.scales.empty())
            trs = decompose(model.bones[b].nodeBindLocal);
        if (!track.positions.empty()) trs.pos = sampleVec(track.positions, ticks);
        if (!track.rotations.empty()) trs.rot = sampleQuat(track.rotations, ticks);
        if (!track.scales.empty()) trs.scl = sampleVec(track.scales, ticks);
        // Clip keys live in raw NODE space; localBind lives in the offset-authoritative space.
        // Apply the clip as a DELTA from the node bind onto the clean bind (identity at bind).
        const Bone& bone = model.bones[b];
        locals[b] = bone.localBind * bone.nodeBindLocalInv * compose(trs);
    }
    return locals;
}

// Per-bone RAW clip-key TRS (exactly what the keys store), BEFORE the offset-space
// reconciliation (localBind * nodeBindLocalInv * ·). This is the ONLY clean space to blend in:
// the reconciled local carries the FBX unit-scale factor (in nodeBindLocalInv, ~100x) and
// shear/reflection, which decompose() cannot represent — decomposing it leaks the ~100x into
// translation and flings a bone to Y~100 (the "flying NPC" bug). Blend these raw TRS, then
// apply the reconciliation as matrix math once (see blendPose), matching samplePose exactly.
struct RawLocal {
    Trs trs;        // clip-key-space TRS (or the node-bind gap-fill when untracked)
    bool tracked;   // did this clip actually key this bone?
};
std::vector<RawLocal> sampleClipRawTrs(const SkeletalModel& model, const AnimClip& clip,
                                       float timeSeconds) {
    const std::size_t count = model.bones.size();
    float ticks = timeSeconds * (clip.ticksPerSec > 0.0f ? clip.ticksPerSec : 25.0f);
    if (clip.duration > 0.0f) {
        ticks = std::fmod(ticks, clip.duration);
        if (ticks < 0.0f) ticks += clip.duration;
    }
    std::vector<RawLocal> out(count);
    for (std::size_t b = 0; b < count; ++b) {
        out[b].trs = decompose(model.bones[b].nodeBindLocal); // zero-delta gap-fill
        out[b].tracked = false;
    }
    for (const BoneTrack& track : clip.tracks) {
        if (track.boneIndex < 0 || static_cast<std::size_t>(track.boneIndex) >= count) continue;
        const std::size_t b = static_cast<std::size_t>(track.boneIndex);
        Trs trs = out[b].trs; // start from the node-bind gap-fill (missing sub-channel = no delta)
        if (!track.positions.empty()) trs.pos = sampleVec(track.positions, ticks);
        if (!track.rotations.empty()) trs.rot = sampleQuat(track.rotations, ticks);
        if (!track.scales.empty()) trs.scl = sampleVec(track.scales, ticks);
        out[b].trs = trs;
        out[b].tracked = true;
    }
    return out;
}

// Forward pass over the topologically ordered bones: parents are always
// resolved before their children, so one loop composes every global.
Pose resolve(const SkeletalModel& model, const std::vector<glm::mat4>& locals) {
    return resolveLocals(model, locals, nullptr);
}

} // namespace

// Public resolve: skinning matrices (+ optional model-space globals for IK). rootInverse
// (GlobalInverseTransform) cancels the scene-root transform Assimp bakes into every offset
// matrix — without it the whole rig inherits the FBX unit scale (~100x) and shatters.
Pose resolveLocals(const SkeletalModel& model, const std::vector<glm::mat4>& locals,
                   std::vector<glm::mat4>* outGlobals) {
    const std::size_t count = model.bones.size();
    std::vector<glm::mat4> globals(count);
    Pose pose;
    pose.skinningMatrices.resize(count);
    for (std::size_t b = 0; b < count; ++b) {
        const Bone& bone = model.bones[b];
        const glm::mat4& parentGlobal =
            bone.parent >= 0 ? globals[static_cast<std::size_t>(bone.parent)] : glm::mat4(1.0f);
        globals[b] = parentGlobal * bone.pre * locals[b];
        pose.skinningMatrices[b] = model.rootInverse * globals[b] * bone.offset;
    }
    if (outGlobals) *outGlobals = std::move(globals);
    return pose;
}

Pose samplePose(const SkeletalModel& model, const AnimClip& clip, float timeSeconds) {
    // Identical result to the old inline body; the per-bone local math now lives in
    // sampleLocalMatrices so the blend layer can reuse it.
    return resolve(model, sampleLocalMatrices(model, clip, timeSeconds));
}

std::vector<Trs> sampleLocalTrs(const SkeletalModel& model, const AnimClip& clip, float t) {
    const std::vector<glm::mat4> locals = sampleLocalMatrices(model, clip, t);
    std::vector<Trs> out(locals.size());
    for (std::size_t b = 0; b < locals.size(); ++b) out[b] = decompose(locals[b]);
    return out;
}

Pose resolveLocalTrs(const SkeletalModel& model, const std::vector<Trs>& locals) {
    std::vector<glm::mat4> mats(locals.size());
    for (std::size_t b = 0; b < locals.size(); ++b) mats[b] = compose(locals[b]);
    return resolve(model, mats);
}

// Per-bone local blend: slerp the rotation (shortest arc, normalized), lerp pos/scale.
Trs blendTrs(const Trs& a, const Trs& b, float w) {
    Trs r;
    r.pos = glm::mix(a.pos, b.pos, w);
    r.scl = glm::mix(a.scl, b.scl, w);
    r.rot = glm::normalize(glm::slerp(a.rot, b.rot, w));
    return r;
}

// Blend two clips per bone in RAW clip-key space, apply the offset-space reconciliation as
// matrix math, resolve ONCE. Blending the raw (clean) TRS — NOT the reconciled local, whose
// ~100x unit scale + shear a decompose can't represent — is what keeps a bone from flying to
// Y~100. At w=0 this is bit-identical to samplePose(clipA) (blendTrs returns a; the reconciliation
// line matches sampleLocalMatrices exactly), so the clean single-clip path and the blend agree.
// w clamps 0→A, 1→B; a 1D blend space passes the same phase to both.
std::vector<glm::mat4> blendLocalMatrices(const SkeletalModel& model, const AnimClip& clipA,
                                          float tA, const AnimClip& clipB, float tB, float w) {
    w = glm::clamp(w, 0.0f, 1.0f);
    const std::vector<RawLocal> a = sampleClipRawTrs(model, clipA, tA);
    const std::vector<RawLocal> b = sampleClipRawTrs(model, clipB, tB);
    const std::size_t count = model.bones.size();
    std::vector<glm::mat4> locals(count);
    for (std::size_t i = 0; i < count; ++i) {
        const Bone& bone = model.bones[i];
        // Bones neither clip keys keep their exact bind local — never round-trip it (that's
        // where the shear/scale corruption enters). Otherwise blend the clean raw TRS and apply
        // the SAME reconciliation samplePose uses (localBind * nodeBindLocalInv * compose(·)).
        if (i < a.size() && i < b.size() && (a[i].tracked || b[i].tracked)) {
            const Trs blended = blendTrs(a[i].trs, b[i].trs, w);
            locals[i] = bone.localBind * bone.nodeBindLocalInv * compose(blended);
        } else {
            locals[i] = bone.localBind;
        }
    }
    return locals;
}

Pose blendPose(const SkeletalModel& model, const AnimClip& clipA, float tA,
               const AnimClip& clipB, float tB, float w) {
    return resolve(model, blendLocalMatrices(model, clipA, tA, clipB, tB, w));
}

Pose bindPose(const SkeletalModel& model) {
    std::vector<glm::mat4> locals(model.bones.size());
    for (std::size_t b = 0; b < locals.size(); ++b) {
        locals[b] = model.bones[b].localBind;
    }
    return resolve(model, locals);
}

Pose idlePose(const SkeletalModel& model, float t) {
    // Returns the BIND pose (booth-VLM-verified clean). Real clip playback via
    // samplePose is now shear-free (the localBind * inverse(nodeBindLocal) * key
    // delta reconciles node vs offset space — R720 qwen3vl confirmed a clean posed
    // SWAT operator, no spikes). A procedural sway idle can be layered on the same
    // delta (localBind * smallRotation); kept as static bind until a sway is authored,
    // since a clean static character beats a broken procedural one.
    (void)t;
    std::vector<glm::mat4> locals(model.bones.size());
    for (std::size_t b = 0; b < locals.size(); ++b) locals[b] = model.bones[b].localBind;
    return resolve(model, locals);
}

} // namespace meat
