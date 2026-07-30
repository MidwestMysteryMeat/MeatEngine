#pragma once
#include <cstdint>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace meat {

// 0 is never a valid id. Layout: generation in the high 16 bits, slot index in
// the low 48. Stale handles (destroyed then reused slot) fail alive() because
// the generation no longer matches.
using EntityId = std::uint64_t;
inline constexpr EntityId kInvalidEntity = 0;

class EntityRegistry {
public:
    EntityId create() {
        std::uint64_t index;
        if (!m_freeList.empty()) {
            index = m_freeList.back();
            m_freeList.pop_back();
        } else {
            index = m_slots.size();
            m_slots.push_back({});
        }
        m_slots[index].alive = true;
        return makeId(m_slots[index].generation, index);
    }

    void destroy(EntityId e) {
        if (!alive(e)) return;
        const std::uint64_t index = slotIndex(e);
        for (auto& [_, pool] : m_pools) pool->onDestroy(index);
        ++m_slots[index].generation;
        m_slots[index].alive = false;
        m_freeList.push_back(index);
    }

    bool alive(EntityId e) const {
        const std::uint64_t index = slotIndex(e);
        return e != kInvalidEntity && index < m_slots.size() && m_slots[index].alive &&
               m_slots[index].generation == generation(e);
    }

    template <typename T> T& add(EntityId e, T component) {
        auto& p = pool<T>();
        const std::uint64_t index = slotIndex(e);
        if (auto it = p.slotToDense.find(index); it != p.slotToDense.end())
            return p.dense[it->second] = std::move(component);
        p.slotToDense[index] = p.dense.size();
        p.denseToSlot.push_back(index);
        p.dense.push_back(std::move(component));
        return p.dense.back();
    }

    template <typename T> T* get(EntityId e) {
        if (!alive(e)) return nullptr;
        auto& p = pool<T>();
        auto it = p.slotToDense.find(slotIndex(e));
        return it == p.slotToDense.end() ? nullptr : &p.dense[it->second];
    }

    template <typename T> void remove(EntityId e) {
        if (alive(e)) pool<T>().removeSlot(slotIndex(e));
    }

    // Calls fn(EntityId, First&, Rest&...) for every entity holding all listed
    // components. Iterates the first type's dense array; keep the rarest first.
    template <typename First, typename... Rest, typename Fn> void each(Fn&& fn) {
        auto& p = pool<First>();
        for (std::size_t d = 0; d < p.dense.size(); ++d) {
            const std::uint64_t index = p.denseToSlot[d];
            if (!m_slots[index].alive) continue;
            const EntityId id = makeId(m_slots[index].generation, index);
            if constexpr (sizeof...(Rest) == 0) {
                fn(id, p.dense[d]);
            } else {
                auto rest = std::tuple<Rest*...>{get<Rest>(id)...};
                const bool all = (std::get<Rest*>(rest) && ...);
                if (all) fn(id, p.dense[d], *std::get<Rest*>(rest)...);
            }
        }
    }

private:
    static EntityId makeId(std::uint16_t gen, std::uint64_t index) {
        return (static_cast<std::uint64_t>(gen) << 48) | (index + 1);
    }
    static std::uint64_t slotIndex(EntityId e) { return (e & 0xFFFFFFFFFFFFull) - 1; }
    static std::uint16_t generation(EntityId e) { return static_cast<std::uint16_t>(e >> 48); }

    struct Slot {
        std::uint16_t generation = 1;
        bool alive = false;
    };

    struct PoolBase {
        virtual ~PoolBase() = default;
        virtual void onDestroy(std::uint64_t slotIndex) = 0;
    };

    template <typename T> struct Pool final : PoolBase {
        std::vector<T> dense;
        std::vector<std::uint64_t> denseToSlot;
        std::unordered_map<std::uint64_t, std::size_t> slotToDense;

        void removeSlot(std::uint64_t slotIdx) {
            auto it = slotToDense.find(slotIdx);
            if (it == slotToDense.end()) return;
            const std::size_t d = it->second, last = dense.size() - 1;
            if (d != last) { // swap-remove, keep dense packed
                dense[d] = std::move(dense[last]);
                denseToSlot[d] = denseToSlot[last];
                slotToDense[denseToSlot[d]] = d;
            }
            dense.pop_back();
            denseToSlot.pop_back();
            slotToDense.erase(it);
        }
        void onDestroy(std::uint64_t slotIdx) override { removeSlot(slotIdx); }
    };

    template <typename T> Pool<T>& pool() {
        auto& slot = m_pools[std::type_index(typeid(T))];
        if (!slot) slot = std::make_unique<Pool<T>>();
        return static_cast<Pool<T>&>(*slot);
    }

    std::vector<Slot> m_slots;
    std::vector<std::uint64_t> m_freeList;
    std::unordered_map<std::type_index, std::unique_ptr<PoolBase>> m_pools;
};

} // namespace meat
