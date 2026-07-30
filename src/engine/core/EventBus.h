#pragma once
#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace meat {

// Synchronous typed event dispatch. Gameplay mutations (damage, pickups, voxel
// edits) flow through here so each has exactly one application site — the
// network-shape rule from ARCHITECTURE.md. Main thread only.
class EventBus {
public:
    template <typename E> void subscribe(std::function<void(const E&)> handler) {
        handlers<E>().push_back(std::move(handler));
    }

    template <typename E> void emit(const E& event) {
        for (auto& h : handlers<E>()) h(event);
    }

private:
    struct ListBase {
        virtual ~ListBase() = default;
    };
    template <typename E> struct List final : ListBase {
        std::vector<std::function<void(const E&)>> fns;
    };

    template <typename E> std::vector<std::function<void(const E&)>>& handlers() {
        auto& slot = m_lists[std::type_index(typeid(E))];
        if (!slot) slot = std::make_unique<List<E>>();
        return static_cast<List<E>&>(*slot).fns;
    }

    std::unordered_map<std::type_index, std::unique_ptr<ListBase>> m_lists;
};

} // namespace meat
