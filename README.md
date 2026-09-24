# PulseKV

> A high-performance, concurrent, in-memory key-value store and benchmarking laboratory built from scratch in modern C++20.

PulseKV was built to explore and benchmark the fundamental systems engineering trade-offs in networking (Winsock TCP), multi-threading concurrency models, thread-safe hash tables, LRU memory eviction, and Write-Ahead Log (WAL) durability.

---

## Features

- **In-Memory Storage Engine:** Hash-table storage backed by `std::unordered_map` with expected $O(1)$ read/write complexity.
- **Line-Oriented Text Protocol:** RESP-inspired human-readable and pipelinable text wire protocol.
- **Bounded Thread Pool Architecture:** Multi-threaded TCP server with worker threads, thread-safe task queue, condition variables, and graceful shutdown.
- **Configurable Locking Strategies:** Supports both exclusive `std::mutex` and readers-writer `std::shared_mutex` for high-throughput concurrent reads.
- **TTL Key Expiration:** Lazy expiration on access combined with active background thread eviction.
- **O(1) LRU Eviction Policy:** Combined hash map and doubly-linked list (`std::list`) providing $O(1)$ capacity-bounded eviction.
- **Write-Ahead Log (WAL) & Crash Recovery:** Append-only log for state mutations (`SET`, `DEL`, TTL) with crash-resilience and startup log replay reconstruction.
- **Benchmarking Laboratory:** Dedicated multi-threaded benchmarking harness (`pulsekv-benchmark`) recording throughput (QPS), latency percentiles (p50, p95, p99, p99.9), memory footprint, and CPU metrics across configurable workloads.

---

## Architecture

```
                       [ Client Connections ]
                                 │
                                 ▼
                     ┌───────────────────────┐
                     │   PulseKV TCP Server  │
                     │     (Winsock2 TCP)    │
                     └───────────┬───────────┘
                                 │ Accept
                                 ▼
                     ┌───────────────────────┐
                     │   Bounded Task Queue  │
                     └───────────┬───────────┘
                                 │
         ┌───────────────────────┼───────────────────────┐
         ▼                       ▼                       ▼
   ┌───────────┐           ┌───────────┐           ┌───────────┐
   │ Worker 0  │           │ Worker 1  │           │ Worker N  │
   └─────┬─────┘           └─────┬─────┘           └─────┬─────┘
         │                       │                       │
         └───────────────────────┼───────────────────────┘
                                 │
                 ┌───────────────┴───────────────┐
                 │                               │
                 ▼                               ▼
     ┌───────────────────────┐       ┌───────────────────────┐
     │      PulseKV Store    │       │    Write-Ahead Log    │
     │ ┌───────────────────┐ │  Log  │       (wal.log)       │
     │ │ std::shared_mutex │ │──────>│   Append-Only Log     │
     │ ├───────────────────┤ │       └───────────────────────┘
     │ │std::unordered_map │ │
     │ ├───────────────────┤ │
     │ │ std::list (LRU)   │ │
     │ ├───────────────────┤ │
     │ │ TTL Background    │ │
     │ └───────────────────┘ │
     └───────────────────────┘
```

---

## Wire Protocol Commands

| Command | Arguments | Response | Description |
|---------|-----------|----------|-------------|
| `PING` | *none* | `+PONG\r\n` | Health check / connection keep-alive |
| `SET` | `key value [EX seconds]` | `+OK\r\n` | Store key-value pair with optional TTL |
| `GET` | `key` | `+<value>\r\n` or `$-1\r\n` | Retrieve value for key (`$-1` if missing/expired) |
| `DEL` | `key` | `:1\r\n` or `:0\r\n` | Delete key (`1` deleted, `0` not found) |
| `EXISTS` | `key` | `:1\r\n` or `:0\r\n` | Check key existence (`1` exists, `0` not found) |
| `EXPIRE` | `key seconds` | `:1\r\n` or `:0\r\n` | Apply TTL to existing key |
| `TTL` | `key` | `:<seconds>\r\n` | Remaining TTL in seconds (`-1` no expiry, `-2` missing) |

---

## Build & Run

### Prerequisites
- Windows 10/11
- GCC 16.x / Clang with C++20 support (MinGW-w64)
- CMake 3.20+

### Building
```powershell
# Configure Release build with MinGW Makefiles
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release

# Build all targets
cmake --build build --parallel
```

### Running the Server
```powershell
# Start PulseKV Server on default port 7379
./build/pulsekv-server.exe --port 7379 --threads 4

# Start with persistence (WAL enabled) and LRU capacity limit
./build/pulsekv-server.exe --port 7379 --threads 4 --wal wal.log --max-keys 100000
```

### Running the Interactive Client
```powershell
./build/pulsekv-client.exe --host 127.0.0.1 --port 7379
```

### Running Automated Test Suite
```powershell
ctest --test-dir build --output-on-failure
```

---

## Benchmarking Laboratory

PulseKV includes a high-throughput benchmarking tool designed to measure latency percentiles and QPS across varied concurrency levels and workloads:

```powershell
# Run benchmark with 50 concurrent clients, 100,000 requests, 80/20 mixed read/write
./build/pulsekv-benchmark.exe --clients 50 --requests 2000 --workload mixed80 --threads 4
```

### Supported Workloads
- `--workload get`: 100% GET operations
- `--workload set`: 100% SET operations
- `--workload mixed80`: 80% GET / 20% SET
- `--workload mixed50`: 50% GET / 50% SET

### Sample Benchmark Results
Full methodology, hardware context, and empirical comparison tables are documented in [BENCHMARKS.md](BENCHMARKS.md), [BOTTLENECK_ANALYSIS.md](BOTTLENECK_ANALYSIS.md), and [EXTERNAL_COMPARISON.md](EXTERNAL_COMPARISON.md).

---

## Project Documentation
- [Implementation Plan & Status](Implementation.md)
- [System Benchmarks & Results](BENCHMARKS.md)
- [Bottleneck & Profiling Analysis](BOTTLENECK_ANALYSIS.md)
- [Durability & WAL Guarantees](DURABILITY.md)
- [External Comparison vs Memcached](EXTERNAL_COMPARISON.md)
- [Sanitizers & Quality Verification](SANITIZERS.md)
- [Interview & Architecture Guide](INTERVIEW_GUIDE.md)

---

## License
MIT License
