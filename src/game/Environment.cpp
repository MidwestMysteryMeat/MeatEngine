#include "game/Environment.h"

namespace meat {

EnvSettings envSettings(GameRules::Environment env) {
    using Env = GameRules::Environment;
    switch (env) {
    case Env::Underwater:
        // Buoyant sink, thick close blue fog, cool blue ambient — a submerged world.
        // Soft blue hemi so underwater form reads without relying on torches alone.
        return {-6.0f,
                glm::vec3(0.06f, 0.20f, 0.32f),
                4.0f,
                40.0f,
                glm::vec3(0.10f, 0.22f, 0.30f),
                glm::vec3(0.04f, 0.08f, 0.12f),
                0.65f};
    case Env::Space:
        // Near-zero gravity, black void, dark ambient. Hemi off by default so space
        // stays ink-black (torch/engine lights define the scene).
        return {-1.5f,
                glm::vec3(0.0f, 0.0f, 0.0f),
                400.0f,
                800.0f,
                glm::vec3(0.05f, 0.05f, 0.07f),
                glm::vec3(0.02f, 0.02f, 0.03f),
                0.0f};
    case Env::Surface:
    default:
        // Snappy gravity, cool grey fog, neutral-cool sky lobe + warmish ground lobe.
        // Hemi on: lifts night readability without flattening AO or torch contrast.
        return {-18.0f,
                glm::vec3(0.10f, 0.11f, 0.13f),
                30.0f,
                120.0f,
                glm::vec3(0.25f, 0.27f, 0.32f),
                glm::vec3(0.10f, 0.09f, 0.07f),
                0.85f};
    }
}

} // namespace meat
