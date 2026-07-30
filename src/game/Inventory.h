#pragma once
#include "engine/net/ByteStream.h"
#include "game/Items.h"

#include <array>

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

    void encode(ByteWriter& w) const {
        for (const auto& s : m_slots) {
            w.write(s.id);
            w.write(s.count);
        }
    }
    bool decode(ByteReader& r) {
        for (auto& s : m_slots)
            if (!r.read(s.id) || !r.read(s.count)) return false;
        return true;
    }

private:
    std::array<ItemStack, kSlots> m_slots{};
};

} // namespace meat
