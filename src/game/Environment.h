#pragma once
#include "game/GameRules.h"

#include <glm/vec3.hpp>

namespace meat {

// The concrete world-feel knobs an Environment preset drives. Kept out of GameRules.h so that
// header stays glm-free (it is included widely, including by save/serialization code). Gravity is
// applied to the physics world + character controllers; fog/ambient feed the renderer's PsxOptions
// and ambient light.
struct EnvSettings {
    float gravity;       // m/s² (negative = down); character fall + Jolt system gravity
    glm::vec3 fogColor;  // PSX vertex-fog colour (also the void/clear tint's mood)
    float fogStart;      // metres: fog begins
    float fogEnd;        // metres: fog fully saturates
    glm::vec3 ambient;   // premultiplied ambient light (colour * intensity) — sky lobe
    // Hemisphere ambient (A3): sky (ambient) vs ground colours blended by normal.y.
    // strength 0 = classic flat ambient only (dark PSX-night preserved); 1 = full hemi.
    // Applied OUTSIDE the voxel block-light gate so form stays readable without torches.
    glm::vec3 hemiGround{0.08f, 0.07f, 0.06f};
    float hemiStrength = 0.0f;
    // B3-sky procedural gradient (zenith / horizon / ground).
    glm::vec3 skyZenith{0.28f, 0.42f, 0.68f};
    glm::vec3 skyHorizon{0.55f, 0.62f, 0.72f};
    glm::vec3 skyGround{0.12f, 0.11f, 0.10f};
    bool skyStars = false;
};

// Maps an Environment preset to its settings. Single source of truth for the presets so server
// (gravity) and client (gravity + fog + ambient) read identical numbers.
EnvSettings envSettings(GameRules::Environment env);

} // namespace meat
