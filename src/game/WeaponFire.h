#pragma once
#include "game/Inventory.h"
#include "game/Items.h"

#include <algorithm>

// Pure, side-effect-light weapon-feel helpers shared by the authoritative combat
// step (ServerSim::processCombat) and the headless gun-feel test. Keeping the
// decision logic here means the test exercises the SAME code the server runs.
namespace meat {

// Decide whether the trigger releases a shot THIS tick.
//   mode          - the weapon's fire mode
//   fireHeld      - fire button held this tick
//   firePressed   - press EDGE this tick (held && !heldLastTick)
//   ready         - fireCooldown has expired (rate cap satisfied)
//   burstCount    - rounds in a burst (Burst mode)
//   burstRemaining- mutable: rounds left in the current burst
// SemiAuto fires exactly once per press edge (holding never auto-repeats). Auto
// fires whenever held and ready. Burst starts an N-round burst on a fresh press
// edge and pays them out on each ready tick until spent, then needs a release.
inline bool triggerReleasesShot(FireMode mode, bool fireHeld, bool firePressed, bool ready,
                                int burstCount, int& burstRemaining) {
    switch (mode) {
    case FireMode::Auto:
        return ready && fireHeld;
    case FireMode::SemiAuto:
        return ready && firePressed;
    case FireMode::Burst:
        // A new burst may only begin on a press edge with none in flight; this is
        // what forces a release between bursts (a held button can't restart one).
        if (firePressed && burstRemaining == 0) burstRemaining = burstCount > 0 ? burstCount : 1;
        if (ready && burstRemaining > 0) {
            --burstRemaining;
            return true;
        }
        return false;
    }
    return false;
}

// Refill the magazine of `weaponId` from reserve ammo, up to magSize.
//   finiteAmmo ON  -> pull rounds out of the reserve stack (def.ammoItem); a
//                     partial reserve tops the mag up as far as it goes.
//   finiteAmmo OFF -> free arcade refill to full, reserve untouched.
// No-op (returns 0) for a magless weapon, a full mag, or an empty reserve.
inline int reloadWeaponMag(Inventory& inv, ItemId weaponId, const ItemDef& def, bool finiteAmmo) {
    if (def.magSize == 0) return 0;
    const int cur = inv.magOf(weaponId);
    const int need = static_cast<int>(def.magSize) - cur;
    if (need <= 0) return 0;
    int loaded = need;
    if (finiteAmmo && def.ammoItem != 0) {
        const int reserve = inv.countOf(def.ammoItem);
        loaded = std::min(need, reserve);
        if (loaded <= 0) return 0;
        inv.remove(def.ammoItem, static_cast<std::uint16_t>(loaded));
    }
    inv.setMag(weaponId, static_cast<std::uint16_t>(cur + loaded));
    return loaded;
}

} // namespace meat
