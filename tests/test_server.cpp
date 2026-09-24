#include "pulsekv/server/server.hpp"
#include "pulsekv/storage/store.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

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

SOCKET connect_client(std::uint16_t port, int retries = 20) {
    for (int r = 0; r < retries; ++r) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        int nodelay = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SOCKET_ERROR) {
            return s;
        }
        closesocket(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return INVALID_SOCKET;
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

#define TEST_CHECK(expr)                                                      \
    do {                                                                      \
        if (!(expr)) {                                                        \
            std::cerr << "\n[FAIL] Assertion failed: " #expr " at line "      \
                      << __LINE__ << std::endl;                               \
            std::exit(1);                                                     \
        }                                                                     \
    } while (false)

} // namespace

int main() {
    WinsockInit ws;
    TEST_CHECK(ws.ok && "WSAStartup must succeed");

    std::cout << "PulseKV — Phase 3 & 4 Server/Client Integration Tests\n";
    std::cout << "====================================================\n\n";

    constexpr std::uint16_t TEST_PORT = 17379;
    pulsekv::Store store;
    pulsekv::Server server(TEST_PORT, store, 16);

    // Run server on a background thread
    std::thread server_thread([&server]() {
        server.run();
    });

    // Wait for server to begin listening
    SOCKET client = connect_client(TEST_PORT, 50);
    TEST_CHECK(client != INVALID_SOCKET && "Failed to connect to server");

    int passed = 0;

    auto test = [&](const std::string& desc, auto fn) {
        std::cout << "  [TEST] " << desc << "... " << std::flush;
        fn();
        std::cout << "PASSED\n" << std::flush;
        ++passed;
    };

    // 1. PING
    test("PING command", [&]() {
        TEST_CHECK(send_line(client, "PING"));
        TEST_CHECK(recv_line(client) == "+PONG");
    });

    // 2. SET and GET
    test("SET and GET", [&]() {
        TEST_CHECK(send_line(client, "SET name Rahul"));
        TEST_CHECK(recv_line(client) == "+OK");

        TEST_CHECK(send_line(client, "GET name"));
        TEST_CHECK(recv_line(client) == "+Rahul");
    });

    // 3. EXISTS
    test("EXISTS for existing key", [&]() {
        TEST_CHECK(send_line(client, "EXISTS name"));
        TEST_CHECK(recv_line(client) == ":1");
    });

    // 4. DEL
    test("DEL removes existing key", [&]() {
        TEST_CHECK(send_line(client, "DEL name"));
        TEST_CHECK(recv_line(client) == ":1");
    });

    // 5. GET after DEL
    test("GET returns null bulk for deleted key", [&]() {
        TEST_CHECK(send_line(client, "GET name"));
        TEST_CHECK(recv_line(client) == "$-1");
    });

    // 6. EXISTS after DEL
    test("EXISTS returns 0 for deleted key", [&]() {
        TEST_CHECK(send_line(client, "EXISTS name"));
        TEST_CHECK(recv_line(client) == ":0");
    });

    // 7. DEL non-existent key
    test("DEL returns 0 for non-existent key", [&]() {
        TEST_CHECK(send_line(client, "DEL non_existent_key"));
        TEST_CHECK(recv_line(client) == ":0");
    });

    // 8. Unknown command
    test("Unknown command error response", [&]() {
        TEST_CHECK(send_line(client, "UNKNOWN cmd"));
        std::string res = recv_line(client);
        TEST_CHECK(res.starts_with("-ERR unknown command"));
    });

    // 9. Wrong argument count
    test("Wrong argument count error response", [&]() {
        TEST_CHECK(send_line(client, "SET onlykey"));
        std::string res = recv_line(client);
        TEST_CHECK(res.starts_with("-ERR wrong number of arguments"));
    });

    // 10. Blank line (should be ignored silently without dropping connection)
    test("Blank line handling", [&]() {
        TEST_CHECK(send_line(client, ""));
        TEST_CHECK(send_line(client, "PING"));
        TEST_CHECK(recv_line(client) == "+PONG");
    });

    // 11. Pipelining multiple commands
    test("Command pipelining in single TCP send", [&]() {
        std::string batch = "SET color blue\r\nGET color\r\n";
        int sent = send(client, batch.data(), static_cast<int>(batch.size()), 0);
        TEST_CHECK(sent == static_cast<int>(batch.size()));
        TEST_CHECK(recv_line(client) == "+OK");
        TEST_CHECK(recv_line(client) == "+blue");
    });

    // 12. Multiple sequential clients against same store
    test("Second client reconnect and shared store verification", [&]() {
        closesocket(client);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        SOCKET client2 = connect_client(TEST_PORT);
        TEST_CHECK(client2 != INVALID_SOCKET);

        TEST_CHECK(send_line(client2, "GET color"));
        TEST_CHECK(recv_line(client2) == "+blue");

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
                SOCKET s = connect_client(TEST_PORT, 20);
                if (s == INVALID_SOCKET) {
                    std::cerr << "[worker " << i << " failed to connect]\n";
                    return;
                }

                std::string key = "concurrent_key_" + std::to_string(i);
                std::string val = "val_" + std::to_string(i);

                bool all_ok = true;
                for (int op = 0; op < OPS_PER_CLIENT; ++op) {
                    if (!send_line(s, "SET " + key + " " + val)) { all_ok = false; break; }
                    if (recv_line(s) != "+OK") { all_ok = false; break; }

                    if (!send_line(s, "GET " + key)) { all_ok = false; break; }
                    if (recv_line(s) != "+" + val) { all_ok = false; break; }
                }

                closesocket(s);
                if (all_ok) {
                    success_count.fetch_add(1);
                }
            });
        }

        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        TEST_CHECK(success_count == NUM_CLIENTS);
    });

    // 14. Graceful shutdown
    test("Server graceful stop", [&]() {
        server.request_stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
        TEST_CHECK(!server.is_running());
    });

    std::cout << "\nResults: " << passed << " / " << passed << " passed\n";
    std::cout << "ALL INTEGRATION TESTS PASSED\n";
    return 0;
}
