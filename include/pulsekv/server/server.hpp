#pragma once

// ---------------------------------------------------------------------------
// Include pulsekv headers BEFORE any Windows headers to avoid macro
// pollution. Windows defines ERROR=0, OPTIONAL=0, etc. which break enums.
// ---------------------------------------------------------------------------
#include "pulsekv/protocol/parser.hpp"
#include "pulsekv/storage/store.hpp"
#include "pulsekv/persistence/wal.hpp"

// ---------------------------------------------------------------------------
// Platform: Windows only — Winsock2
// Must come after our own headers to avoid macro contamination.
// ---------------------------------------------------------------------------
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

// Windows defines ERROR=0 which collides with enum names. Undefine it here.
#ifdef ERROR
#undef ERROR
#endif

#include "pulsekv/concurrency/thread_pool.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace pulsekv {

// ---------------------------------------------------------------------------
// Server — Phase 7+12: Bounded ThreadPool TCP server with optional WAL
//
// Lifecycle:
//   Server srv(port, store, num_threads, wal);
//   srv.run();   // blocks until shutdown
//
// WAL:
//   Pass a non-null WALWriter* to enable write-ahead logging.
//   Every SET and DEL command is logged BEFORE being applied to the store.
//   Pass nullptr (the default) to disable WAL (existing behaviour).
// ---------------------------------------------------------------------------
class Server {
public:
    static constexpr std::uint16_t DEFAULT_PORT    = 7379;
    static constexpr int           BACKLOG         = 128;
    static constexpr int           RECV_BUF_SIZE   = 4096;
    static constexpr int           MAX_LINE_BYTES  = 65536;  // 64 KiB per line

    explicit Server(std::uint16_t port, Store& store,
                   std::size_t   num_threads = 0,
                   WALWriter*    wal         = nullptr);
    ~Server();

    // Disallow copy and move — the server owns the listening socket.
    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&)                 = delete;
    Server& operator=(Server&&)      = delete;

    // Block and serve clients until request_stop() is called.
    // Returns 0 on clean shutdown, non-zero on fatal error.
    int run();

    // Signal the run() loop to exit and shuts down the thread pool.
    void request_stop();

    [[nodiscard]] bool is_running() const noexcept;

private:
    // Winsock initialisation / cleanup (RAII wrapper).
    bool init_winsock();

    // Create, bind, and start listening on the server socket.
    SOCKET create_listen_socket();

    // Handle one connected client to completion on a worker thread.
    void handle_client(SOCKET client_fd, const std::string& peer_addr);

    // Execute a parsed command against the store and return the wire response.
    // Synchronized via store_mutex_.
    [[nodiscard]] std::string execute(const Command& cmd);

    // Send all bytes in buf over sock. Returns false on error.
    bool send_all(SOCKET sock, const std::string& buf);

    std::uint16_t            port_;
    Store&                   store_;
    SOCKET                   listen_fd_  = INVALID_SOCKET;
    std::atomic<bool>        stop_flag_  = false;
    std::atomic<bool>        running_    = false;
    bool                     wsa_ok_     = false;

    // Concurrency (Phase 8: Store is thread-safe):
    ThreadPool               thread_pool_;      // Bounded worker pool
    std::mutex               sockets_mutex_;    // Protects active_sockets_
    std::vector<SOCKET>      active_sockets_;   // Open client sockets for shutdown

    // Phase 12: optional Write-Ahead Log (nullptr = disabled)
    WALWriter*               wal_ = nullptr;
};

} // namespace pulsekv
