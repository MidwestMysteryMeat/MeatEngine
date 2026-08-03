#pragma once
#include <cstdint>
#include <vector>

namespace meat {

// Effect-composition core (GAS-lite). An Effect is a *tagged executor + params*:
// a POD with no RTTI/virtuals, dispatched by ServerSim::runEffects switching on
// `kind`. Weapons/abilities/items are built from an EffectList instead of bespoke
// code — the engine doesn't distinguish gun from spell from potion; a delivery +
// an effect list + presentation do (see ARCHITECTURE.md §game/abilities,
// §game/authoring). All executors are server-authoritative and deterministic.
enum class EffectKind : std::uint8_t {
    // Single target (player or NPC): params[0] = damage dealt.
    Damage,
    // Radial: params[0] = center damage, `radius` = falloff radius (m). Reuses
    // ServerSim::applyBlast — the same distance falloff + voxel-crater carving
    // explosives already use, so routing a blast through here is behaviour-equal.
    AreaDamage,
    // Single target (player): params[0] = health restored, clamped to the max.
    Heal,
    // Timed stat buff/debuff on the target player: params[0] = outgoing-damage
    // multiplier, params[1] = movement-speed multiplier, `duration` = seconds it
    // lasts. Stored per-player and ticked down each fixed tick. Both halves are
    // enforced: the damage mult scales server-side damage, the speed mult drives
    // CharacterController::setSpeedScale each tick (so speedMult<1 is a Slow).
    ApplyModifier,
};

// POD effect: kind + a small param block + radius/duration. No allocation, no
// virtuals — copyable into an ItemDef/Projectile and switched on directly.
struct Effect {
    EffectKind kind = EffectKind::Damage;
    float params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float radius = 0.0f;   // AreaDamage blast radius (m)
    float duration = 0.0f; // ApplyModifier lifetime (s)
};

using EffectList = std::vector<Effect>;

// Authoring helpers so item defs read declaratively (spell-maker style) instead
// of raw brace-initialising the params array at every call site.
inline Effect damageEffect(float dmg) {
    return Effect{EffectKind::Damage, {dmg, 0.0f, 0.0f, 0.0f}, 0.0f, 0.0f};
}
inline Effect areaDamageEffect(float centerDamage, float radius) {
    return Effect{EffectKind::AreaDamage, {centerDamage, 0.0f, 0.0f, 0.0f}, radius, 0.0f};
}
inline Effect healEffect(float amount) {
    return Effect{EffectKind::Heal, {amount, 0.0f, 0.0f, 0.0f}, 0.0f, 0.0f};
}
inline Effect modifierEffect(float damageMult, float speedMult, float duration) {
    return Effect{EffectKind::ApplyModifier, {damageMult, speedMult, 0.0f, 0.0f}, 0.0f, duration};
}

} // namespace meat
