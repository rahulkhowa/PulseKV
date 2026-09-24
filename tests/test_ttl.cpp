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

#define TEST_CHECK(expr)                                                      \
    do {                                                                      \
        if (!(expr)) {                                                        \
            std::cerr << "\n[FAIL] Assertion failed: " #expr " at line "      \
                      << __LINE__ << std::endl;                               \
            std::exit(1);                                                     \
        }                                                                     \
    } while (false)

struct WinsockGuard {
    bool ok = false;
    WinsockGuard() {
        WSADATA d{};
        ok = (WSAStartup(MAKEWORD(2, 2), &d) == 0);
    }
    ~WinsockGuard() { if (ok) WSACleanup(); }
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

void test_ttl_immediate_get() {
    pulsekv::Store s;
    s.set("session", "token123", 5);
    auto val = s.get("session");
    TEST_CHECK(val.has_value() && val.value() == "token123");
    TEST_CHECK(s.exists("session"));
}

void test_ttl_expiration_after_wait() {
    pulsekv::Store s;
    // Set 80ms TTL via set_with_expiry
    s.set_with_expiry("short_lived", "data",
                      std::chrono::steady_clock::now() + std::chrono::milliseconds(80));

    TEST_CHECK(s.get("short_lived") == "data");

    // Wait past expiration
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    // Lazy expiration on get
    TEST_CHECK(s.get("short_lived") == std::nullopt);
    TEST_CHECK(!s.exists("short_lived"));
}

void test_ttl_persistent_does_not_expire() {
    pulsekv::Store s;
    s.set("permanent", "value", 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    TEST_CHECK(s.get("permanent") == "value");
    TEST_CHECK(s.exists("permanent"));
}

void test_ttl_overwrite_removes_or_updates_expiry() {
    pulsekv::Store s;
    // Set with short TTL
    s.set_with_expiry("key", "val1",
                      std::chrono::steady_clock::now() + std::chrono::milliseconds(80));

    // Overwrite without TTL -> should become persistent
    s.set("key", "val2", 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    TEST_CHECK(s.get("key") == "val2");

    // Overwrite again with new TTL
    s.set_with_expiry("key", "val3",
                      std::chrono::steady_clock::now() + std::chrono::milliseconds(80));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    TEST_CHECK(s.get("key") == std::nullopt);
}

void test_active_background_purge() {
    pulsekv::Store s(pulsekv::LockStrategy::SHARED_MUTEX, true);
    // Insert keys with 50ms TTL
    for (int i = 0; i < 50; ++i) {
        s.set_with_expiry("exp_" + std::to_string(i), "v",
                          std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
    }
    s.set("persistent", "stay", 0);

    // Wait for background worker to purge (runs every 100ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    // Without calling get on any exp_ keys, size() should reflect background purges
    TEST_CHECK(s.size() == 1);
    TEST_CHECK(s.exists("persistent"));
}

void test_ttl_tcp_integration() {
    constexpr std::uint16_t PORT = 17386;
    pulsekv::Store store;
    pulsekv::Server server(PORT, store, 4);

    std::thread server_th([&server]() { server.run(); });

    SOCKET client = connect_client(PORT, 50);
    TEST_CHECK(client != INVALID_SOCKET);

    // Send SET key value EX 1
    TEST_CHECK(send_line(client, "SET token xyz123 EX 1"));
    TEST_CHECK(recv_line(client) == "+OK");

    // Immediate GET
    TEST_CHECK(send_line(client, "GET token"));
    TEST_CHECK(recv_line(client) == "+xyz123");

    // Wait 1.1s for expiration
    std::this_thread::sleep_for(std::chrono::milliseconds(1150));

    // GET after expiration
    TEST_CHECK(send_line(client, "GET token"));
    TEST_CHECK(recv_line(client) == "$-1");

    // EXISTS after expiration
    TEST_CHECK(send_line(client, "EXISTS token"));
    TEST_CHECK(recv_line(client) == ":0");

    closesocket(client);
    server.request_stop();
    if (server_th.joinable()) server_th.join();
}

} // namespace

int main() {
    WinsockGuard wsa;
    std::cout << "PulseKV — Phase 10 TTL Tests\n";
    std::cout << "============================\n\n";

    int passed = 0;
    auto run = [&](const std::string& name, auto fn) {
        std::cout << "  [TEST] " << name << "... " << std::flush;
        fn();
        std::cout << "PASSED\n" << std::flush;
        ++passed;
    };

    run("Immediate GET with TTL", test_ttl_immediate_get);
    run("Expiration after wait (lazy eviction)", test_ttl_expiration_after_wait);
    run("Persistent keys do not expire", test_ttl_persistent_does_not_expire);
    run("Overwrite updates/clears TTL", test_ttl_overwrite_removes_or_updates_expiry);
    run("Active background purge thread", test_active_background_purge);
    run("TCP Server SET key val EX n integration", test_ttl_tcp_integration);

    std::cout << "\nResults: " << passed << " / " << passed << " passed\n";
    std::cout << "ALL TTL TESTS PASSED\n";
    return 0;
}
