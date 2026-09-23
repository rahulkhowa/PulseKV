// tests/test_wal.cpp
//
// Phase 12 unit tests for WALWriter and wal_recover().
// Tests write records, close the writer, then recover into a fresh Store
// and verify state.

#include <cassert>
#include <cstdio>       // std::remove
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "pulsekv/persistence/wal.hpp"
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

// Temporary file path helper — delete before and after each test.
static const std::string WAL_TMP = "test_wal_tmp.log";

static void remove_wal() { std::remove(WAL_TMP.c_str()); }

// ---- individual tests ------------------------------------------------------

void test_wal_write_and_recover_set() {
    remove_wal();
    {
        pulsekv::WALWriter w(WAL_TMP);
        w.log_set("name", "Rahul", 0);
        w.log_set("city", "Delhi", 0);
    }

    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    std::size_t n = pulsekv::wal_recover(WAL_TMP, s);
    CHECK(n == 2);
    CHECK(s.get("name") == "Rahul");
    CHECK(s.get("city") == "Delhi");
    remove_wal();
}

void test_wal_recover_del() {
    remove_wal();
    {
        pulsekv::WALWriter w(WAL_TMP);
        w.log_set("key", "value", 0);
        w.log_del("key");
    }

    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    std::size_t n = pulsekv::wal_recover(WAL_TMP, s);
    CHECK(n == 2);
    CHECK(s.get("key") == std::nullopt);
    remove_wal();
}

void test_wal_recover_overwrite() {
    remove_wal();
    {
        pulsekv::WALWriter w(WAL_TMP);
        w.log_set("k", "first",  0);
        w.log_set("k", "second", 0);
    }

    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    pulsekv::wal_recover(WAL_TMP, s);
    CHECK(s.get("k") == "second");
    remove_wal();
}

void test_wal_recover_with_ttl() {
    remove_wal();
    {
        pulsekv::WALWriter w(WAL_TMP);
        // Record with TTL. After recovery the key has a NEW TTL relative to
        // the recovery time — the original wall-clock offset is lost.
        // This is a known limitation: TTL-with-WAL only provides approximate
        // expiry after restart. We just verify recovery doesn't crash.
        w.log_set("session", "abc123", 3600);
    }

    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    pulsekv::wal_recover(WAL_TMP, s);
    // Key must exist immediately after recovery.
    CHECK(s.get("session") == "abc123");
    // TTL must be positive (freshly set to 3600s from now).
    CHECK(s.ttl("session") > 0);
    remove_wal();
}

void test_wal_no_file_is_clean_start() {
    remove_wal();
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    // wal_recover must return 0 and not throw if the file doesn't exist.
    std::size_t n = pulsekv::wal_recover(WAL_TMP, s);
    CHECK(n == 0);
    CHECK(s.size() == 0);
}

void test_wal_records_written_counter() {
    remove_wal();
    pulsekv::WALWriter w(WAL_TMP);
    CHECK(w.records_written() == 0);
    w.log_set("a", "1", 0);
    CHECK(w.records_written() == 1);
    w.log_del("a");
    CHECK(w.records_written() == 2);
    remove_wal();
}

void test_wal_path_accessor() {
    remove_wal();
    pulsekv::WALWriter w(WAL_TMP);
    CHECK(w.path() == WAL_TMP);
    remove_wal();
}

void test_wal_append_across_restarts() {
    // Simulate two server sessions: first writes 3 keys, second adds 2 more.
    // Recovery after the second session must see all 5 keys.
    remove_wal();

    // Session 1
    {
        pulsekv::WALWriter w(WAL_TMP);
        w.log_set("a", "1", 0);
        w.log_set("b", "2", 0);
        w.log_set("c", "3", 0);
    }

    // Session 2 (file already exists — must append, not truncate)
    {
        pulsekv::WALWriter w(WAL_TMP);
        w.log_set("d", "4", 0);
        w.log_set("e", "5", 0);
    }

    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    std::size_t n = pulsekv::wal_recover(WAL_TMP, s);
    CHECK(n == 5);
    CHECK(s.get("a") == "1");
    CHECK(s.get("b") == "2");
    CHECK(s.get("c") == "3");
    CHECK(s.get("d") == "4");
    CHECK(s.get("e") == "5");
    remove_wal();
}

void test_wal_empty_file() {
    remove_wal();
    // Create an empty file.
    { std::ofstream f(WAL_TMP); }
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    std::size_t n = pulsekv::wal_recover(WAL_TMP, s);
    CHECK(n == 0);
    remove_wal();
}

void test_wal_malformed_records_skipped() {
    remove_wal();
    // Write a mix of valid and malformed records directly.
    {
        std::ofstream f(WAL_TMP);
        f << "SET valid_key valid_value 0\r\n";
        f << "GARBAGE\r\n";                    // unknown type
        f << "SET\r\n";                         // SET with no args
        f << "DEL\r\n";                         // DEL with no args
        f << "SET another value 0\r\n";
    }

    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    std::size_t n = pulsekv::wal_recover(WAL_TMP, s);
    // Only the 2 well-formed SET records replay successfully.
    CHECK(n == 2);
    CHECK(s.get("valid_key") == "valid_value");
    CHECK(s.get("another")   == "value");
    remove_wal();
}

void test_wal_concurrent_writers() {
    // Multiple threads writing to the same WALWriter must not corrupt the file.
    remove_wal();
    constexpr int THREADS = 4;
    constexpr int WRITES  = 100;

    {
        pulsekv::WALWriter w(WAL_TMP);
        std::vector<std::thread> threads;
        for (int t = 0; t < THREADS; ++t) {
            threads.emplace_back([&w, t]() {
                for (int i = 0; i < WRITES; ++i) {
                    w.log_set("t" + std::to_string(t) + "k" + std::to_string(i),
                              "v" + std::to_string(i), 0);
                }
            });
        }
        for (auto& th : threads) th.join();
        CHECK(w.records_written() == static_cast<std::size_t>(THREADS * WRITES));
    }

    // All records must be recoverable.
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    std::size_t n = pulsekv::wal_recover(WAL_TMP, s);
    CHECK(n == static_cast<std::size_t>(THREADS * WRITES));
    remove_wal();
}

// ---- main ------------------------------------------------------------------

int main() {
    std::cout << "PulseKV — Phase 12 WAL Tests\n";
    std::cout << "============================\n\n";

    test_wal_write_and_recover_set();
    test_wal_recover_del();
    test_wal_recover_overwrite();
    test_wal_recover_with_ttl();
    test_wal_no_file_is_clean_start();
    test_wal_records_written_counter();
    test_wal_path_accessor();
    test_wal_append_across_restarts();
    test_wal_empty_file();
    test_wal_malformed_records_skipped();
    test_wal_concurrent_writers();

    std::cout << "\nResults: " << tests_passed << " / " << tests_run << " passed\n";
    if (tests_passed == tests_run) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    } else {
        std::cout << (tests_run - tests_passed) << " TESTS FAILED\n";
        return 1;
    }
}
