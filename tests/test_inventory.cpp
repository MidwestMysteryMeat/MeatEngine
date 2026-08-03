// Inventory mechanics: stacking, cross-slot removal, counting, and the H3
// magazine model (a per-weapon chambered-round counter separate from reserve
// ammo). These are server-authoritative and drive what every client's HUD
// shows, so the arithmetic has to be exact.

#include "Harness.h"

#include "engine/net/ByteStream.h"
#include "game/Inventory.h"
#include "game/Items.h"

#include <cstdio>

namespace {

using meattest::check;

// A tiny registry: one stackable ammo item and one magazine weapon.
struct Fixture {
    meat::ItemRegistry items;
    meat::ItemId ammo = 0;
    meat::ItemId rifle = 0;
    Fixture() {
        ammo = items.add({.name = "ammo", .type = meat::ItemType::Ammo, .maxStack = 30});
        rifle = items.add(
            {.name = "rifle", .type = meat::ItemType::Weapon, .maxStack = 1, .magSize = 20});
    }
};

void testStackingSpillsToNewSlots() {
    std::printf("adding past a stack's max spills into new slots\n");
    Fixture f;
    meat::Inventory inv;
    // 30-max ammo: 75 rounds must occupy 3 slots (30 + 30 + 15), 0 leftover.
    const std::uint16_t leftover = inv.add(f.ammo, 75, f.items);
    check(leftover == 0, "all 75 rounds fit");
    check(inv.countOf(f.ammo) == 75, "countOf sums across the split stacks");
    int usedSlots = 0;
    for (int i = 0; i < meat::Inventory::kSlots; ++i)
        if (inv.slot(i).id == f.ammo) ++usedSlots;
    check(usedSlots == 3, "75 / 30 fills exactly three slots");
}

void testAddReturnsLeftoverWhenFull() {
    std::printf("a full bag returns the rounds that didn't fit\n");
    Fixture f;
    meat::Inventory inv;
    // Fill every slot with ammo (36 slots * 30 = 1080), then one more must overflow.
    const std::uint16_t cap = meat::Inventory::kSlots * 30;
    check(inv.add(f.ammo, cap, f.items) == 0, "the bag holds exactly its capacity");
    check(inv.add(f.ammo, 5, f.items) == 5, "5 more overflow when the bag is full");
    check(inv.countOf(f.ammo) == cap, "count stays at capacity, not more");
}

void testRemoveAcrossStacks() {
    std::printf("removal drains across stacks and clears emptied slots\n");
    Fixture f;
    meat::Inventory inv;
    inv.add(f.ammo, 45, f.items); // two stacks: 30 + 15
    check(inv.remove(f.ammo, 40) == 40, "removed the requested amount");
    check(inv.countOf(f.ammo) == 5, "5 rounds remain");
    // Removing more than present returns only what was there, leaving nothing.
    check(inv.remove(f.ammo, 99) == 5, "over-removal returns only what existed");
    check(inv.countOf(f.ammo) == 0, "the item is fully gone");
}

void testMagazinesAreSeparateFromReserve() {
    std::printf("magazines are tracked separately from reserve ammo\n");
    Fixture f;
    meat::Inventory inv;
    inv.add(f.rifle, 1, f.items);
    inv.add(f.ammo, 60, f.items);
    inv.initMags(f.items);
    check(inv.magOf(f.rifle) == 20, "initMags loads a full magazine");
    check(inv.countOf(f.ammo) == 60, "the mag does not consume reserve on init");
    // A dry mag must be representable while reserve remains (forces a reload).
    inv.setMag(f.rifle, 0);
    check(inv.magOf(f.rifle) == 0 && inv.countOf(f.ammo) == 60,
          "a weapon can be empty-chambered with reserve still in the bag");
}

void testEncodeDecodeRoundTrip() {
    std::printf("inventory + magazines survive the wire round-trip\n");
    Fixture f;
    meat::Inventory inv;
    inv.add(f.rifle, 1, f.items);
    inv.add(f.ammo, 47, f.items);
    inv.initMags(f.items);
    inv.setMag(f.rifle, 13);

    meat::ByteWriter w;
    inv.encode(w);
    meat::ByteReader r(w.data());
    meat::Inventory out;
    check(out.decode(r), "decode accepts a well-formed buffer");
    check(out.countOf(f.ammo) == 47, "reserve ammo survived");
    check(out.countOf(f.rifle) == 1, "the weapon survived");
    check(out.magOf(f.rifle) == 13, "the chambered round count survived");
}

void testDecodeRejectsGarbage() {
    std::printf("decode rejects truncated / oversized-mag buffers\n");
    // Truncated: fewer bytes than 36 slots need.
    std::vector<std::byte> tiny(4, std::byte{0});
    meat::ByteReader r1(tiny);
    meat::Inventory a;
    check(!a.decode(r1), "a truncated buffer is refused");

    // Valid slots, then a mag count claiming more mags than slots exist.
    meat::ByteWriter w;
    for (int i = 0; i < meat::Inventory::kSlots; ++i) {
        w.write(meat::ItemId{0});
        w.write(std::uint16_t{0});
    }
    w.write(static_cast<std::uint16_t>(meat::Inventory::kSlots + 1)); // impossible mag count
    meat::ByteReader r2(w.data());
    meat::Inventory b;
    check(!b.decode(r2), "an impossible magazine count is refused");
}

} // namespace

namespace meattest {

void runInventory() {
    testStackingSpillsToNewSlots();
    testAddReturnsLeftoverWhenFull();
    testRemoveAcrossStacks();
    testMagazinesAreSeparateFromReserve();
    testEncodeDecodeRoundTrip();
    testDecodeRejectsGarbage();
}

} // namespace meattest
