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
    // Shove the target player away from the effect origin: params[0] = impulse
    // speed (m/s). Applied via CharacterController::addImpulse (horizontal push +
    // a small upward pop); decays over a fraction of a second. NPCs are unaffected
    // (they don't use the character controller).
    Knockback,
    // Damage-over-time on the target player: params[0] = damage/sec, `duration` =
    // seconds it burns. Stacks; ticked server-side each fixed tick until it expires,
    // and a burn that kills credits the igniter. NPCs aren't ignitable yet.
    Ignite,
    // Arc/chain: params[0] = damage per target, params[1] = max targets, `radius` =
    // jump range (m). Damages the player nearest the origin, then arcs to the
    // nearest unhit player within range of the last, up to max targets. Kills
    // credit the source. Players only for now (NPC arc targets are a follow-up).
    Chain,
    // Summon an owned AI helper at the effect's target point: params[0] selects the
    // archetype (0 = auto-turret, >=1 = mobile companion). Owned by the acting
    // player; replicates through the normal entity-snapshot path.
    SpawnEntity,
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
inline Effect knockbackEffect(float speed) {
    return Effect{EffectKind::Knockback, {speed, 0.0f, 0.0f, 0.0f}, 0.0f, 0.0f};
}
inline Effect igniteEffect(float dps, float seconds) {
    return Effect{EffectKind::Ignite, {dps, 0.0f, 0.0f, 0.0f}, 0.0f, seconds};
}
inline Effect chainEffect(float damage, int maxTargets, float range) {
    return Effect{EffectKind::Chain,
                  {damage, static_cast<float>(maxTargets), 0.0f, 0.0f}, range, 0.0f};
}
// archetype: 0 = turret, 1 = companion.
inline Effect spawnEntityEffect(int archetype) {
    return Effect{EffectKind::SpawnEntity,
                  {static_cast<float>(archetype), 0.0f, 0.0f, 0.0f}, 0.0f, 0.0f};
}

} // namespace meat
