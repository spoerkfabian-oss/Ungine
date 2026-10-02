#pragma once
#include "Engine/Core/MoveOnlyFunction.h"

#include <concepts>
#include <cstdint>
#include <functional>
#include <mutex>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Engine {

template <class T>
concept EventType = std::is_class_v<T> && std::is_copy_constructible_v<T> && !std::is_polymorphic_v<T>;

class EventBus;

// RAII handle: unsubscribes on destruction. Must not outlive its EventBus.
class [[nodiscard]] Subscription {
public:
    Subscription() = default;
    ~Subscription() { Reset(); }

    Subscription(Subscription&& o) noexcept
        : m_Bus{std::exchange(o.m_Bus, nullptr)}, m_Type{o.m_Type}, m_Id{o.m_Id} {}
    Subscription& operator=(Subscription&& o) noexcept
    {
        if (this != &o) {
            Reset();
            m_Bus  = std::exchange(o.m_Bus, nullptr);
            m_Type = o.m_Type;
            m_Id   = o.m_Id;
        }
        return *this;
    }
    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;

    void Reset() noexcept;
    [[nodiscard]] bool Active() const noexcept { return m_Bus != nullptr; }

private:
    friend class EventBus;
    Subscription(EventBus* bus, std::type_index type, std::uint64_t id) noexcept
        : m_Bus{bus}, m_Type{type}, m_Id{id} {}

    EventBus*       m_Bus = nullptr;
    std::type_index m_Type{typeid(void)};
    std::uint64_t   m_Id = 0;
};

// Publish/Subscribe: main thread only. Enqueue: thread-safe, delivered on Flush().
// Handlers only need to be movable.
// Handlers returning `true` consume the event (stop propagation).
// Subscribing/unsubscribing inside a handler is safe (applied after dispatch).
class EventBus {
public:
    EventBus()                           = default;
    EventBus(const EventBus&)            = delete;
    EventBus& operator=(const EventBus&) = delete;

    template <EventType E, class F>
        requires std::invocable<F&, const E&>
    Subscription Subscribe(F&& handler)
    {
        const std::uint64_t id = ++m_NextId;
        Handler h{id, true, [fn = std::forward<F>(handler)](const void* e) mutable -> bool {
                      const E& ev = *static_cast<const E*>(e);
                      if constexpr (std::same_as<std::invoke_result_t<F&, const E&>, bool>) {
                          return fn(ev);
                      } else {
                          fn(ev);
                          return false;
                      }
                  }};
        const std::type_index type{typeid(E)};
        if (m_DispatchDepth > 0)
            m_PendingAdds.emplace_back(type, std::move(h));
        else
            m_Handlers[type].push_back(std::move(h));
        return Subscription{this, type, id};
    }

    template <EventType E>
    void Publish(const E& event) { Dispatch(typeid(E), &event); }

    template <EventType E>
    void Enqueue(E event)
    {
        std::scoped_lock lock{m_QueueMutex};
        m_Queue.emplace_back([this, ev = std::move(event)] { Publish(ev); });
    }

    void Flush();

private:
    friend class Subscription;

    struct Handler {
        std::uint64_t                     id;
        bool                              alive;
        MoveOnlyFunction<bool(const void*)> fn;
    };

    void Dispatch(std::type_index type, const void* event);
    void Unsubscribe(std::type_index type, std::uint64_t id) noexcept;
    void ApplyPendingChanges();

    std::unordered_map<std::type_index, std::vector<Handler>> m_Handlers;
    std::vector<std::pair<std::type_index, Handler>>          m_PendingAdds;
    std::uint64_t m_NextId          = 0;
    int           m_DispatchDepth   = 0;
    bool          m_NeedsCompaction = false;

    std::mutex                         m_QueueMutex;
    std::vector<MoveOnlyFunction<void()>> m_Queue;
};

inline void Subscription::Reset() noexcept
{
    if (m_Bus) {
        m_Bus->Unsubscribe(m_Type, m_Id);
        m_Bus = nullptr;
    }
}

} // namespace Engine
