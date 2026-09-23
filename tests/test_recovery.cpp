// tests/test_recovery.cpp
//
// Phase 13: Crash / Recovery Testing for PulseKV
//
// Scenarios verified:
//  1. Clean shutdown & restart sequence
//  2. Torn write / incomplete final record (simulated crash during write)
//  3. Corrupted middle record with valid records before and after
//  4. Repeated operations (SET, DEL, overwrite sequences on same keys)
//  5. Empty WAL and whitespace / newline-only WAL files
//  6. Binary garbage / unparseable records mixed in log
//  7. Large volume recovery (thousands of mutations)
//  8. Cumulative session replay (multiple crash-restart cycles)

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "pulsekv/persistence/wal.hpp"
#include "pulsekv/storage/store.hpp"

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

static const std::string CRASH_WAL = "test_crash_recovery.log";
static void cleanup_wal() { std::remove(CRASH_WAL.c_str()); }

// 1. Clean shutdown & restart
void test_clean_shutdown_restart() {
    cleanup_wal();
    {
        pulsekv::WALWriter w(CRASH_WAL);
        w.log_set("user:100", "Alice", 0);
        w.log_set("user:200", "Bob", 0);
        w.sync();
    } // clean shutdown / close

    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    std::size_t recovered = pulsekv::wal_recover(CRASH_WAL, s);
    CHECK(recovered == 2);
    CHECK(s.get("user:100") == "Alice");
    CHECK(s.get("user:200") == "Bob");
    cleanup_wal();
}

// 2. Torn write / incomplete final record (crash while writing last record)
void test_torn_write_incomplete_record() {
    cleanup_wal();
    // Write valid records, then append a half-written record without newline
    {
        pulsekv::WALWriter w(CRASH_WAL);
        w.log_set("k1", "v1", 0);
        w.log_set("k2", "v2", 0);
    }
    // Simulate crash cutting off write mid-flight
    {
        std::ofstream f(CRASH_WAL, std::ios::app | std::ios::binary);
        f << "SET k3 half_wr"; // truncated, no complete value or ttl or newline
    }

    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    std::size_t recovered = pulsekv::wal_recover(CRASH_WAL, s);
    CHECK(recovered == 2);
    CHECK(s.get("k1") == "v1");
    CHECK(s.get("k2") == "v2");
    CHECK(s.get("k3") == std::nullopt);
    cleanup_wal();
}

// 3. Corrupted middle record (e.g. disk bit flip / bad block)
void test_corrupted_middle_record() {
    cleanup_wal();
    {
        std::ofstream f(CRASH_WAL);
        f << "SET alpha 111 0\r\n";
        f << "CORRUPTED_GARBAGE_LINE_12345\r\n";
        f << "SET beta 222 0\r\n";
        f << "DEL alpha\r\n";
        f << "SET gamma 333 0\r\n";
    }

    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    std::size_t recovered = pulsekv::wal_recover(CRASH_WAL, s);
    // 4 valid commands should be applied (alpha SET, beta SET, alpha DEL, gamma SET)
    CHECK(recovered == 4);
    CHECK(s.get("alpha") == std::nullopt);
    CHECK(s.get("beta") == "222");
    CHECK(s.get("gamma") == "333");
    cleanup_wal();
}

// 4. Repeated operations & overwrites
void test_repeated_operations() {
    cleanup_wal();
    {
        pulsekv::WALWriter w(CRASH_WAL);
        for (int i = 0; i < 50; ++i) {
            w.log_set("counter", std::to_string(i), 0);
        }
        w.log_del("counter");
        w.log_set("counter", "final_value", 0);
    }

    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    std::size_t recovered = pulsekv::wal_recover(CRASH_WAL, s);
    CHECK(recovered == 52);
    CHECK(s.get("counter") == "final_value");
    cleanup_wal();
}

// 5. Empty and whitespace-only WAL
void test_empty_and_whitespace_wal() {
    cleanup_wal();
    {
        std::ofstream f(CRASH_WAL);
        f << "   \r\n\n  \t\r\n   \n";
    }

    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    std::size_t recovered = pulsekv::wal_recover(CRASH_WAL, s);
    CHECK(recovered == 0);
    CHECK(s.size() == 0);
    cleanup_wal();
}

// 6. Multiple crash-restart cycles (cumulative replay)
void test_multiple_crash_restart_cycles() {
    cleanup_wal();

    // Session 1 writes and crashes
    {
        pulsekv::WALWriter w(CRASH_WAL);
        w.log_set("session_key_1", "v1", 0);
        w.log_set("shared_key", "s1", 0);
    }

    // Recover session 1
    {
        pulsekv::Store s1(pulsekv::LockStrategy::SHARED_MUTEX, false);
        CHECK(pulsekv::wal_recover(CRASH_WAL, s1) == 2);
        CHECK(s1.get("shared_key") == "s1");
    }

    // Session 2 appends and crashes
    {
        pulsekv::WALWriter w(CRASH_WAL);
        w.log_set("session_key_2", "v2", 0);
        w.log_set("shared_key", "s2", 0);
    }

    // Recover session 2
    {
        pulsekv::Store s2(pulsekv::LockStrategy::SHARED_MUTEX, false);
        CHECK(pulsekv::wal_recover(CRASH_WAL, s2) == 4);
        CHECK(s2.get("session_key_1") == "v1");
        CHECK(s2.get("session_key_2") == "v2");
        CHECK(s2.get("shared_key") == "s2");
    }

    // Session 3 deletes shared_key and appends torn write
    {
        pulsekv::WALWriter w(CRASH_WAL);
        w.log_del("shared_key");
    }
    {
        std::ofstream f(CRASH_WAL, std::ios::app | std::ios::binary);
        f << "SET session_key_3 partially_wri";
    }

    // Recover session 3
    {
        pulsekv::Store s3(pulsekv::LockStrategy::SHARED_MUTEX, false);
        CHECK(pulsekv::wal_recover(CRASH_WAL, s3) == 5);
        CHECK(s3.get("shared_key") == std::nullopt);
        CHECK(s3.get("session_key_1") == "v1");
        CHECK(s3.get("session_key_2") == "v2");
        CHECK(s3.get("session_key_3") == std::nullopt);
    }

    cleanup_wal();
}

// 7. High volume log recovery
void test_high_volume_recovery() {
    cleanup_wal();
    constexpr int COUNT = 10000;
    {
        pulsekv::WALWriter w(CRASH_WAL);
        for (int i = 0; i < COUNT; ++i) {
            w.log_set("k" + std::to_string(i), "v" + std::to_string(i), 0);
        }
    }

    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, false);
    std::size_t recovered = pulsekv::wal_recover(CRASH_WAL, s);
    CHECK(recovered == COUNT);
    CHECK(s.size() == COUNT);
    CHECK(s.get("k0") == "v0");
    CHECK(s.get("k9999") == "v9999");
    cleanup_wal();
}

int main() {
    std::cout << "PulseKV — Phase 13 Crash & Recovery Tests\n";
    std::cout << "========================================\n\n";

    test_clean_shutdown_restart();
    test_torn_write_incomplete_record();
    test_corrupted_middle_record();
    test_repeated_operations();
    test_empty_and_whitespace_wal();
    test_multiple_crash_restart_cycles();
    test_high_volume_recovery();

    std::cout << "\nResults: " << tests_passed << " / " << tests_run << " passed\n";
    if (tests_passed == tests_run) {
        std::cout << "ALL PHASE 13 RECOVERY TESTS PASSED\n";
        return 0;
    } else {
        std::cout << (tests_run - tests_passed) << " TESTS FAILED\n";
        return 1;
    }
}
