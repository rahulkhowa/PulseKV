// client/main.cpp
//
// PulseKV command-line client — Phase 4
//
// Usage:
//   pulsekv-client [host] [port]
//
// Defaults: host=127.0.0.1  port=7379
//
// Interactive session:
//   pulsekv> SET name Rahul
//   +OK
//   pulsekv> GET name
//   +Rahul
//   pulsekv> quit

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

// ---------------------------------------------------------------------------
// RAII Winsock guard
// ---------------------------------------------------------------------------
struct WinsockGuard {
    bool ok = false;
    WinsockGuard() {
        WSADATA d{};
        ok = (WSAStartup(MAKEWORD(2, 2), &d) == 0);
    }
    ~WinsockGuard() { if (ok) WSACleanup(); }
};

// ---------------------------------------------------------------------------
// RAII socket wrapper
// ---------------------------------------------------------------------------
struct Socket {
    SOCKET fd = INVALID_SOCKET;
    explicit Socket(SOCKET s) : fd(s) {}
    ~Socket() { if (fd != INVALID_SOCKET) closesocket(fd); }
    Socket(const Socket&)            = delete;
    Socket& operator=(const Socket&) = delete;
    bool valid() const { return fd != INVALID_SOCKET; }
};

// ---------------------------------------------------------------------------
// send_line — write "line\r\n" fully
// ---------------------------------------------------------------------------
static bool send_line(SOCKET fd, const std::string& line) {
    std::string msg = line + "\r\n";
    const char* p = msg.data();
    int rem = static_cast<int>(msg.size());
    while (rem > 0) {
        int n = send(fd, p, rem, 0);
        if (n == SOCKET_ERROR) return false;
        p   += n;
        rem -= n;
    }
    return true;
}

// ---------------------------------------------------------------------------
// recv_line — read until '\n', returns the line (without \r\n)
// ---------------------------------------------------------------------------
static bool recv_line(SOCKET fd, std::string& out) {
    out.clear();
    char c = 0;
    while (true) {
        int n = recv(fd, &c, 1, 0);
        if (n <= 0) return false;     // disconnected or error
        if (c == '\n') break;
        if (c != '\r') out += c;
    }
    return true;
}

// ---------------------------------------------------------------------------
// connect_to — resolve host and connect
// ---------------------------------------------------------------------------
static SOCKET connect_to(const std::string& host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0) {
        std::cerr << "Error: cannot resolve '" << host << "'\n";
        return INVALID_SOCKET;
    }

    SOCKET fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == INVALID_SOCKET) {
        freeaddrinfo(res);
        std::cerr << "Error: socket() failed\n";
        return INVALID_SOCKET;
    }

    if (connect(fd, res->ai_addr, static_cast<int>(res->ai_addrlen)) == SOCKET_ERROR) {
        freeaddrinfo(res);
        closesocket(fd);
        std::cerr << "Error: cannot connect to " << host << ":" << port
                  << " (is the server running?)\n";
        return INVALID_SOCKET;
    }

    freeaddrinfo(res);
    return fd;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 7379;

    if (argc >= 2) host = argv[1];
    if (argc >= 3) {
        try {
            int p = std::stoi(argv[2]);
            if (p < 1 || p > 65535) throw std::out_of_range("range");
            port = static_cast<std::uint16_t>(p);
        } catch (...) {
            std::cerr << "Invalid port: " << argv[2] << "\n";
            return 1;
        }
    }

    WinsockGuard wsa;
    if (!wsa.ok) {
        std::cerr << "Error: WSAStartup failed\n";
        return 1;
    }

    Socket sock(connect_to(host, port));
    if (!sock.valid()) return 1;

    std::cout << "Connected to " << host << ":" << port << "\n";
    std::cout << "Type commands (SET/GET/DEL/EXISTS/PING) or 'quit' to exit.\n\n";

    std::string input;
    while (true) {
        std::cout << "pulsekv> ";
        if (!std::getline(std::cin, input)) break;  // EOF (Ctrl+Z / Ctrl+D)

        // Trim trailing whitespace
        while (!input.empty() && (input.back() == ' ' || input.back() == '\r'))
            input.pop_back();

        if (input.empty()) continue;

        // Local quit command — don't send to server
        if (input == "quit" || input == "exit" || input == "q") {
            std::cout << "Bye.\n";
            break;
        }

        if (!send_line(sock.fd, input)) {
            std::cerr << "Error: send failed (server disconnected?)\n";
            break;
        }

        std::string response;
        if (!recv_line(sock.fd, response)) {
            std::cerr << "Error: recv failed (server disconnected?)\n";
            break;
        }

        std::cout << response << "\n";
    }

    return 0;
}
