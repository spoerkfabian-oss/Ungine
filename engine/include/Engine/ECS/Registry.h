#pragma once
#include "Engine/ECS/Entity.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Engine {

// Components must move without throwing: pools swap-and-pop them, which could not be undone.
template <class T>
concept Component = std::is_object_v<T> && std::movable<T> && !std::is_const_v<T> &&
                    std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>;

// Registries are not thread-safe (use one from one thread at a time); component ids may be
// assigned from any thread.
namespace detail {
inline std::size_t NextComponentId() noexcept
{
    static std::atomic<std::size_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}
template <class T>
std::size_t ComponentId() noexcept
{
    static const std::size_t id = NextComponentId();
    return id;
}
} // namespace detail

class IComponentPool {
public:
    virtual ~IComponentPool() = default;
    virtual void                     Remove(Entity e)         = 0;
    [[nodiscard]] virtual bool       Contains(Entity e) const = 0;
    [[nodiscard]] virtual std::size_t Size() const            = 0;
    [[nodiscard]] virtual Entity     EntityAt(std::size_t i) const = 0;
};

// Sparse set: O(1) add/remove/lookup, components tightly packed for cache-friendly iteration.
// References returned by Get/Emplace are invalidated by any later Emplace/Remove on the same pool.
// Get requires Contains (checked by assert only, like std::vector::operator[]).
template <Component T>
class ComponentPool final : public IComponentPool {
public:
    // Strong guarantee: if constructing T (or allocating) throws, the pool is unchanged.
    // Already present: replaced (asserts in debug builds).
    template <class... Args>
    T& Emplace(Entity e, Args&&... args)
    {
        if (T* existing = TryGet(e)) {
            assert(false && "Component already present");
            *existing = Make(std::forward<Args>(args)...);
            return *existing;
        }
        const std::uint32_t index = EntityIndex(e);
        if (index >= m_Sparse.size())
            m_Sparse.resize(std::size_t{index} + 1, kInvalid); // extra free entries are harmless
        if constexpr (std::is_aggregate_v<T>)
            m_Components.push_back(T{std::forward<Args>(args)...});
        else
            m_Components.emplace_back(std::forward<Args>(args)...);
        try {
            m_Dense.push_back(e);
        } catch (...) {
            m_Components.pop_back();
            throw;
        }
        m_Sparse[index] = static_cast<std::uint32_t>(m_Dense.size() - 1);
        return m_Components.back();
    }

    void Remove(Entity e) override
    {
        if (!Contains(e))
            return;
        const std::uint32_t index = EntityIndex(e);
        const std::uint32_t pos   = m_Sparse[index];
        const std::uint32_t last  = static_cast<std::uint32_t>(m_Dense.size() - 1);
        if (pos != last) { // swap-and-pop keeps the arrays dense
            m_Dense[pos]      = m_Dense[last];
            m_Components[pos] = std::move(m_Components[last]);
            m_Sparse[EntityIndex(m_Dense[pos])] = pos;
        }
        m_Dense.pop_back();
        m_Components.pop_back();
        m_Sparse[index] = kInvalid;
    }

    [[nodiscard]] bool Contains(Entity e) const override
    {
        const std::uint32_t index = EntityIndex(e);
        // Comparing the full handle also rejects stale generations.
        return index < m_Sparse.size() && m_Sparse[index] != kInvalid && m_Dense[m_Sparse[index]] == e;
    }

    [[nodiscard]] T& Get(Entity e)
    {
        assert(Contains(e));
        return m_Components[m_Sparse[EntityIndex(e)]];
    }
    [[nodiscard]] T* TryGet(Entity e) { return Contains(e) ? &m_Components[m_Sparse[EntityIndex(e)]] : nullptr; }
    [[nodiscard]] const T& Get(Entity e) const
    {
        assert(Contains(e));
        return m_Components[m_Sparse[EntityIndex(e)]];
    }
    [[nodiscard]] const T* TryGet(Entity e) const
    {
        return Contains(e) ? &m_Components[m_Sparse[EntityIndex(e)]] : nullptr;
    }

    [[nodiscard]] std::size_t Size() const override { return m_Dense.size(); }
    [[nodiscard]] Entity      EntityAt(std::size_t i) const override { return m_Dense[i]; }
    [[nodiscard]] std::span<T> Components() { return m_Components; }

private:
    template <class... Args>
    static T Make(Args&&... args)
    {
        if constexpr (std::is_aggregate_v<T>)
            return T{std::forward<Args>(args)...};
        else
            return T(std::forward<Args>(args)...);
    }

    static constexpr std::uint32_t kInvalid = ~std::uint32_t{0};
    std::vector<std::uint32_t> m_Sparse;
    std::vector<Entity>        m_Dense;
    std::vector<T>             m_Components;
};

// Iterates the entities that have all of Ts when Each starts (driven by the smallest pool). The
// callback may add or remove components and create or destroy entities (any of them, e.g. whole
// subtrees): entities that lose a component or die before their turn are skipped, new ones are
// not visited, none is visited twice.
template <Component... Ts>
class View {
public:
    explicit View(ComponentPool<Ts>*... pools) : m_Pools{pools...} {}

    template <class F>
        requires std::invocable<F&, Entity, Ts&...>
    void Each(F&& fn)
    {
        if ((!std::get<ComponentPool<Ts>*>(m_Pools) || ...)) // (x == nullptr) trips clang's -Wparentheses-equality
            return;

        const IComponentPool* driver = nullptr;
        ((driver = (!driver || std::get<ComponentPool<Ts>*>(m_Pools)->Size() < driver->Size())
                       ? static_cast<const IComponentPool*>(std::get<ComponentPool<Ts>*>(m_Pools))
                       : driver),
         ...);

        // Snapshot: swap-and-pop removals reorder the pool while the callback runs.
        std::vector<Entity> entities(driver->Size());
        for (std::size_t i = 0; i < entities.size(); ++i)
            entities[i] = driver->EntityAt(i);
        for (const Entity e : entities)
            if ((std::get<ComponentPool<Ts>*>(m_Pools)->Contains(e) && ...)) // also rejects dead handles
                fn(e, std::get<ComponentPool<Ts>*>(m_Pools)->Get(e)...);
    }

private:
    std::tuple<ComponentPool<Ts>*...> m_Pools;
};

// Read-only View (Registry::ViewOf() const): the callback gets const components and may not
// change the registry; otherwise like View.
template <Component... Ts>
class ConstView {
public:
    explicit ConstView(const ComponentPool<Ts>*... pools) : m_Pools{pools...} {}

    template <class F>
        requires std::invocable<F&, Entity, const Ts&...>
    void Each(F&& fn) const
    {
        if ((!std::get<const ComponentPool<Ts>*>(m_Pools) || ...))
            return;

        const IComponentPool* driver = nullptr;
        ((driver = (!driver || std::get<const ComponentPool<Ts>*>(m_Pools)->Size() < driver->Size())
                       ? static_cast<const IComponentPool*>(std::get<const ComponentPool<Ts>*>(m_Pools))
                       : driver),
         ...);
        for (std::size_t i = 0; i < driver->Size(); ++i) {
            const Entity e = driver->EntityAt(i);
            if ((std::get<const ComponentPool<Ts>*>(m_Pools)->Contains(e) && ...))
                fn(e, std::get<const ComponentPool<Ts>*>(m_Pools)->Get(e)...);
        }
    }

private:
    std::tuple<const ComponentPool<Ts>*...> m_Pools;
};

class Registry {
public:
    Registry()                           = default;
    Registry(const Registry&)            = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&)                 = default;
    Registry& operator=(Registry&&)      = default;

    [[nodiscard]] Entity Create()
    {
        std::uint32_t index = 0;
        if (!m_FreeList.empty()) {
            index = m_FreeList.back();
            m_FreeList.pop_back();
        } else {
            index = static_cast<std::uint32_t>(m_Generations.size());
            m_Generations.push_back(0);
        }
        ++m_Alive;
        return MakeEntity(index, m_Generations[index]);
    }

    void Destroy(Entity e)
    {
        if (!Valid(e))
            return;
        for (auto& pool : m_Pools)
            if (pool)
                pool->Remove(e);
        // A slot whose generation is used up is retired instead of reused: old handles never alias.
        if (++m_Generations[EntityIndex(e)] != kRetiredGeneration)
            m_FreeList.push_back(EntityIndex(e));
        --m_Alive;
    }

    [[nodiscard]] bool Valid(Entity e) const
    {
        const std::uint32_t index = EntityIndex(e);
        return e != NullEntity && index < m_Generations.size() && m_Generations[index] == EntityGeneration(e) &&
               EntityGeneration(e) != kRetiredGeneration;
    }

    // Handle with the slot's current generation (e.g. from a GPU entity-index buffer). A free slot
    // also yields a "valid" handle: check for a component the entity must have.
    [[nodiscard]] Entity EntityAtIndex(std::uint32_t index) const
    {
        return index < m_Generations.size() ? MakeEntity(index, m_Generations[index]) : NullEntity;
    }

    // Throws std::invalid_argument for a dead entity (a component on a dead slot would corrupt the pool).
    template <Component T, class... Args>
    T& Emplace(Entity e, Args&&... args)
    {
        if (!Valid(e))
            throw std::invalid_argument("Registry::Emplace: invalid entity");
        return Pool<T>().Emplace(e, std::forward<Args>(args)...);
    }

    template <Component T, class... Args>
    T& EmplaceOrReplace(Entity e, Args&&... args)
    {
        if (!Valid(e))
            throw std::invalid_argument("Registry::EmplaceOrReplace: invalid entity");
        auto& pool = Pool<T>();
        if (T* existing = pool.TryGet(e)) {
            *existing = T{std::forward<Args>(args)...};
            return *existing;
        }
        return pool.Emplace(e, std::forward<Args>(args)...);
    }

    template <Component T>
    void Remove(Entity e)
    {
        if (auto* pool = FindPool<T>())
            pool->Remove(e);
    }

    template <Component T>
    [[nodiscard]] bool Has(Entity e) const
    {
        const auto* pool = FindPool<T>();
        return pool && pool->Contains(e);
    }

    template <Component T>
    [[nodiscard]] T& Get(Entity e)
    {
        auto* pool = FindPool<T>();
        assert(pool && "Component type never added");
        return pool->Get(e);
    }

    template <Component T>
    [[nodiscard]] T* TryGet(Entity e)
    {
        auto* pool = FindPool<T>();
        return pool ? pool->TryGet(e) : nullptr;
    }

    template <Component T>
    [[nodiscard]] const T& Get(Entity e) const
    {
        const auto* pool = FindPool<T>();
        assert(pool && "Component type never added");
        return std::as_const(*pool).Get(e);
    }

    template <Component T>
    [[nodiscard]] const T* TryGet(Entity e) const
    {
        const auto* pool = FindPool<T>();
        return pool ? std::as_const(*pool).TryGet(e) : nullptr;
    }

    template <Component... Ts>
    [[nodiscard]] View<Ts...> ViewOf()
    {
        return View<Ts...>(FindPool<Ts>()...);
    }
    template <Component... Ts>
    [[nodiscard]] ConstView<Ts...> ViewOf() const
    {
        return ConstView<Ts...>(FindPool<Ts>()...);
    }

    [[nodiscard]] std::size_t AliveCount() const { return m_Alive; }

private:
    template <Component T>
    ComponentPool<T>& Pool()
    {
        const std::size_t id = detail::ComponentId<T>();
        if (id >= m_Pools.size())
            m_Pools.resize(id + 1);
        if (!m_Pools[id])
            m_Pools[id] = std::make_unique<ComponentPool<T>>();
        return static_cast<ComponentPool<T>&>(*m_Pools[id]);
    }

    template <Component T>
    ComponentPool<T>* FindPool() const
    {
        const std::size_t id = detail::ComponentId<T>();
        return id < m_Pools.size() ? static_cast<ComponentPool<T>*>(m_Pools[id].get()) : nullptr;
    }

    std::vector<std::unique_ptr<IComponentPool>> m_Pools; // indexed by component id
    static constexpr std::uint32_t kRetiredGeneration = ~std::uint32_t{0};

    std::vector<std::uint32_t>                   m_Generations;
    std::vector<std::uint32_t>                   m_FreeList;
    std::size_t                                  m_Alive = 0;
};

} // namespace Engine
