#pragma once
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <vector>

namespace Engine {

// Fixed-size FIFO worker pool. Submit/Enqueue are thread-safe (jobs may enqueue jobs).
// The destructor runs every job that is still queued, then joins the workers.
class ThreadPool {
public:
    explicit ThreadPool(std::uint32_t threadCount = 0); // 0 = DefaultThreadCount()
    ~ThreadPool();

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Exceptions thrown by `fn` are rethrown by future::get().
    template <class F>
        requires std::invocable<F&>
    [[nodiscard]] auto Submit(F&& fn) -> std::future<std::invoke_result_t<F&>>
    {
        using R   = std::invoke_result_t<F&>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn)); // std::function needs copyable
        std::future<R> future = task->get_future();
        Enqueue([task] { (*task)(); });
        return future;
    }

    // Fire-and-forget. An escaping exception is logged and swallowed.
    void Enqueue(std::function<void()> job);

    [[nodiscard]] std::uint32_t ThreadCount() const { return static_cast<std::uint32_t>(m_Workers.size()); }

    // hardware_concurrency - 1 (the main thread keeps a core), at least 1.
    [[nodiscard]] static std::uint32_t DefaultThreadCount();

private:
    void WorkerLoop(std::stop_token stop);

    std::mutex                        m_Mutex;
    std::condition_variable_any       m_Wake;
    std::deque<std::function<void()>> m_Jobs;
    std::vector<std::jthread>         m_Workers; // last member: joined before the queue dies
};

} // namespace Engine
