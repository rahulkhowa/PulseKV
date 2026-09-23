#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace pulsekv {

// ---------------------------------------------------------------------------
// ThreadPool — Phase 7
//
// A bounded thread pool with a FIFO task queue.
// Workers wait on a condition variable until tasks are enqueued.
// Shuts down gracefully by draining remaining tasks or stopping promptly.
// ---------------------------------------------------------------------------
class ThreadPool {
public:
    // Construct thread pool with specified number of worker threads.
    // Defaults to hardware concurrency (fallback to 4 if 0).
    explicit ThreadPool(std::size_t num_threads = default_thread_count());

    // Destructor: signals shutdown and joins all workers.
    ~ThreadPool();

    // Non-copyable and non-movable.
    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&)                 = delete;
    ThreadPool& operator=(ThreadPool&&)      = delete;

    // Enqueue a task for execution. Returns true if queued, false if stopping.
    bool enqueue(std::function<void()> task);

    // Request shutdown and join all worker threads.
    void shutdown();

    [[nodiscard]] std::size_t thread_count() const noexcept;
    [[nodiscard]] std::size_t queue_size() const;
    [[nodiscard]] bool is_stopped() const noexcept;

    static std::size_t default_thread_count() noexcept {
        auto count = std::thread::hardware_concurrency();
        return (count > 0) ? count : 4;
    }

private:
    void worker_loop();

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex                queue_mutex_;
    std::condition_variable           cv_;
    std::atomic<bool>                 stop_{false};
};

} // namespace pulsekv
