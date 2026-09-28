#pragma once
#include "Engine/ECS/Entity.h"

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Engine {

template <class T>
concept Component = std::is_object_v<T> && std::movable<T> && !std::is_const_v<T>;

namespace detail {
inline std::size_t NextComponentId() noexcept
{
    static std::size_t counter = 0;
    return counter++;
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
template <Component T>
class ComponentPool final : public IComponentPool {
public:
    template <class... Args>
    T& Emplace(Entity e, Args&&... args)
    {
        assert(!Contains(e) && "Component already present");
        const std::uint32_t index = EntityIndex(e);
        if (index >= m_Sparse.size())
            m_Sparse.resize(std::size_t{index} + 1, kInvalid);
        m_Sparse[index] = static_cast<std::uint32_t>(m_Dense.size());
        m_Dense.push_back(e);
        if constexpr (std::is_aggregate_v<T>)
            m_Components.push_back(T{std::forward<Args>(args)...});
        else
            m_Components.emplace_back(std::forward<Args>(args)...);
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

    [[nodiscard]] std::size_t Size() const override { return m_Dense.size(); }
    [[nodiscard]] Entity      EntityAt(std::size_t i) const override { return m_Dense[i]; }
    [[nodiscard]] std::span<T> Components() { return m_Components; }

private:
    static constexpr std::uint32_t kInvalid = ~std::uint32_t{0};
    std::vector<std::uint32_t> m_Sparse;
    std::vector<Entity>        m_Dense;
    std::vector<T>             m_Components;
};

// Iterates entities that have all of Ts, driven by the smallest pool.
// Iteration runs backwards, so removing components from / destroying the *current* entity is safe.
template <Component... Ts>
class View {
public:
    explicit View(ComponentPool<Ts>*... pools) : m_Pools{pools...} {}

    template <class F>
        requires std::invocable<F&, Entity, Ts&...>
    void Each(F&& fn)
    {
        if (((std::get<ComponentPool<Ts>*>(m_Pools) == nullptr) || ...))
            return;

        const IComponentPool* driver = nullptr;
        ((driver = (!driver || std::get<ComponentPool<Ts>*>(m_Pools)->Size() < driver->Size())
                       ? static_cast<const IComponentPool*>(std::get<ComponentPool<Ts>*>(m_Pools))
                       : driver),
         ...);

        for (std::size_t i = driver->Size(); i-- > 0;) {
            if (i >= driver->Size()) // callback shrank the driver pool by more than one
                continue;
            const Entity e = driver->EntityAt(i);
            if ((std::get<ComponentPool<Ts>*>(m_Pools)->Contains(e) && ...))
                fn(e, std::get<ComponentPool<Ts>*>(m_Pools)->Get(e)...);
        }
    }

private:
    std::tuple<ComponentPool<Ts>*...> m_Pools;
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
        ++m_Generations[EntityIndex(e)];
        m_FreeList.push_back(EntityIndex(e));
        --m_Alive;
    }

    [[nodiscard]] bool Valid(Entity e) const
    {
        const std::uint32_t index = EntityIndex(e);
        return e != NullEntity && index < m_Generations.size() && m_Generations[index] == EntityGeneration(e);
    }

    template <Component T, class... Args>
    T& Emplace(Entity e, Args&&... args)
    {
        assert(Valid(e));
        return Pool<T>().Emplace(e, std::forward<Args>(args)...);
    }

    template <Component T, class... Args>
    T& EmplaceOrReplace(Entity e, Args&&... args)
    {
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

    template <Component... Ts>
    [[nodiscard]] View<Ts...> ViewOf()
    {
        return View<Ts...>(FindPool<Ts>()...);
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
    std::vector<std::uint32_t>                   m_Generations;
    std::vector<std::uint32_t>                   m_FreeList;
    std::size_t                                  m_Alive = 0;
};

} // namespace Engine
