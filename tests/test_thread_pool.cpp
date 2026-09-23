#include "pulsekv/concurrency/thread_pool.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

void test_basic_execution() {
    pulsekv::ThreadPool pool(4);
    assert(pool.thread_count() == 4);
    assert(!pool.is_stopped());

    std::atomic<int> counter{0};
    constexpr int N = 100;

    for (int i = 0; i < N; ++i) {
        bool ok = pool.enqueue([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
        assert(ok);
    }

    pool.shutdown();
    assert(counter.load() == N);
    assert(pool.is_stopped());
}

void test_parallel_execution() {
    pulsekv::ThreadPool pool(4);
    std::atomic<int> active_workers{0};
    std::atomic<int> max_concurrent{0};
    constexpr int N = 20;

    for (int i = 0; i < N; ++i) {
        pool.enqueue([&]() {
            int cur = active_workers.fetch_add(1) + 1;
            int prev_max = max_concurrent.load();
            while (cur > prev_max && !max_concurrent.compare_exchange_weak(prev_max, cur)) {}

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            active_workers.fetch_sub(1);
        });
    }

    pool.shutdown();
    // At least 2 workers ran concurrently on a 4-thread pool
    assert(max_concurrent.load() >= 2);
}

void test_heavy_task_load() {
    pulsekv::ThreadPool pool(8);
    constexpr int N = 10000;
    std::atomic<int> sum{0};

    for (int i = 1; i <= N; ++i) {
        pool.enqueue([&sum, i]() {
            sum.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool.shutdown();
    assert(sum.load() == N);
}

void test_enqueue_after_shutdown() {
    pulsekv::ThreadPool pool(2);
    pool.shutdown();
    assert(pool.is_stopped());

    bool ok = pool.enqueue([]() {});
    assert(!ok && "enqueue after shutdown must return false");
}

} // namespace

int main() {
    std::cout << "PulseKV — Phase 7 ThreadPool Tests\n";
    std::cout << "==================================\n\n";

    int passed = 0;
    auto run = [&](const std::string& name, auto fn) {
        std::cout << "  [TEST] " << name << "... ";
        fn();
        std::cout << "PASSED\n";
        ++passed;
    };

    run("Basic task execution", test_basic_execution);
    run("Parallel worker concurrency", test_parallel_execution);
    run("Heavy task load (10,000 tasks)", test_heavy_task_load);
    run("Enqueue after shutdown rejected", test_enqueue_after_shutdown);

    std::cout << "\nResults: " << passed << " / " << passed << " passed\n";
    std::cout << "ALL THREADPOOL TESTS PASSED\n";
    return 0;
}
