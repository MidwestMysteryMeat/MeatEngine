#pragma once
#include "engine/asset/SkeletalModel.h"

#include <glm/glm.hpp>

#include <functional>
#include <vector>

namespace meat {

// A two-bone leg chain (hip → knee → ankle) for foot IK, plus the data the solver needs.
struct FootIkLeg {
    int hip = -1, knee = -1, ankle = -1;
    glm::vec3 midAxisLocal{0.0f, 0.0f, 1.0f}; // knee hinge axis in the knee's local space
};

// The rig's legs + the ankle-to-sole vertical offset (render units), derived once from the bind
// pose so the solver can plant soles (not ankle joints) on the ground.
struct FootIkRig {
    FootIkLeg legs[2];
    float soleOffset = 0.0f;
    bool valid() const { return legs[0].ankle >= 0 || legs[1].ankle >= 0; }
};

// Resolve the leg chains + knee hinge axes + sole offset from a rigged model. `groundedFit` is the
// model→render matrix used for the pose (m_npcActor->transform), needed to measure soleOffset.
FootIkRig buildFootIkRig(const SkeletalModel& model, const glm::mat4& groundedFit);

// Two-bone foot IK: plant each foot on the terrain by rotating hip+knee so the sole reaches the
// ground under it (pushes a foot up onto steps; never forces a swinging foot down below its clip).
// Edits `locals` in place (hip/knee rotations) — caller re-resolves. `globals` are the current
// pose's model-space transforms (from resolveLocals), `modelToWorld` maps that model space to
// world, groundY(x,z) is the terrain surface height in world space, forwardWorld is the character
// facing (knee pole), weight 0..1 dials the effect. No-op when the rig is invalid or weight<=0.
void applyFootIk(const SkeletalModel& model, const FootIkRig& rig, std::vector<glm::mat4>& locals,
                 const std::vector<glm::mat4>& globals, const glm::mat4& modelToWorld,
                 const std::function<float(float, float)>& groundY, const glm::vec3& forwardWorld,
                 float weight);

} // namespace meat
