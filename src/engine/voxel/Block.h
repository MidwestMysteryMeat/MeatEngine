#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace meat {

using BlockId = std::uint16_t; // 0 = air

// faceTex order matches the mesher's face/neighbor order: +X,-X,+Y,-Y,+Z,-Z.
struct BlockDef {
    std::string name;
    std::array<std::uint16_t, 6> faceTex{};
    bool solid = true;
};

// Register block types at startup, before streaming begins: workers read the
// registry concurrently during meshing, so it must be immutable by then.
class BlockRegistry {
public:
    BlockId add(BlockDef def) {
        m_defs.push_back(std::move(def));
        return static_cast<BlockId>(m_defs.size()); // ids are sequential from 1; 0 stays air
    }

    const BlockDef& get(BlockId id) const {
        if (id == 0) return airDef();
        assert(static_cast<std::size_t>(id) <= m_defs.size());
        return m_defs[id - 1];
    }

private:
    static const BlockDef& airDef() {
        static const BlockDef air{"air", {}, false};
        return air;
    }

    std::vector<BlockDef> m_defs;
};

} // namespace meat
