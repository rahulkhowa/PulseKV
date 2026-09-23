#include "pulsekv/server/server.hpp"
#include "pulsekv/protocol/parser.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>

namespace pulsekv {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

Server::Server(std::uint16_t port, Store& store, std::size_t num_threads, WALWriter* wal)
    : port_(port), store_(store), thread_pool_(num_threads), wal_(wal) {}

Server::~Server() {
    request_stop();
    if (listen_fd_ != INVALID_SOCKET) {
        closesocket(listen_fd_);
        listen_fd_ = INVALID_SOCKET;
    }
    if (wsa_ok_) {
        WSACleanup();
        wsa_ok_ = false;
    }
}

// ---------------------------------------------------------------------------
// Winsock initialisation
// ---------------------------------------------------------------------------

bool Server::init_winsock() {
    WSADATA wsa_data{};
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (rc != 0) {
        std::cerr << "[server] WSAStartup failed: " << rc << "\n";
        return false;
    }
    wsa_ok_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// Socket creation
// ---------------------------------------------------------------------------

SOCKET Server::create_listen_socket() {
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) {
        std::cerr << "[server] socket() failed: " << WSAGetLastError() << "\n";
        return INVALID_SOCKET;
    }

    // SO_REUSEADDR: allow quick restart without "address already in use"
    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse)) == SOCKET_ERROR) {
        std::cerr << "[server] setsockopt(SO_REUSEADDR) failed: " << WSAGetLastError() << "\n";
        closesocket(fd);
        return INVALID_SOCKET;
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port_);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "[server] bind() failed on port " << port_
                  << ": " << WSAGetLastError() << "\n";
        closesocket(fd);
        return INVALID_SOCKET;
    }

    if (listen(fd, BACKLOG) == SOCKET_ERROR) {
        std::cerr << "[server] listen() failed: " << WSAGetLastError() << "\n";
        closesocket(fd);
        return INVALID_SOCKET;
    }

    return fd;
}

// ---------------------------------------------------------------------------
// run() — main accept loop (Thread-per-connection in Phase 6)
// ---------------------------------------------------------------------------

int Server::run() {
    if (!init_winsock()) return 1;

    listen_fd_ = create_listen_socket();
    if (listen_fd_ == INVALID_SOCKET) return 1;

    std::cout << "[server] Listening on 0.0.0.0:" << port_ << "\n";
    std::cout << "[server] Press Ctrl+C to stop.\n";

    running_ = true;

    while (!stop_flag_) {
        // Set a timeout on accept() so the stop_flag is checked periodically.
        // We use select() with a 500ms timeout to avoid blocking forever.
        fd_set read_set{};
        FD_ZERO(&read_set);
        FD_SET(listen_fd_, &read_set);

        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 500'000;  // 500 ms

        int ready = select(0, &read_set, nullptr, nullptr, &tv);
        if (ready == SOCKET_ERROR) {
            if (stop_flag_) break;
            std::cerr << "[server] select() error: " << WSAGetLastError() << "\n";
            break;
        }
        if (ready == 0) continue;  // timeout — check stop_flag

        // A connection is ready to accept.
        sockaddr_in client_addr{};
        int addr_len = sizeof(client_addr);
        SOCKET client_fd = accept(listen_fd_,
                                  reinterpret_cast<sockaddr*>(&client_addr),
                                  &addr_len);
        if (client_fd == INVALID_SOCKET) {
            if (stop_flag_) break;
            std::cerr << "[server] accept() failed: " << WSAGetLastError() << "\n";
            continue;
        }

        // Format peer address for logging.
        char peer_buf[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &client_addr.sin_addr, peer_buf, sizeof(peer_buf));
        std::string peer = std::string(peer_buf) + ":" +
                           std::to_string(ntohs(client_addr.sin_port));

        std::cout << "[server] Connected: " << peer << "\n";

        // Phase 7: dispatch connection handling to bounded ThreadPool
        {
            std::lock_guard<std::mutex> lock(sockets_mutex_);
            active_sockets_.push_back(client_fd);
        }
        thread_pool_.enqueue([this, client_fd, peer]() {
            handle_client(client_fd, peer);
            {
                std::lock_guard<std::mutex> lock(sockets_mutex_);
                auto it = std::find(active_sockets_.begin(), active_sockets_.end(), client_fd);
                if (it != active_sockets_.end()) {
                    active_sockets_.erase(it);
                }
            }
            std::cout << "[server] Disconnected: " << peer << "\n";
        });
    }

    // Shut down thread pool (drain or join workers)
    thread_pool_.shutdown();
    {
        std::lock_guard<std::mutex> lock(sockets_mutex_);
        active_sockets_.clear();
    }

    running_ = false;
    std::cout << "[server] Shutdown complete.\n";
    return 0;
}

// ---------------------------------------------------------------------------
// handle_client — receive lines, parse, execute, respond
// ---------------------------------------------------------------------------

void Server::handle_client(SOCKET client_fd, const std::string& peer_addr) {
    char raw_buf[RECV_BUF_SIZE];
    std::string line_buf;  // accumulates partial data between recv() calls

    while (!stop_flag_) {
        int n = recv(client_fd, raw_buf, sizeof(raw_buf), 0);
        if (n == 0) {
            // Client closed connection gracefully.
            break;
        }
        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSAECONNRESET && !stop_flag_) {
                std::cerr << "[server] recv() error from " << peer_addr
                          << ": " << err << "\n";
            }
            break;
        }

        // Append received bytes to our line buffer.
        line_buf.append(raw_buf, static_cast<std::size_t>(n));

        // Guard against oversized requests (potential DoS / misbehaving client).
        if (line_buf.size() > MAX_LINE_BYTES) {
            send_all(client_fd, Response::error("ERR request too large"));
            break;
        }

        // Process all complete lines (terminated by \n).
        std::size_t pos = 0;
        while (true) {
            std::size_t newline = line_buf.find('\n', pos);
            if (newline == std::string::npos) break;

            // Extract the line, stripping the trailing \r\n or \n.
            std::string line = line_buf.substr(pos, newline - pos);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            pos = newline + 1;

            // Parse the line.
            ParseResult result = Parser::parse(line);

            if (result.status == ParseStatus::EMPTY) {
                // Blank line — ignore silently, no response.
                continue;
            }

            if (result.status == ParseStatus::ERR) {
                if (!send_all(client_fd, Response::error(result.error_message))) {
                    goto done;  // send failed — client gone
                }
                continue;
            }

            // OK — execute the command.
            std::string response = execute(result.command);
            if (!send_all(client_fd, response)) {
                goto done;
            }
        }

        // Remove processed bytes from the buffer.
        if (pos > 0) {
            line_buf.erase(0, pos);
        }
    }

done:
    closesocket(client_fd);
}

// ---------------------------------------------------------------------------
// execute — dispatch command to Store, return wire-format response
// Synchronized via store_mutex_ for thread-safety across concurrent clients.
// ---------------------------------------------------------------------------

std::string Server::execute(const Command& cmd) {
    switch (cmd.type) {
        case CommandType::PING:
            return Response::ok("PONG");

        case CommandType::SET:
            // Write-ahead: log BEFORE mutating the store.
            if (wal_) wal_->log_set(cmd.args[0], cmd.args[1], cmd.ttl_seconds);
            store_.set(cmd.args[0], cmd.args[1], cmd.ttl_seconds);
            return Response::ok();

        case CommandType::GET: {
            auto val = store_.get(cmd.args[0]);
            if (!val.has_value()) return Response::null_bulk();
            return Response::ok(val.value());
        }

        case CommandType::DEL: {
            // Write-ahead: log BEFORE mutating the store.
            if (wal_) wal_->log_del(cmd.args[0]);
            bool deleted = store_.del(cmd.args[0]);
            return Response::integer(deleted ? 1 : 0);
        }

        case CommandType::EXISTS: {
            bool found = store_.exists(cmd.args[0]);
            return Response::integer(found ? 1 : 0);
        }

        case CommandType::EXPIRE: {
            // EXPIRE key seconds — returns :1 if key existed, :0 if not
            bool ok = store_.expire(cmd.args[0], cmd.ttl_seconds);
            return Response::integer(ok ? 1 : 0);
        }

        case CommandType::TTL: {
            // TTL key — returns remaining seconds, -1 (no TTL), or -2 (missing/expired)
            long long secs = store_.ttl(cmd.args[0]);
            return Response::integer(secs);
        }

        default:
            return Response::error("ERR internal error");
    }
}

// ---------------------------------------------------------------------------
// send_all — write entire buffer to socket
// ---------------------------------------------------------------------------

bool Server::send_all(SOCKET sock, const std::string& buf) {
    const char* ptr = buf.data();
    int remaining   = static_cast<int>(buf.size());

    while (remaining > 0) {
        int sent = send(sock, ptr, remaining, 0);
        if (sent == SOCKET_ERROR) {
            return false;
        }
        ptr       += sent;
        remaining -= sent;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Stop / status
// ---------------------------------------------------------------------------

void Server::request_stop() {
    stop_flag_ = true;
    {
        std::lock_guard<std::mutex> lock(sockets_mutex_);
        for (SOCKET s : active_sockets_) {
            shutdown(s, SD_BOTH);
        }
    }
    thread_pool_.shutdown();
}

bool Server::is_running() const noexcept {
    return running_;
}

} // namespace pulsekv
