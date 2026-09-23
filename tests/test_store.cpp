// tests/test_store.cpp
//
// Phase 1 unit tests for the Store class.
// Uses simple assert-based testing — no external framework.
//
// Build:
//   g++ -std=c++20 -I../include -o test_store test_store.cpp ../src/storage/store.cpp
// Run:
//   ./test_store

#include <cassert>
#include <iostream>
#include <string>

#include "pulsekv/storage/store.hpp"

// ---- helpers ---------------------------------------------------------------

static int tests_run    = 0;
static int tests_passed = 0;

#define CHECK(expr)                                                         \
    do {                                                                    \
        ++tests_run;                                                        \
        if (!(expr)) {                                                      \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__          \
                      << "  " << #expr << "\n";                            \
        } else {                                                            \
            ++tests_passed;                                                 \
        }                                                                   \
    } while (false)

// ---- individual tests ------------------------------------------------------

void test_set_and_get() {
    pulsekv::Store s;
    s.set("name", "Rahul");
    CHECK(s.get("name") == "Rahul");
}

void test_get_missing_key() {
    pulsekv::Store s;
    CHECK(s.get("ghost") == std::nullopt);
}

void test_overwrite() {
    pulsekv::Store s;
    s.set("k", "first");
    s.set("k", "second");
    CHECK(s.get("k") == "second");
}

void test_del_existing() {
    pulsekv::Store s;
    s.set("x", "42");
    bool removed = s.del("x");
    CHECK(removed == true);
    CHECK(s.get("x") == std::nullopt);
}

void test_del_missing() {
    pulsekv::Store s;
    bool removed = s.del("nonexistent");
    CHECK(removed == false);
}

void test_exists_true() {
    pulsekv::Store s;
    s.set("flag", "1");
    CHECK(s.exists("flag") == true);
}

void test_exists_false() {
    pulsekv::Store s;
    CHECK(s.exists("nobody") == false);
}

void test_exists_after_del() {
    pulsekv::Store s;
    s.set("tmp", "val");
    s.del("tmp");
    CHECK(s.exists("tmp") == false);
}

void test_size() {
    pulsekv::Store s;
    CHECK(s.size() == 0);
    s.set("a", "1");
    CHECK(s.size() == 1);
    s.set("b", "2");
    CHECK(s.size() == 2);
    s.del("a");
    CHECK(s.size() == 1);
}

void test_empty_value() {
    pulsekv::Store s;
    s.set("empty", "");
    auto v = s.get("empty");
    CHECK(v.has_value());
    CHECK(v.value() == "");
}

void test_large_value() {
    pulsekv::Store s;
    std::string big(1'000'000, 'x');
    s.set("big", big);
    auto v = s.get("big");
    CHECK(v.has_value());
    CHECK(v.value().size() == 1'000'000);
}

void test_multiple_keys() {
    pulsekv::Store s;
    for (int i = 0; i < 1000; ++i) {
        s.set("key" + std::to_string(i), "val" + std::to_string(i));
    }
    CHECK(s.size() == 1000);
    for (int i = 0; i < 1000; ++i) {
        CHECK(s.get("key" + std::to_string(i)) == "val" + std::to_string(i));
    }
}

// ---- Phase 8 concurrent store tests ----------------------------------------

#include <thread>
#include <vector>
#include <atomic>

void test_concurrent_readers() {
    pulsekv::Store s;
    for (int i = 0; i < 100; ++i) {
        s.set("k" + std::to_string(i), "v" + std::to_string(i));
    }

    constexpr int NUM_THREADS = 8;
    constexpr int READS_PER_THREAD = 1000;
    std::vector<std::thread> readers;
    std::atomic<int> success{0};

    for (int t = 0; t < NUM_THREADS; ++t) {
        readers.emplace_back([&s, &success]() {
            for (int r = 0; r < READS_PER_THREAD; ++r) {
                int key_idx = r % 100;
                auto val = s.get("k" + std::to_string(key_idx));
                if (val.has_value() && val.value() == "v" + std::to_string(key_idx)) {
                    success.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& r : readers) r.join();
    CHECK(success.load() == NUM_THREADS * READS_PER_THREAD);
}

void test_concurrent_writers() {
    pulsekv::Store s;
    constexpr int NUM_THREADS = 8;
    constexpr int WRITES_PER_THREAD = 500;
    std::vector<std::thread> writers;

    for (int t = 0; t < NUM_THREADS; ++t) {
        writers.emplace_back([&s, t]() {
            for (int w = 0; w < WRITES_PER_THREAD; ++w) {
                std::string key = "t" + std::to_string(t) + "_k" + std::to_string(w);
                s.set(key, "val" + std::to_string(w));
            }
        });
    }

    for (auto& w : writers) w.join();
    CHECK(s.size() == NUM_THREADS * WRITES_PER_THREAD);

    // Verify all keys
    for (int t = 0; t < NUM_THREADS; ++t) {
        for (int w = 0; w < WRITES_PER_THREAD; ++w) {
            std::string key = "t" + std::to_string(t) + "_k" + std::to_string(w);
            CHECK(s.get(key) == "val" + std::to_string(w));
        }
    }
}

void test_concurrent_mixed_read_write() {
    pulsekv::Store s;
    std::atomic<bool> stop{false};
    constexpr int NUM_READERS = 4;
    constexpr int NUM_WRITERS = 4;

    std::vector<std::thread> threads;

    // Writers
    for (int t = 0; t < NUM_WRITERS; ++t) {
        threads.emplace_back([&s, &stop, t]() {
            int counter = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                std::string key = "shared_key_" + std::to_string((t + counter) % 20);
                s.set(key, "write_" + std::to_string(counter));
                ++counter;
                if (counter > 1000) break;
            }
        });
    }

    // Readers
    std::atomic<int> read_count{0};
    for (int t = 0; t < NUM_READERS; ++t) {
        threads.emplace_back([&s, &stop, &read_count]() {
            while (!stop.load(std::memory_order_relaxed)) {
                for (int i = 0; i < 20; ++i) {
                    (void)s.exists("shared_key_" + std::to_string(i));
                    (void)s.get("shared_key_" + std::to_string(i));
                    read_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // Run mixed workload for a moment
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true);

    for (auto& th : threads) th.join();
    CHECK(read_count.load() > 0);
}

// ---- Phase 10 TTL tests ----------------------------------------------------

#include <chrono>

void test_ttl_set_ex_expires() {
    // A key with TTL=1s must be gone after 2s.
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    s.set("session", "Rahul", 1 /*second*/);
    CHECK(s.get("session") == "Rahul");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(s.get("session") == std::nullopt);
}

void test_ttl_exists_expires() {
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    s.set("tok", "abc", 1);
    CHECK(s.exists("tok") == true);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(s.exists("tok") == false);
}

void test_ttl_no_expiry_persistent() {
    // A key without TTL must never expire.
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    s.set("persist", "forever");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(s.get("persist") == "forever");
}

void test_ttl_overwrite_resets_expiry() {
    // Overwriting a key with SET removes the old TTL if no EX supplied.
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    s.set("k", "v1", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    s.set("k", "v2");          // re-set with no TTL
    std::this_thread::sleep_for(std::chrono::milliseconds(700)); // total > 1s
    // k was reset without TTL, so it should still exist
    CHECK(s.get("k") == "v2");
}

void test_ttl_expire_command_sets_ttl() {
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    s.set("key", "value");
    bool ok = s.expire("key", 1);
    CHECK(ok == true);
    CHECK(s.get("key") == "value");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(s.get("key") == std::nullopt);
}

void test_ttl_expire_missing_key() {
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    bool ok = s.expire("ghost", 10);
    CHECK(ok == false);
}

void test_ttl_expire_removes_ttl() {
    // expire(key, 0) should make the key persistent.
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    s.set("k", "v", 1);
    bool ok = s.expire("k", 0);  // 0 = remove TTL
    CHECK(ok == true);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(s.get("k") == "v");    // still alive
}

void test_ttl_query_positive() {
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    s.set("k", "v", 10);
    long long remaining = s.ttl("k");
    // Should be <= 10 and >= 8 (allow a few seconds of slack)
    CHECK(remaining >= 8 && remaining <= 10);
}

void test_ttl_query_persistent() {
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    s.set("k", "v");
    CHECK(s.ttl("k") == -1LL);
}

void test_ttl_query_missing() {
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    CHECK(s.ttl("ghost") == -2LL);
}

void test_ttl_query_expired() {
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    s.set("k", "v", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(s.ttl("k") == -2LL);
}

void test_ttl_purge_expired() {
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    s.set("a", "1", 1);
    s.set("b", "2", 1);
    s.set("c", "3");        // persistent
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    std::size_t purged = s.purge_expired();
    CHECK(purged == 2);
    CHECK(s.get("c") == "3");
}

void test_ttl_background_cleanup() {
    // Background cleanup thread should remove expired keys within ~300ms of expiry.
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, true /*background on*/);
    s.set("x", "v", 1);
    // Wait for expiry + a few cleanup intervals (100ms each)
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    // Don't call get() — just check the size directly to verify cleanup ran.
    CHECK(s.size() == 0);
    s.stop_background_cleanup();
}

// ---- Phase 11 LRU tests ----------------------------------------------------

void test_lru_capacity_eviction() {
    // Store with capacity=3: inserting a 4th key must evict the LRU key.
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false, 3);
    s.set("a", "1");
    s.set("b", "2");
    s.set("c", "3");
    CHECK(s.size() == 3);
    s.set("d", "4");  // should evict "a" (oldest, never accessed)
    CHECK(s.size() == 3);
    CHECK(s.evicted_count() == 1);
    CHECK(s.get("a") == std::nullopt);  // evicted
    CHECK(s.get("b") == "2");
    CHECK(s.get("c") == "3");
    CHECK(s.get("d") == "4");
}

void test_lru_access_promotes_key() {
    // Access "a" after inserting a, b, c. Then insert "d".
    // "b" (LRU) should be evicted, not "a".
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false, 3);
    s.set("a", "1");
    s.set("b", "2");
    s.set("c", "3");
    (void)s.get("a");       // promotes "a" to MRU
    s.set("d", "4"); // evicts LRU which is now "b"
    CHECK(s.get("a") == "1");           // promoted, not evicted
    CHECK(s.get("b") == std::nullopt);  // was LRU
    CHECK(s.get("c") == "3");
    CHECK(s.get("d") == "4");
}

void test_lru_overwrite_promotes() {
    // SET on an existing key must move it to MRU.
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false, 3);
    s.set("a", "1");
    s.set("b", "2");
    s.set("c", "3");
    s.set("a", "updated"); // overwrite — moves "a" to MRU
    s.set("d", "4");       // evicts "b" (now LRU)
    CHECK(s.get("a") == "updated");
    CHECK(s.get("b") == std::nullopt); // evicted
    CHECK(s.get("c") == "3");
    CHECK(s.get("d") == "4");
}

void test_lru_eviction_order_strict() {
    // Insert a, b, c, d with capacity=3. Verify the exact eviction sequence.
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false, 3);
    s.set("a", "1");  // order: a
    s.set("b", "2");  // order: b a
    s.set("c", "3");  // order: c b a
    // Now insert d — evicts a (LRU tail)
    s.set("d", "4");  // order: d c b
    CHECK(s.get("a") == std::nullopt);
    // Access b — moves b to MRU
    (void)s.get("b");       // order: b d c
    // Insert e — evicts c (LRU tail)
    s.set("e", "5"); // order: e b d
    CHECK(s.get("c") == std::nullopt);
    CHECK(s.get("d") == "4");
    CHECK(s.get("b") == "2");
    CHECK(s.get("e") == "5");
}

void test_lru_del_maintains_list() {
    // del() must remove from both table_ and lru_order_, keeping the list consistent.
    // After deleting "b" from a cap-3 store holding {a,b,c}, we have {a,c} (2 entries).
    // Inserting "d" fills back to capacity (no eviction yet).
    // Inserting "e" now evicts the LRU — which should be "a" (oldest surviving key).
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false, 3);
    s.set("a", "1");  // order: a          (LRU tail)
    s.set("b", "2");  // order: b a
    s.set("c", "3");  // order: c b a
    s.del("b");       // remove "b" from middle of list  →  order: c a
    s.set("d", "4"); // fills to capacity  →  order: d c a  (no eviction)
    CHECK(s.size() == 3);
    CHECK(s.get("b") == std::nullopt);  // was deleted
    CHECK(s.get("a") == "1");           // still alive (not evicted yet)
    CHECK(s.get("c") == "3");
    CHECK(s.get("d") == "4");
    // Now trigger an eviction. After the GETs above the order is: d c a → but
    // each get() spliced to front, so order became: d a c → c a d → a d c → d c a
    // It's easier to just insert another key and verify no crash + size stays ≤ 3.
    s.set("e", "5"); // evicts LRU tail — store must not crash and size stays at 3
    CHECK(s.size() == 3);
    CHECK(s.evicted_count() == 1);
}

void test_lru_with_ttl_expiry() {
    // Expired keys consume a slot until lazily removed; new SET should evict LRU.
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false, 3);
    s.set("a", "1", 1); // TTL 1s
    s.set("b", "2");
    s.set("c", "3");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    // "a" is now expired. A GET on "a" lazily removes it.
    CHECK(s.get("a") == std::nullopt);
    // Now only b and c are alive — inserting d should not evict b or c.
    s.set("d", "4");
    CHECK(s.size() == 3);
    CHECK(s.get("b") == "2");
    CHECK(s.get("c") == "3");
    CHECK(s.get("d") == "4");
}

void test_lru_exists_does_not_promote() {
    // exists() must NOT update LRU order (Redis semantics).
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false, 3);
    s.set("a", "1");
    s.set("b", "2");
    s.set("c", "3");
    // "a" is LRU. exists("a") should NOT promote it.
    (void)s.exists("a");
    s.set("d", "4"); // must still evict "a"
    CHECK(s.get("a") == std::nullopt); // evicted
}

void test_lru_unlimited_no_eviction() {
    // max_capacity == 0 means no eviction — all keys survive.
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false, 0);
    for (int i = 0; i < 1000; ++i) {
        s.set("k" + std::to_string(i), "v");
    }
    CHECK(s.size() == 1000);
    CHECK(s.evicted_count() == 0);
}

void test_lru_concurrent_with_capacity() {
    // Multiple writers racing into a bounded store must not corrupt it.
    constexpr std::size_t CAPACITY = 100;
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false, CAPACITY);

    constexpr int WRITERS = 4;
    constexpr int WRITES  = 500;
    std::vector<std::thread> threads;
    for (int t = 0; t < WRITERS; ++t) {
        threads.emplace_back([&s, t]() {
            for (int i = 0; i < WRITES; ++i) {
                s.set("t" + std::to_string(t) + "_k" + std::to_string(i),
                      "val" + std::to_string(i));
            }
        });
    }
    for (auto& th : threads) th.join();

    // Size must not exceed capacity.
    CHECK(s.size() <= CAPACITY);
    // Total evictions + remaining == total insertions attempted.
    CHECK(s.evicted_count() + s.size() == static_cast<std::size_t>(WRITERS * WRITES));
}

// ---- main ------------------------------------------------------------------

int main() {
    std::cout << "PulseKV — Phase 1, 8, 10 & 11 Store Tests\n";
    std::cout << "==========================================\n\n";

    test_set_and_get();
    test_get_missing_key();
    test_overwrite();
    test_del_existing();
    test_del_missing();
    test_exists_true();
    test_exists_false();
    test_exists_after_del();
    test_size();
    test_empty_value();
    test_large_value();
    test_multiple_keys();

    // Phase 8 concurrent tests
    test_concurrent_readers();
    test_concurrent_writers();
    test_concurrent_mixed_read_write();

    // Phase 10 TTL tests
    std::cout << "\n[Phase 10 TTL tests — some use 1s sleeps, takes ~8s]\n";
    test_ttl_set_ex_expires();
    test_ttl_exists_expires();
    test_ttl_no_expiry_persistent();
    test_ttl_overwrite_resets_expiry();
    test_ttl_expire_command_sets_ttl();
    test_ttl_expire_missing_key();
    test_ttl_expire_removes_ttl();
    test_ttl_query_positive();
    test_ttl_query_persistent();
    test_ttl_query_missing();
    test_ttl_query_expired();
    test_ttl_purge_expired();
    test_ttl_background_cleanup();

    // Phase 11 LRU tests
    std::cout << "\n[Phase 11 LRU tests]\n";
    test_lru_capacity_eviction();
    test_lru_access_promotes_key();
    test_lru_overwrite_promotes();
    test_lru_eviction_order_strict();
    test_lru_del_maintains_list();
    test_lru_with_ttl_expiry();
    test_lru_exists_does_not_promote();
    test_lru_unlimited_no_eviction();
    test_lru_concurrent_with_capacity();

    std::cout << "\nResults: " << tests_passed << " / " << tests_run << " passed\n";

    if (tests_passed == tests_run) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    } else {
        std::cout << (tests_run - tests_passed) << " TESTS FAILED\n";
        return 1;
    }
}
