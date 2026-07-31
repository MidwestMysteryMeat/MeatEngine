#include "game/Environment.h"

namespace meat {

EnvSettings envSettings(GameRules::Environment env) {
    using Env = GameRules::Environment;
    switch (env) {
    case Env::Underwater:
        // Buoyant sink, thick close blue fog, cool blue ambient — a submerged world.
        return {-6.0f, glm::vec3(0.06f, 0.20f, 0.32f), 4.0f, 40.0f, glm::vec3(0.10f, 0.22f, 0.30f)};
    case Env::Space:
        // Near-zero gravity (not 0, so the grounded FPS controller still behaves), black void with
        // fog pushed far past view distance (effectively none), dark ambient.
        return {-1.5f, glm::vec3(0.0f, 0.0f, 0.0f), 400.0f, 800.0f, glm::vec3(0.05f, 0.05f, 0.07f)};
    case Env::Surface:
    default:
        // The engine default: snappy -18 gravity, cool grey distance fog, neutral-cool ambient.
        return {-18.0f, glm::vec3(0.10f, 0.11f, 0.13f), 30.0f, 120.0f,
                glm::vec3(0.25f, 0.27f, 0.32f)};
    }
}

} // namespace meat
