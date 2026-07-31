#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace meat {

using BlockId = std::uint16_t; // 0 = air

// faceTex order matches the mesher's face/neighbor order: +X,-X,+Y,-Y,+Z,-Z.
// hp/penCost are the ballistics material model: hp = damage a block absorbs
// before breaking (chip destruction), penCost = penetration budget a bullet
// spends passing through one voxel of this material.
struct BlockDef {
    std::string name;
    std::array<std::uint16_t, 6> faceTex{};
    bool solid = true;
    float hp = 50.0f;
    float penCost = 20.0f;
    // Block-light emission at the source, 0..15 (0 = not a light). A torch/lamp
    // block seeds the flood-fill BFS at this level; light falls off -1 per voxel
    // through air and stops at solids. Read on the main/edit thread only.
    std::uint8_t lightEmission = 0;
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

    // For validating remote data before it reaches get()'s assert.
    bool isValid(BlockId id) const { return static_cast<std::size_t>(id) <= m_defs.size(); }

    // Number of registered solid types (ids 1..count). Air is not counted.
    std::size_t count() const { return m_defs.size(); }

private:
    static const BlockDef& airDef() {
        static const BlockDef air{"air", {}, false};
        return air;
    }

    std::vector<BlockDef> m_defs;
};

} // namespace meat
