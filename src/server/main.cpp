// src/server/main.cpp
//
// PulseKV server entry point — Phase 7, 8, 9, 10, 11, 12.
//
// Usage:
//   pulsekv-server [--port N] [--threads N] [--lock mutex|shared]
//                  [--max-keys N] [--wal <path>]

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "pulsekv/server/server.hpp"
#include "pulsekv/storage/store.hpp"
#include "pulsekv/persistence/wal.hpp"

static pulsekv::Server* g_server = nullptr;

static BOOL WINAPI ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT ||
        ctrl_type == CTRL_CLOSE_EVENT) {
        std::cout << "\n[main] Shutdown requested...\n";
        if (g_server) {
            g_server->request_stop();
        }
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char* argv[]) {
    std::uint16_t port        = pulsekv::Server::DEFAULT_PORT;
    std::size_t   num_threads = 0;
    std::size_t   max_keys    = 0;   // 0 = unlimited (LRU disabled)
    std::string   wal_path;          // empty = WAL disabled
    pulsekv::LockStrategy lock_strat = pulsekv::LockStrategy::SHARED_MUTEX;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--threads" && i + 1 < argc) {
            num_threads = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--lock" && i + 1 < argc) {
            std::string strat = argv[++i];
            lock_strat = (strat == "mutex") ? pulsekv::LockStrategy::MUTEX
                                            : pulsekv::LockStrategy::SHARED_MUTEX;
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--max-keys" && i + 1 < argc) {
            max_keys = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--wal" && i + 1 < argc) {
            wal_path = argv[++i];
        } else if (arg[0] != '-') {
            try {
                int p = std::stoi(arg);
                if (p >= 1 && p <= 65535) port = static_cast<std::uint16_t>(p);
            } catch (...) {}
        }
    }

    std::cout << "PulseKV Server (Phase 12 — WAL)\n";
    std::cout << "Port:     " << port << "\n";
    std::cout << "Workers:  "
              << (num_threads ? num_threads : pulsekv::ThreadPool::default_thread_count())
              << "\n";
    std::cout << "Locking:  "
              << (lock_strat == pulsekv::LockStrategy::MUTEX ? "Exclusive Mutex"
                                                             : "Shared Mutex (RW)")
              << "\n";
    std::cout << "Max keys: "
              << (max_keys ? std::to_string(max_keys) : "unlimited (LRU off)") << "\n";
    std::cout << "WAL:      "
              << (wal_path.empty() ? "disabled" : wal_path) << "\n";
    std::cout << "------------------------------------------\n";

    // --- Create store ---
    pulsekv::Store store(lock_strat, true /*background cleanup*/, max_keys);

    // --- WAL recovery: replay log before accepting clients ---
    std::unique_ptr<pulsekv::WALWriter> wal;
    if (!wal_path.empty()) {
        // Recover existing WAL (no-op if file does not exist yet).
        std::size_t replayed = pulsekv::wal_recover(wal_path, store);
        std::cout << "[main] WAL recovery complete. " << replayed
                  << " operations replayed.\n";

        // Open WAL writer for this session (appends to existing file).
        try {
            wal = std::make_unique<pulsekv::WALWriter>(wal_path);
        } catch (const std::exception& ex) {
            std::cerr << "[main] " << ex.what() << "\n";
            return 1;
        }
    }

    pulsekv::Server server(port, store, num_threads, wal.get());

    g_server = &server;
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    return server.run();
}
