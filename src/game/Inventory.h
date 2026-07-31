#pragma once
#include "engine/net/ByteStream.h"
#include "game/Items.h"

#include <array>
#include <unordered_map>

namespace meat {

struct ItemStack {
    ItemId id = 0;
    std::uint16_t count = 0;
};

// 36 slots; the first 9 are the hotbar in HotbarBackpack mode. Server-owned;
// clients hold a mirrored copy for UI only.
class Inventory {
public:
    static constexpr int kSlots = 36;
    static constexpr int kHotbar = 9;

    ItemStack& slot(int i) { return m_slots[static_cast<std::size_t>(i)]; }
    const ItemStack& slot(int i) const { return m_slots[static_cast<std::size_t>(i)]; }

    // Add with stacking; returns leftover count that didn't fit.
    std::uint16_t add(ItemId id, std::uint16_t count, const ItemRegistry& items) {
        const std::uint16_t maxStack = items.get(id).maxStack;
        for (auto& s : m_slots) { // top up existing stacks first
            if (count == 0) break;
            if (s.id == id && s.count < maxStack) {
                const auto space = static_cast<std::uint16_t>(maxStack - s.count);
                const auto moved = count < space ? count : space;
                s.count = static_cast<std::uint16_t>(s.count + moved);
                count = static_cast<std::uint16_t>(count - moved);
            }
        }
        for (auto& s : m_slots) {
            if (count == 0) break;
            if (s.id == 0) {
                const auto moved = count < maxStack ? count : maxStack;
                s = {id, moved};
                count = static_cast<std::uint16_t>(count - moved);
            }
        }
        return count;
    }

    // Remove up to count of an item anywhere in the bag; returns amount removed.
    std::uint16_t remove(ItemId id, std::uint16_t count) {
        std::uint16_t removed = 0;
        for (auto& s : m_slots) {
            if (removed == count) break;
            if (s.id != id) continue;
            const auto take = static_cast<std::uint16_t>(
                (count - removed) < s.count ? (count - removed) : s.count);
            s.count = static_cast<std::uint16_t>(s.count - take);
            removed = static_cast<std::uint16_t>(removed + take);
            if (s.count == 0) s.id = 0;
        }
        return removed;
    }

    std::uint16_t countOf(ItemId id) const {
        std::uint32_t total = 0;
        for (const auto& s : m_slots)
            if (s.id == id) total += s.count;
        return static_cast<std::uint16_t>(total > 0xFFFF ? 0xFFFF : total);
    }

    // --- Magazines (H3) ---------------------------------------------------
    // Rounds currently chambered in a given weapon. Reserve ammo lives in the
    // slots (countOf(ammoItem)); the mag is a separate per-weapon counter so a
    // gun can run dry and force a reload while reserve still remains.
    std::uint16_t magOf(ItemId id) const {
        const auto it = m_mags.find(id);
        return it == m_mags.end() ? std::uint16_t{0} : it->second;
    }
    bool magTracked(ItemId id) const { return m_mags.find(id) != m_mags.end(); }
    void setMag(ItemId id, std::uint16_t rounds) { m_mags[id] = rounds; }
    // Load a full magazine for every weapon stack that takes a mag and isn't
    // tracked yet — the "first time the weapon is held/given" init.
    void initMags(const ItemRegistry& items) {
        for (const auto& s : m_slots) {
            if (s.id == 0) continue;
            const ItemDef& def = items.get(s.id);
            if (def.type == ItemType::Weapon && def.magSize > 0 &&
                m_mags.find(s.id) == m_mags.end())
                m_mags[s.id] = def.magSize;
        }
    }

    void encode(ByteWriter& w) const {
        for (const auto& s : m_slots) {
            w.write(s.id);
            w.write(s.count);
        }
        // Magazines ride the same message so the client HUD can read loaded rounds.
        w.write(static_cast<std::uint16_t>(m_mags.size()));
        for (const auto& [id, rounds] : m_mags) {
            w.write(id);
            w.write(rounds);
        }
    }
    bool decode(ByteReader& r) {
        for (auto& s : m_slots)
            if (!r.read(s.id) || !r.read(s.count)) return false;
        std::uint16_t n = 0;
        if (!r.read(n)) return false;
        if (n > kSlots) return false; // can't track more mags than slots — reject garbage
        m_mags.clear();
        for (std::uint16_t i = 0; i < n; ++i) {
            ItemId id = 0;
            std::uint16_t rounds = 0;
            if (!r.read(id) || !r.read(rounds)) return false;
            m_mags[id] = rounds;
        }
        return true;
    }

private:
    std::array<ItemStack, kSlots> m_slots{};
    std::unordered_map<ItemId, std::uint16_t> m_mags{}; // weapon id -> loaded rounds
};

} // namespace meat
