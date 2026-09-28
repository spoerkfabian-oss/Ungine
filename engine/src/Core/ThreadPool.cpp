#include "Engine/Core/ThreadPool.h"
#include "Engine/Core/Log.h"

#include <algorithm>
#include <exception>
#include <format>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Engine {

namespace {
void SetCurrentThreadName(std::uint32_t index)
{
#ifdef _WIN32
    const std::wstring name = std::format(L"Worker {}", index);
    SetThreadDescription(GetCurrentThread(), name.c_str());
#else
    (void)index; // pthread_setname_np is not portable enough to bother
#endif
}
} // namespace

ThreadPool::ThreadPool(std::uint32_t threadCount)
{
    const std::uint32_t count = threadCount > 0 ? threadCount : DefaultThreadCount();
    m_Workers.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        m_Workers.emplace_back([this, i](std::stop_token stop) {
            SetCurrentThreadName(i);
            WorkerLoop(stop);
        });
    }
}

ThreadPool::~ThreadPool()
{
    // Stop everyone first so the workers drain the queue in parallel; jthread joins on destruction.
    for (std::jthread& w : m_Workers)
        w.request_stop();
    m_Workers.clear();
}

void ThreadPool::Enqueue(std::function<void()> job)
{
    {
        std::scoped_lock lock{m_Mutex};
        m_Jobs.push_back(std::move(job));
    }
    m_Wake.notify_one();
}

std::uint32_t ThreadPool::DefaultThreadCount()
{
    const unsigned hw = std::thread::hardware_concurrency(); // 0 if unknown
    return std::max(1u, hw > 1 ? hw - 1 : 1u);
}

void ThreadPool::WorkerLoop(std::stop_token stop)
{
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock{m_Mutex};
            // Returns on new work or stop request; queued work is still drained after a stop.
            m_Wake.wait(lock, stop, [this] { return !m_Jobs.empty(); });
            if (m_Jobs.empty())
                return;
            job = std::move(m_Jobs.front());
            m_Jobs.pop_front();
        }
        try {
            job();
        } catch (const std::exception& e) {
            ENGINE_ERROR("ThreadPool: job threw: {}", e.what());
        } catch (...) {
            ENGINE_ERROR("ThreadPool: job threw a non-std exception");
        }
    }
}

} // namespace Engine
