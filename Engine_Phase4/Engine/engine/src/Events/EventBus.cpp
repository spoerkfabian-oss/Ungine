#include "Engine/Events/EventBus.h"

#include <algorithm>

namespace Engine {

void EventBus::Dispatch(std::type_index type, const void* event)
{
    const auto it = m_Handlers.find(type);
    if (it == m_Handlers.end())
        return;

    // Map and vectors are never mutated while depth > 0 (adds/removals are deferred),
    // so `handlers` and the running std::function stay valid even under re-entrancy.
    struct DepthGuard {
        EventBus& bus;
        ~DepthGuard()
        {
            if (--bus.m_DispatchDepth == 0)
                bus.ApplyPendingChanges();
        }
    };
    ++m_DispatchDepth;
    const DepthGuard guard{*this};

    auto& handlers = it->second;
    for (std::size_t i = 0; i < handlers.size(); ++i) {
        if (handlers[i].alive && handlers[i].fn(event))
            break;
    }
}

void EventBus::Unsubscribe(std::type_index type, std::uint64_t id) noexcept
{
    const auto pending = std::ranges::find_if(m_PendingAdds, [id](const auto& p) { return p.second.id == id; });
    if (pending != m_PendingAdds.end()) {
        m_PendingAdds.erase(pending);
        return;
    }

    if (const auto it = m_Handlers.find(type); it != m_Handlers.end()) {
        for (auto& h : it->second) {
            if (h.id == id) {
                h.alive = false; // never destroy a std::function that might be executing
                break;
            }
        }
        m_NeedsCompaction = true;
        if (m_DispatchDepth == 0)
            ApplyPendingChanges();
    }
}

void EventBus::ApplyPendingChanges()
{
    if (m_NeedsCompaction) {
        for (auto& [type, list] : m_Handlers)
            std::erase_if(list, [](const Handler& h) { return !h.alive; });
        m_NeedsCompaction = false;
    }
    for (auto& [type, handler] : m_PendingAdds)
        m_Handlers[type].push_back(std::move(handler));
    m_PendingAdds.clear();
}

void EventBus::Flush()
{
    std::vector<std::function<void()>> queue;
    {
        std::scoped_lock lock{m_QueueMutex};
        queue.swap(m_Queue);
    }
    for (auto& fn : queue) // events enqueued during Flush land in the next frame
        fn();
}

} // namespace Engine
