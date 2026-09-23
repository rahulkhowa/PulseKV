#include "pulsekv/server/server.hpp"
#include "pulsekv/storage/store.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

struct WinsockInit {
    bool ok = false;
    WinsockInit() {
        WSADATA d{};
        ok = (WSAStartup(MAKEWORD(2, 2), &d) == 0);
    }
    ~WinsockInit() {
        if (ok) WSACleanup();
    }
};

SOCKET connect_client(std::uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

bool send_line(SOCKET s, const std::string& line) {
    std::string msg = line + "\r\n";
    const char* ptr = msg.data();
    int rem = static_cast<int>(msg.size());
    while (rem > 0) {
        int n = send(s, ptr, rem, 0);
        if (n == SOCKET_ERROR) return false;
        ptr += n;
        rem -= n;
    }
    return true;
}

std::string recv_line(SOCKET s) {
    std::string out;
    char c = 0;
    while (true) {
        int n = recv(s, &c, 1, 0);
        if (n <= 0) break;
        if (c == '\n') break;
        if (c != '\r') out += c;
    }
    return out;
}

} // namespace

int main() {
    WinsockInit ws;
    assert(ws.ok && "WSAStartup must succeed");

    std::cout << "PulseKV — Phase 3 & 4 Server/Client Integration Tests\n";
    std::cout << "====================================================\n\n";

    constexpr std::uint16_t TEST_PORT = 17379;
    pulsekv::Store store;
    pulsekv::Server server(TEST_PORT, store);

    // Run server on a background thread
    std::thread server_thread([&server]() {
        server.run();
    });

    // Wait for server to begin listening
    SOCKET client = INVALID_SOCKET;
    for (int retry = 0; retry < 50; ++retry) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        client = connect_client(TEST_PORT);
        if (client != INVALID_SOCKET) break;
    }
    assert(client != INVALID_SOCKET && "Failed to connect to server");

    int passed = 0;

    auto test = [&](const std::string& desc, auto fn) {
        std::cout << "  [TEST] " << desc << "... ";
        fn();
        std::cout << "PASSED\n";
        ++passed;
    };

    // 1. PING
    test("PING command", [&]() {
        assert(send_line(client, "PING"));
        assert(recv_line(client) == "+PONG");
    });

    // 2. SET and GET
    test("SET and GET", [&]() {
        assert(send_line(client, "SET name Rahul"));
        assert(recv_line(client) == "+OK");

        assert(send_line(client, "GET name"));
        assert(recv_line(client) == "+Rahul");
    });

    // 3. EXISTS
    test("EXISTS for existing key", [&]() {
        assert(send_line(client, "EXISTS name"));
        assert(recv_line(client) == ":1");
    });

    // 4. DEL
    test("DEL removes existing key", [&]() {
        assert(send_line(client, "DEL name"));
        assert(recv_line(client) == ":1");
    });

    // 5. GET after DEL
    test("GET returns null bulk for deleted key", [&]() {
        assert(send_line(client, "GET name"));
        assert(recv_line(client) == "$-1");
    });

    // 6. EXISTS after DEL
    test("EXISTS returns 0 for deleted key", [&]() {
        assert(send_line(client, "EXISTS name"));
        assert(recv_line(client) == ":0");
    });

    // 7. DEL non-existent key
    test("DEL returns 0 for non-existent key", [&]() {
        assert(send_line(client, "DEL non_existent_key"));
        assert(recv_line(client) == ":0");
    });

    // 8. Unknown command
    test("Unknown command error response", [&]() {
        assert(send_line(client, "UNKNOWN cmd"));
        std::string res = recv_line(client);
        assert(res.rfind("-ERR unknown command", 0) == 0);
    });

    // 9. Wrong argument count
    test("Wrong argument count error response", [&]() {
        assert(send_line(client, "SET onlykey"));
        std::string res = recv_line(client);
        assert(res.rfind("-ERR wrong number of arguments", 0) == 0);
    });

    // 10. Blank line (should be ignored silently without dropping connection)
    test("Blank line handling", [&]() {
        assert(send_line(client, ""));
        assert(send_line(client, "PING"));
        assert(recv_line(client) == "+PONG");
    });

    // 11. Pipelining multiple commands
    test("Command pipelining in single TCP send", [&]() {
        std::string batch = "SET color blue\r\nGET color\r\n";
        int sent = send(client, batch.data(), static_cast<int>(batch.size()), 0);
        assert(sent == static_cast<int>(batch.size()));
        assert(recv_line(client) == "+OK");
        assert(recv_line(client) == "+blue");
    });

    // 12. Multiple sequential clients against same store
    test("Second client reconnect and shared store verification", [&]() {
        closesocket(client);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        SOCKET client2 = connect_client(TEST_PORT);
        assert(client2 != INVALID_SOCKET);

        assert(send_line(client2, "GET color"));
        assert(recv_line(client2) == "+blue");

        closesocket(client2);
    });

    // 13. Multiple simultaneous concurrent clients (Phase 6)
    test("Multiple concurrent clients simultaneous execution", [&]() {
        constexpr int NUM_CLIENTS = 10;
        constexpr int OPS_PER_CLIENT = 50;
        std::vector<std::thread> workers;
        std::atomic<int> success_count = 0;

        for (int i = 0; i < NUM_CLIENTS; ++i) {
            workers.emplace_back([i, &success_count]() {
                SOCKET s = connect_client(TEST_PORT);
                if (s == INVALID_SOCKET) return;

                std::string key = "concurrent_key_" + std::to_string(i);
                std::string val = "val_" + std::to_string(i);

                for (int op = 0; op < OPS_PER_CLIENT; ++op) {
                    if (!send_line(s, "SET " + key + " " + val)) break;
                    if (recv_line(s) != "+OK") break;

                    if (!send_line(s, "GET " + key)) break;
                    if (recv_line(s) != "+" + val) break;
                }

                closesocket(s);
                success_count.fetch_add(1);
            });
        }

        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        assert(success_count == NUM_CLIENTS);
    });

    // 14. Graceful shutdown
    test("Server graceful stop", [&]() {
        server.request_stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
        assert(!server.is_running());
    });

    std::cout << "\nResults: " << passed << " / " << passed << " passed\n";
    std::cout << "ALL INTEGRATION TESTS PASSED\n";
    return 0;
}
