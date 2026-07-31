#include "game/Environment.h"

namespace meat {

EnvSettings envSettings(GameRules::Environment env) {
    using Env = GameRules::Environment;
    switch (env) {
    case Env::Underwater: {
        // Buoyant sink, thick close blue fog, cool blue ambient — a submerged world.
        EnvSettings e{};
        e.gravity = -6.0f;
        e.fogColor = glm::vec3(0.06f, 0.20f, 0.32f);
        e.fogStart = 4.0f;
        e.fogEnd = 40.0f;
        e.ambient = glm::vec3(0.10f, 0.22f, 0.30f);
        e.hemiGround = glm::vec3(0.04f, 0.08f, 0.12f);
        e.hemiStrength = 0.65f;
        e.skyZenith = glm::vec3(0.02f, 0.12f, 0.22f);
        e.skyHorizon = glm::vec3(0.04f, 0.18f, 0.28f);
        e.skyGround = glm::vec3(0.01f, 0.05f, 0.10f);
        e.skyStars = false;
        return e;
    }
    case Env::Space: {
        // Near-zero gravity, black void + sparse stars.
        EnvSettings e{};
        e.gravity = -1.5f;
        e.fogColor = glm::vec3(0.0f, 0.0f, 0.0f);
        e.fogStart = 400.0f;
        e.fogEnd = 800.0f;
        e.ambient = glm::vec3(0.05f, 0.05f, 0.07f);
        e.hemiGround = glm::vec3(0.02f, 0.02f, 0.03f);
        e.hemiStrength = 0.0f;
        e.skyZenith = glm::vec3(0.01f, 0.01f, 0.03f);
        e.skyHorizon = glm::vec3(0.02f, 0.02f, 0.05f);
        e.skyGround = glm::vec3(0.0f, 0.0f, 0.0f);
        e.skyStars = true;
        return e;
    }
    case Env::Surface:
    default: {
        EnvSettings e{};
        e.gravity = -18.0f;
        e.fogColor = glm::vec3(0.10f, 0.11f, 0.13f);
        e.fogStart = 30.0f;
        e.fogEnd = 120.0f;
        e.ambient = glm::vec3(0.25f, 0.27f, 0.32f);
        e.hemiGround = glm::vec3(0.10f, 0.09f, 0.07f);
        e.hemiStrength = 0.85f;
        e.skyZenith = glm::vec3(0.28f, 0.45f, 0.75f);
        e.skyHorizon = glm::vec3(0.62f, 0.68f, 0.78f);
        e.skyGround = glm::vec3(0.14f, 0.13f, 0.11f);
        e.skyStars = false;
        return e;
    }
    }
}

} // namespace meat
