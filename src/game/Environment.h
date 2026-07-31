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
    glm::vec3 ambient;   // premultiplied ambient light (colour * intensity)
};

// Maps an Environment preset to its settings. Single source of truth for the presets so server
// (gravity) and client (gravity + fog + ambient) read identical numbers.
EnvSettings envSettings(GameRules::Environment env);

} // namespace meat
