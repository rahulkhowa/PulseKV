// benchmark/main.cpp
//
// PulseKV dedicated benchmarking tool — Phase 5, 9, 14, 15
//
// Usage:
//   pulsekv-benchmark [options]
//
// Options:
//   --host <ip>          Server host (default: 127.0.0.1)
//   --port <num>         Server port (default: 7379)
//   --workload <type>    get | set | mixed80 | mixed50 | mixed90 | mixed10 (default: mixed80)
//   --requests <num>     Number of requests per client (default: 20000)
//   --clients <num>      Concurrent client threads (default: 1)
//   --valsize <bytes>    Value size in bytes (default: 64)
//   --keyspace <num>     Key range size (default: 10000)
//   --dist <type>        Key distribution: uniform | zipfian (default: uniform)
//   --warmup <num>       Warmup requests per client (default: 0)
//   --no-prepopulate     Do not prepopulate keys before benchmark
//   --json               Output summary in JSON format

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

struct BenchmarkConfig {
    std::string   host        = "127.0.0.1";
    std::uint16_t port        = 7379;
    std::string   workload    = "mixed80";  // get, set, mixed80, mixed50, mixed90, mixed10
    std::size_t   requests    = 20000;
    std::size_t   clients     = 1;
    std::size_t   val_size    = 64;
    std::size_t   key_space   = 10000;
    std::string   dist        = "uniform";  // uniform, zipfian
    std::size_t   warmup      = 0;
    bool          prepopulate = true;
    bool          json_output = false;
};

struct WinsockGuard {
    bool ok = false;
    WinsockGuard() {
        WSADATA d{};
        ok = (WSAStartup(MAKEWORD(2, 2), &d) == 0);
    }
    ~WinsockGuard() { if (ok) WSACleanup(); }
};

struct SocketConn {
    SOCKET fd = INVALID_SOCKET;

    static SocketConn connect_to(const std::string& host, std::uint16_t port) {
        SocketConn c;
        c.fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (c.fd == INVALID_SOCKET) return c;

        int nodelay = 1;
        setsockopt(c.fd, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        if (connect(c.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            closesocket(c.fd);
            c.fd = INVALID_SOCKET;
        }
        return c;
    }

    ~SocketConn() {
        if (fd != INVALID_SOCKET) closesocket(fd);
    }

    SocketConn() = default;
    SocketConn(SocketConn&& o) noexcept : fd(o.fd) { o.fd = INVALID_SOCKET; }
    SocketConn& operator=(SocketConn&& o) noexcept {
        if (this != &o) {
            if (fd != INVALID_SOCKET) closesocket(fd);
            fd = o.fd;
            o.fd = INVALID_SOCKET;
        }
        return *this;
    }

    SocketConn(const SocketConn&) = delete;
    SocketConn& operator=(const SocketConn&) = delete;

    bool valid() const { return fd != INVALID_SOCKET; }

    bool send_all(const std::string& s) const {
        const char* p = s.data();
        int rem = static_cast<int>(s.size());
        while (rem > 0) {
            int n = send(fd, p, rem, 0);
            if (n <= 0) return false;
            p += n;
            rem -= n;
        }
        return true;
    }

    bool recv_line(std::string& out) const {
        out.clear();
        char c = 0;
        while (true) {
            int n = recv(fd, &c, 1, 0);
            if (n <= 0) return false;
            if (c == '\n') break;
            if (c != '\r') out += c;
        }
        return true;
    }
};

// Zipfian distribution generator (skew parameter s = 0.99)
class ZipfianGenerator {
public:
    explicit ZipfianGenerator(std::size_t n, double s = 0.99)
        : n_(n), s_(s), dist_(0.0, 1.0) {
        if (n_ == 0) n_ = 1;
        cdf_.resize(n_);
        double sum = 0.0;
        for (std::size_t i = 1; i <= n_; ++i) {
            sum += 1.0 / std::pow(static_cast<double>(i), s_);
        }
        double running = 0.0;
        for (std::size_t i = 1; i <= n_; ++i) {
            running += (1.0 / std::pow(static_cast<double>(i), s_)) / sum;
            cdf_[i - 1] = running;
        }
    }

    template <typename RNG>
    std::size_t next(RNG& rng) {
        double p = dist_(rng);
        auto it = std::lower_bound(cdf_.begin(), cdf_.end(), p);
        if (it == cdf_.end()) return n_ - 1;
        return static_cast<std::size_t>(std::distance(cdf_.begin(), it));
    }

private:
    std::size_t n_;
    double s_;
    std::vector<double> cdf_;
    std::uniform_real_distribution<double> dist_;
};

struct ClientStats {
    std::vector<double> latencies_us;
    std::size_t errors = 0;
};

std::size_t get_memory_usage_bytes() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize;
    }
    return 0;
}

void print_help() {
    std::cout << "PulseKV Benchmark Tool\n\n"
              << "Options:\n"
              << "  --host <ip>          Server host (default: 127.0.0.1)\n"
              << "  --port <num>         Server port (default: 7379)\n"
              << "  --workload <type>    get | set | mixed80 | mixed50 | mixed90 | mixed10 (default: mixed80)\n"
              << "  --requests <num>     Number of requests per client (default: 20000)\n"
              << "  --clients <num>      Concurrent client count (default: 1)\n"
              << "  --valsize <bytes>    Value size in bytes (default: 64)\n"
              << "  --keyspace <num>     Key range size (default: 10000)\n"
              << "  --dist <type>        Key distribution: uniform | zipfian (default: uniform)\n"
              << "  --warmup <num>       Warmup requests per client (default: 0)\n"
              << "  --no-prepopulate     Do not prepopulate keys before benchmark\n"
              << "  --json               Output summary in JSON\n";
}

BenchmarkConfig parse_args(int argc, char* argv[]) {
    BenchmarkConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            cfg.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            cfg.port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--workload" && i + 1 < argc) {
            cfg.workload = argv[++i];
        } else if (arg == "--requests" && i + 1 < argc) {
            cfg.requests = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--clients" && i + 1 < argc) {
            cfg.clients = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--valsize" && i + 1 < argc) {
            cfg.val_size = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--keyspace" && i + 1 < argc) {
            cfg.key_space = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--dist" && i + 1 < argc) {
            cfg.dist = argv[++i];
        } else if (arg == "--warmup" && i + 1 < argc) {
            cfg.warmup = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--no-prepopulate") {
            cfg.prepopulate = false;
        } else if (arg == "--json") {
            cfg.json_output = true;
        } else if (arg == "--help" || arg == "-h") {
            print_help();
            std::exit(0);
        }
    }
    return cfg;
}

int get_read_percentage(const std::string& workload) {
    if (workload == "get")     return 100;
    if (workload == "set")     return 0;
    if (workload == "mixed90") return 90;
    if (workload == "mixed80") return 80;
    if (workload == "mixed50") return 50;
    if (workload == "mixed10") return 10;
    return 80;
}

} // namespace

int main(int argc, char* argv[]) {
    BenchmarkConfig cfg = parse_args(argc, argv);
    WinsockGuard wsa;
    if (!wsa.ok) {
        std::cerr << "Error: WSAStartup failed\n";
        return 1;
    }

    if (!cfg.json_output) {
        std::cout << "=========================================================\n";
        std::cout << " PulseKV Benchmark Harness (Phase 14)\n";
        std::cout << "=========================================================\n";
        std::cout << "Target:        " << cfg.host << ":" << cfg.port << "\n";
        std::cout << "Workload:      " << cfg.workload << " (" << get_read_percentage(cfg.workload) << "% GET)\n";
        std::cout << "Clients:       " << cfg.clients << "\n";
        std::cout << "Requests/c:    " << cfg.requests << "\n";
        std::cout << "Warmup reqs/c: " << cfg.warmup << "\n";
        std::cout << "Total reqs:    " << (cfg.clients * cfg.requests) << "\n";
        std::cout << "Distribution:  " << cfg.dist << "\n";
        std::cout << "Value size:    " << cfg.val_size << " bytes\n";
        std::cout << "Key space:     " << cfg.key_space << " keys\n";
        std::cout << "---------------------------------------------------------\n";
    }

    std::string sample_val(cfg.val_size, 'x');

    // Prepopulate keys if requested and workload reads keys
    if (cfg.prepopulate && (cfg.workload != "set")) {
        std::size_t count = std::min<std::size_t>(cfg.key_space, 5000);
        if (!cfg.json_output) {
            std::cout << "[Pre-populating keyspace with " << count << " keys...]\n";
        }
        auto prepop_conn = SocketConn::connect_to(cfg.host, cfg.port);
        if (!prepop_conn.valid()) {
            std::cerr << "Error: cannot connect to server for prepopulation.\n";
            return 1;
        }
        std::string resp;
        for (std::size_t i = 0; i < count; ++i) {
            std::string cmd = "SET key:" + std::to_string(i) + " " + sample_val + "\r\n";
            if (!prepop_conn.send_all(cmd) || !prepop_conn.recv_line(resp)) {
                std::cerr << "Error during prepopulation at key " << i << "\n";
                break;
            }
        }
        if (!cfg.json_output) {
            std::cout << "[Pre-population complete]\n";
        }
    }

    std::shared_ptr<ZipfianGenerator> zipf_gen;
    if (cfg.dist == "zipfian") {
        zipf_gen = std::make_shared<ZipfianGenerator>(cfg.key_space, 0.99);
    }

    int read_pct = get_read_percentage(cfg.workload);

    std::vector<ClientStats> all_stats(cfg.clients);
    std::vector<std::thread> threads;
    threads.reserve(cfg.clients);

    std::size_t mem_before = get_memory_usage_bytes();
    auto start_time = std::chrono::steady_clock::now();

    for (std::size_t c = 0; c < cfg.clients; ++c) {
        threads.emplace_back([&, c]() {
            ClientStats& stats = all_stats[c];
            stats.latencies_us.reserve(cfg.requests);

            auto conn = SocketConn::connect_to(cfg.host, cfg.port);
            if (!conn.valid()) {
                stats.errors = cfg.requests + cfg.warmup;
                return;
            }

            std::mt19937_64 rng(1337 + c);
            std::uniform_int_distribution<std::size_t> uniform_dist(0, cfg.key_space - 1);
            std::uniform_int_distribution<int> pct_dist(1, 100);

            auto get_key_idx = [&]() -> std::size_t {
                if (zipf_gen) {
                    return zipf_gen->next(rng);
                }
                return uniform_dist(rng);
            };

            std::string response;

            // Warmup phase (not timed)
            for (std::size_t w = 0; w < cfg.warmup; ++w) {
                std::size_t k = get_key_idx();
                std::string cmd;
                if (pct_dist(rng) <= read_pct) {
                    cmd = "GET key:" + std::to_string(k) + "\r\n";
                } else {
                    cmd = "SET key:" + std::to_string(k) + " " + sample_val + "\r\n";
                }
                if (!conn.send_all(cmd) || !conn.recv_line(response)) {
                    ++stats.errors;
                }
            }

            // Measurement phase
            for (std::size_t r = 0; r < cfg.requests; ++r) {
                std::size_t k = get_key_idx();
                std::string cmd;

                if (pct_dist(rng) <= read_pct) {
                    cmd = "GET key:" + std::to_string(k) + "\r\n";
                } else {
                    cmd = "SET key:" + std::to_string(k) + " " + sample_val + "\r\n";
                }

                auto req_start = std::chrono::high_resolution_clock::now();
                if (!conn.send_all(cmd) || !conn.recv_line(response)) {
                    ++stats.errors;
                    continue;
                }
                auto req_end = std::chrono::high_resolution_clock::now();

                double duration_us = std::chrono::duration<double, std::micro>(req_end - req_start).count();
                stats.latencies_us.push_back(duration_us);
            }
        });
    }

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    auto end_time = std::chrono::steady_clock::now();
    double total_seconds = std::chrono::duration<double>(end_time - start_time).count();
    std::size_t mem_after = get_memory_usage_bytes();

    std::vector<double> all_latencies;
    std::size_t total_errors = 0;
    for (const auto& s : all_stats) {
        all_latencies.insert(all_latencies.end(), s.latencies_us.begin(), s.latencies_us.end());
        total_errors += s.errors;
    }

    std::size_t total_completed = all_latencies.size();
    std::size_t total_scheduled = cfg.clients * cfg.requests;
    double throughput = (total_seconds > 0) ? (static_cast<double>(total_completed) / total_seconds) : 0.0;

    std::sort(all_latencies.begin(), all_latencies.end());

    auto percentile = [&](double p) -> double {
        if (all_latencies.empty()) return 0.0;
        std::size_t idx = static_cast<std::size_t>(p / 100.0 * static_cast<double>(all_latencies.size() - 1));
        return all_latencies[idx];
    };

    double min_lat = all_latencies.empty() ? 0.0 : all_latencies.front();
    double max_lat = all_latencies.empty() ? 0.0 : all_latencies.back();
    double sum_lat = std::accumulate(all_latencies.begin(), all_latencies.end(), 0.0);
    double avg_lat = all_latencies.empty() ? 0.0 : (sum_lat / static_cast<double>(all_latencies.size()));

    double p50  = percentile(50.0);
    double p95  = percentile(95.0);
    double p99  = percentile(99.0);
    double p999 = percentile(99.9);

    if (cfg.json_output) {
        std::cout << "{\n"
                  << "  \"workload\": \"" << cfg.workload << "\",\n"
                  << "  \"clients\": " << cfg.clients << ",\n"
                  << "  \"requests_per_client\": " << cfg.requests << ",\n"
                  << "  \"total_scheduled\": " << total_scheduled << ",\n"
                  << "  \"total_completed\": " << total_completed << ",\n"
                  << "  \"errors\": " << total_errors << ",\n"
                  << "  \"duration_sec\": " << total_seconds << ",\n"
                  << "  \"throughput_qps\": " << throughput << ",\n"
                  << "  \"memory_mb_start\": " << (static_cast<double>(mem_before) / (1024.0 * 1024.0)) << ",\n"
                  << "  \"memory_mb_end\": " << (static_cast<double>(mem_after) / (1024.0 * 1024.0)) << ",\n"
                  << "  \"latency_us\": {\n"
                  << "    \"avg\": " << avg_lat << ",\n"
                  << "    \"min\": " << min_lat << ",\n"
                  << "    \"p50\": " << p50 << ",\n"
                  << "    \"p95\": " << p95 << ",\n"
                  << "    \"p99\": " << p99 << ",\n"
                  << "    \"p999\": " << p999 << ",\n"
                  << "    \"max\": " << max_lat << "\n"
                  << "  }\n"
                  << "}\n";
    } else {
        std::cout << "\nBenchmark Results\n";
        std::cout << "=================\n\n";
        std::cout << "Workload: " << cfg.workload << "\n";
        std::cout << "Clients:  " << cfg.clients << "\n";
        std::cout << "Requests: " << total_completed << " / " << total_scheduled << "\n\n";
        std::cout << "Throughput:\n";
        std::cout << std::fixed << std::setprecision(0) << throughput << " requests/sec\n\n";
        std::cout << "Latency:\n";
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "p50:   " << (p50 / 1000.0)  << " ms  (" << (p50) << " us)\n";
        std::cout << "p95:   " << (p95 / 1000.0)  << " ms  (" << (p95) << " us)\n";
        std::cout << "p99:   " << (p99 / 1000.0)  << " ms  (" << (p99) << " us)\n";
        std::cout << "p99.9: " << (p999 / 1000.0) << " ms  (" << (p999) << " us)\n";
        std::cout << "max:   " << (max_lat / 1000.0) << " ms  (" << (max_lat) << " us)\n\n";
        std::cout << "Memory:\n";
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Start: " << (static_cast<double>(mem_before) / (1024.0 * 1024.0)) << " MB\n";
        std::cout << "End:   " << (static_cast<double>(mem_after) / (1024.0 * 1024.0)) << " MB\n\n";
        std::cout << "Errors:\n" << total_errors << "\n";
        std::cout << "=================\n";
    }

    return (total_errors > 0) ? 1 : 0;
}
