#include "pulsekv/concurrency/thread_pool.hpp"

namespace pulsekv {

ThreadPool::ThreadPool(std::size_t num_threads) {
    if (num_threads == 0) {
        num_threads = default_thread_count();
    }
    workers_.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&ThreadPool::worker_loop, this);
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

bool ThreadPool::enqueue(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (stop_) {
            return false;
        }
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (stop_) return;
        stop_ = true;
    }
    cv_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

std::size_t ThreadPool::thread_count() const noexcept {
    return workers_.size();
}

std::size_t ThreadPool::queue_size() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return tasks_.size();
}

bool ThreadPool::is_stopped() const noexcept {
    return stop_.load();
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, [this]() {
                return stop_ || !tasks_.empty();
            });

            if (stop_ && tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        if (task) {
            task();
        }
    }
}

} // namespace pulsekv
