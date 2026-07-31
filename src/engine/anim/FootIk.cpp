#include "engine/anim/FootIk.h"

#include "ozz/animation/runtime/ik_two_bone_job.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/simd_quaternion.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <string>

namespace meat {
namespace {

// glm (column-major) → ozz Float4x4 (4 SimdFloat4 columns). glm mat columns are m[0..3].
ozz::math::Float4x4 toOzz(const glm::mat4& m) {
    ozz::math::Float4x4 r;
    r.cols[0] = ozz::math::simd_float4::LoadPtrU(glm::value_ptr(m) + 0);
    r.cols[1] = ozz::math::simd_float4::LoadPtrU(glm::value_ptr(m) + 4);
    r.cols[2] = ozz::math::simd_float4::LoadPtrU(glm::value_ptr(m) + 8);
    r.cols[3] = ozz::math::simd_float4::LoadPtrU(glm::value_ptr(m) + 12);
    return r;
}
glm::quat fromOzz(const ozz::math::SimdQuaternion& q) {
    float f[4];
    ozz::math::StorePtrU(q.xyzw, f); // ozz stores (x, y, z, w)
    return glm::quat(f[3], f[0], f[1], f[2]); // glm is (w, x, y, z)
}
ozz::math::SimdFloat4 point(const glm::vec3& v) {
    return ozz::math::simd_float4::Load(v.x, v.y, v.z, 1.0f);
}
ozz::math::SimdFloat4 axis(const glm::vec3& v) {
    return ozz::math::simd_float4::Load(v.x, v.y, v.z, 0.0f);
}

int boneBySuffix(const SkeletalModel& model, const char* suffix) {
    for (int b = 0; b < static_cast<int>(model.bones.size()); ++b) {
        const std::string& n = model.bones[b].name;
        const std::size_t colon = n.find_last_of(':');
        if ((colon == std::string::npos ? n : n.substr(colon + 1)) == suffix) return b;
    }
    return -1;
}
// Bone bind position in the "M" space the IK works in: rootInverse * inverse(offset) (the space
// resolveLocals' globals live in — see applyFootIk). Origin column of that matrix.
glm::vec3 bindPosM(const SkeletalModel& model, int b) {
    return glm::vec3(model.rootInverse * glm::inverse(model.bones[b].offset) * glm::vec4(0, 0, 0, 1));
}

} // namespace

FootIkRig buildFootIkRig(const SkeletalModel& model, const glm::mat4& groundedFit) {
    FootIkRig rig;
    const std::pair<const char*, const char*> chains[2][3] = {
        {{"LeftUpLeg", nullptr}, {"LeftLeg", nullptr}, {"LeftFoot", nullptr}},
        {{"RightUpLeg", nullptr}, {"RightLeg", nullptr}, {"RightFoot", nullptr}}};
    for (int s = 0; s < 2; ++s) {
        FootIkLeg& leg = rig.legs[s];
        leg.hip = boneBySuffix(model, chains[s][0].first);
        leg.knee = boneBySuffix(model, chains[s][1].first);
        leg.ankle = boneBySuffix(model, chains[s][2].first);
        if (leg.hip < 0 || leg.knee < 0 || leg.ankle < 0) {
            leg.hip = leg.knee = leg.ankle = -1; // partial chain is unusable
            continue;
        }
        // Knee hinge axis = normal of the (thigh, shin) plane, expressed in the knee's local frame.
        // Rig-independent; a slightly-bent bind pose (true of Mixamo) makes the cross well-defined.
        const glm::vec3 thigh = bindPosM(model, leg.knee) - bindPosM(model, leg.hip);
        const glm::vec3 shin = bindPosM(model, leg.ankle) - bindPosM(model, leg.knee);
        const glm::vec3 bendM = glm::cross(thigh, shin);
        const glm::mat3 kneeRotInv =
            glm::inverse(glm::mat3(model.rootInverse * glm::inverse(model.bones[leg.knee].offset)));
        const glm::vec3 axisLocal = kneeRotInv * bendM;
        leg.midAxisLocal = glm::length(axisLocal) > 1e-6f ? glm::normalize(axisLocal)
                                                          : glm::vec3(0.0f, 0.0f, 1.0f);
    }
    // Sole offset: ankle height above the grounded sole (bind minY was shifted to 0 by groundedFit).
    const int a = rig.legs[0].ankle >= 0 ? rig.legs[0].ankle : rig.legs[1].ankle;
    if (a >= 0)
        rig.soleOffset =
            (groundedFit * model.rootInverse * glm::inverse(model.bones[a].offset) *
             glm::vec4(0, 0, 0, 1)).y;
    return rig;
}

void applyFootIk(const SkeletalModel& model, const FootIkRig& rig, std::vector<glm::mat4>& locals,
                 const std::vector<glm::mat4>& globals, const glm::mat4& modelToWorld,
                 const std::function<float(float, float)>& groundY, const glm::vec3& forwardWorld,
                 float weight) {
    if (!rig.valid() || weight <= 0.0f || !groundY) return;
    const glm::mat4 worldToModel = glm::inverse(modelToWorld);
    const glm::mat3 worldToModelRot = glm::mat3(worldToModel);
    const glm::vec3 poleModel = glm::length(forwardWorld) > 1e-5f
                                    ? glm::normalize(worldToModelRot * forwardWorld)
                                    : glm::vec3(0, 0, 1);
    for (const FootIkLeg& leg : rig.legs) {
        if (leg.ankle < 0) continue;
        // Joint model matrices (the space resolveLocals' globals live in).
        const glm::mat4 hipM = model.rootInverse * globals[leg.hip];
        const glm::mat4 kneeM = model.rootInverse * globals[leg.knee];
        const glm::mat4 ankleM = model.rootInverse * globals[leg.ankle];
        const glm::vec3 ankleWorld = glm::vec3(modelToWorld * glm::vec4(glm::vec3(ankleM[3]), 1));
        const float terrainY = groundY(ankleWorld.x, ankleWorld.z);
        const float desiredAnkleY = terrainY + rig.soleOffset;
        // Only pull a foot UP onto higher ground; a swinging (lifted) foot keeps its clip height.
        if (ankleWorld.y >= desiredAnkleY - 0.01f) continue; // already at/above target → no-op
        const glm::vec3 targetWorld(ankleWorld.x, desiredAnkleY, ankleWorld.z);
        const glm::vec3 targetModel = glm::vec3(worldToModel * glm::vec4(targetWorld, 1));

        ozz::animation::IKTwoBoneJob job;
        const ozz::math::Float4x4 start = toOzz(hipM), mid = toOzz(kneeM), end = toOzz(ankleM);
        job.start_joint = &start;
        job.mid_joint = &mid;
        job.end_joint = &end;
        job.target = point(targetModel);
        job.pole_vector = axis(poleModel);
        job.mid_axis = axis(leg.midAxisLocal);
        job.soften = 0.97f;
        job.weight = weight;
        ozz::math::SimdQuaternion startCorr, midCorr;
        job.start_joint_correction = &startCorr;
        job.mid_joint_correction = &midCorr;
        if (!job.Validate() || !job.Run()) continue; // any bad input → leave this leg untouched
        // Corrections are local-space, post-multiplied onto the joint local rotation.
        locals[leg.hip] = locals[leg.hip] * glm::mat4_cast(fromOzz(startCorr));
        locals[leg.knee] = locals[leg.knee] * glm::mat4_cast(fromOzz(midCorr));
    }
}

} // namespace meat
