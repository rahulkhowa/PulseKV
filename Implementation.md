# PulseKV — Implementation Plan

## Concurrent In-Memory Key-Value Store & Benchmarking Laboratory

---

# CURRENT PHASE STATUS

| Phase | Name                        | Status      |
|-------|-----------------------------|-------------|
| 0     | Design                      | ✅ Complete  |
| 1     | Basic Storage Engine        | ✅ Complete  |
| 2     | Command Parser              | ✅ Complete  |
| 3     | TCP Server                  | ✅ Complete  |
| 4     | Client                      | ✅ Complete  |
| 5     | First Baseline Benchmark    | ✅ Complete  |
| 6     | Concurrent Clients          | ✅ Complete  |
| 7     | Thread Pool                 | ✅ Complete  |
| 8     | Thread-Safe Storage         | ✅ Complete  |
| 9     | Concurrency Benchmark       | ✅ Complete  |
| 10    | TTL                         | ✅ Complete  |
| 11    | LRU Eviction                | ✅ Complete  |
| 12    | Write-Ahead Log (WAL)       | ✅ Complete  |
| 13    | Crash / Recovery Testing    | ✅ Complete  |
| 14    | Benchmark Harness           | ✅ Complete  |
| 15    | Fair Benchmarking           | ✅ Complete  |
| 16    | External Comparison         | ✅ Complete  |
| 17    | Bottleneck Analysis         | ✅ Complete  |
| 18    | Optional Sharding           | ⬜ Deferred  |
| 19    | Sanitizers                  | ⬜ Pending   |
| 20    | Final Test Suite            | ⬜ Pending   |

---

# PHASE 0 — DESIGN

## 0.1 Development Environment

### Platform

- **OS:** Windows 10/11 (Build 26200)
- **Target OS:** Windows only (initial scope — POSIX not required)
- **Networking API:** Winsock2 (ws2_32)
- **Shell:** PowerShell (build scripts) + CMD for tool invocation

### Compiler

- **Compiler:** GCC 16.x (WinLibs, MinGW-w64, POSIX threads, UCRT runtime)
- **Standard:** C++20
- **Key C++20 features used:**
  - std::jthread + std::stop_token for cooperative thread shutdown
  - Designated initializers
  - std::span
  - [[nodiscard]], [[likely]] / [[unlikely]] attributes
  - Concepts (where genuinely useful)
  - std::atomic_ref if needed
- **Build type:** Release (-O2) for benchmarks, Debug (-g -O0) for development
- **Build system:** CMake 4.x with MinGW Makefiles

### Third-Party Libraries

None - no Boost, no ASIO, no {fmt}, no spdlog.
Standard library only.

---

## 0.2 Repository Structure

```
PulseKV/
|
+-- CMakeLists.txt
+-- README.md
+-- Implementation.md         <- this file
+-- INTERVIEW_GUIDE.md
+-- BENCHMARKS.md
+-- .gitignore
|
+-- include/
|   +-- pulsekv/
|       +-- storage/          <- StorageEngine, LRU, TTL
|       +-- protocol/         <- Command parser, response builder
|       +-- server/           <- TCP server, connection handler
|       +-- concurrency/      <- ThreadPool
|       +-- persistence/      <- WAL, recovery
|
+-- src/
|   +-- storage/
|   +-- protocol/
|   +-- server/
|   +-- concurrency/
|   +-- persistence/
|
+-- tests/                    <- unit/integration tests
|
+-- benchmark/                <- pulsekv-benchmark tool
|
+-- client/                   <- pulsekv-client CLI
```

Directories are created only when the corresponding phase is implemented.
No empty skeleton directories.

---

## 0.3 Protocol Design

### Wire Format

A simple line-oriented text protocol (newline-delimited, no binary framing).

Requests (client to server):
```
<COMMAND> [arg1] [arg2] ...\r\n
```

Responses (server to client):
```
+OK\r\n                  <- success with no data
+<value>\r\n             <- success with string value
-ERR <message>\r\n       <- error
:0\r\n  or  :1\r\n       <- integer response (e.g. for EXISTS)
$-1\r\n                  <- null (key not found)
```

This is intentionally RESP-inspired but simplified. It is NOT full RESP.
The goal is readability and debuggability over a raw telnet session.

### Supported Commands (Phase 1-2)

| Command             | Arguments     | Response         | Description              |
|---------------------|---------------|------------------|--------------------------|
| SET key value       | key, value    | +OK              | Store key to value       |
| GET key             | key           | +value / $-1     | Retrieve value           |
| DEL key             | key           | :1 / :0          | Delete; 1=deleted 0=miss |
| EXISTS key          | key           | :1 / :0          | 1=exists, 0=not found    |
| SET key value EX n  | key,val,n     | +OK              | Set with TTL (Phase 10)  |
| PING                | none          | +PONG            | Liveness check           |

### Error Handling

All error responses follow: -ERR <reason>\r\n

Handled cases:
- Empty line: silently ignored (no response)
- Unknown command: -ERR unknown command 'CMD'\r\n
- Wrong arg count: -ERR wrong number of arguments for 'CMD'\r\n
- Value too large: -ERR value too large\r\n
- Server error: -ERR internal error\r\n

---

## 0.4 Storage API

The core storage engine exposes a clean C++ API.
The server calls this API; the storage engine knows nothing about networking.

```cpp
// include/pulsekv/storage/store.hpp

class Store {
public:
    void set(std::string key, std::string value);
    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);
    bool exists(const std::string& key);
};
```

Internal representation:

```cpp
struct Entry {
    std::string value;
    // Phase 10: std::optional<std::chrono::steady_clock::time_point> expiry;
};

std::unordered_map<std::string, Entry> table_;
```

Complexity guarantees (expected/average, not absolute):

| Operation | Expected | Worst case |
|-----------|----------|------------|
| SET       | O(1)     | O(n)       |
| GET       | O(1)     | O(n)       |
| DEL       | O(1)     | O(n)       |
| EXISTS    | O(1)     | O(n)       |

---

## 0.5 Concurrency Model

Phased approach - do not implement all at once:

| Phase | Model                      | Locking                                  |
|-------|----------------------------|------------------------------------------|
| 1-5   | Single-threaded            | None                                     |
| 6     | Thread-per-connection      | Global std::mutex                        |
| 7-8   | Thread pool                | Global std::mutex then std::shared_mutex |
| 18    | Sharded (if justified)     | Per-shard std::shared_mutex              |

Thread Pool Design (Phase 7):
```
Accept loop (main thread)
    |
    v
Task Queue  <-- std::queue<std::function<void()>>
    |            protected by std::mutex + std::condition_variable
    |
    +-- Worker 0
    +-- Worker 1
    +-- Worker 2
    +-- Worker N
```

Each worker executes one client connection's full lifecycle:
recv -> parse -> execute -> send -> close

Synchronization primitives:
- std::mutex: exclusive lock for write-heavy critical sections
- std::shared_mutex: readers-writer lock
- std::condition_variable: task queue notifications
- std::atomic<bool>: shutdown flag
- std::jthread: RAII thread with cooperative stop (C++20)

---

## 0.6 Ownership and Lifetime Rules

- Store is owned by the server, created at startup, destroyed at shutdown
- ThreadPool is owned by the server, shutdown before Store destruction
- Connection handlers are tasks dispatched to the thread pool; hold reference to Store only (no ownership)
- WAL file handle owned by WALWriter, passed to Store at construction
- No shared_ptr for hot path data - raw references with documented lifetimes
- RAII everywhere: sockets wrapped in RAII handles, WAL file using RAII file wrapper

---

## 0.7 Error Handling Strategy

- No exceptions for expected errors (key not found, expired TTL) - use std::optional / return codes
- Exceptions only for unrecoverable startup failures (bind failed, WAL open failed) - then terminate clearly
- Connection errors (client disconnected, recv=0): close connection cleanly, do not crash
- WAL errors: log + continue (best-effort durability, document limitation)
- Unknown errors: log + return -ERR internal error to client

---

## 0.8 Shutdown Behavior

Graceful shutdown sequence:
1. Ctrl+C -> SetConsoleCtrlHandler sets std::atomic<bool> running = false
2. Accept loop detects running == false, exits
3. ThreadPool destructor: sets stop=true, notifies all workers, joins all threads
4. Store destructor: no pending work, safe to destroy
5. WALWriter destructor: flush and close file
6. WSACleanup()

---

## 0.9 Testing Strategy

Phase 1-5: Manual assert-based unit tests in tests/ binary.

```cpp
// tests/test_store.cpp
void test_set_get() {
    Store s;
    s.set("k", "v");
    assert(s.get("k").value() == "v");
}
```

Phase 6+: Evaluate Catch2 single-header if the test suite grows unwieldy.

Test categories:
- Storage: SET, GET, DEL, EXISTS, overwrite, missing key
- Parser: valid commands, missing args, extra args, empty input, unknown cmd
- Server: single client round-trip, disconnect mid-session
- Concurrency: concurrent reads, concurrent writes, mixed (Phase 8+)
- TTL: set, wait, verify expiration (Phase 10)
- LRU: eviction ordering, capacity limits (Phase 11)
- WAL: write, restart, recovery, truncated record (Phase 12-13)

---

## 0.10 Benchmarking Methodology

Tool: pulsekv-benchmark - a dedicated multi-threaded C++ benchmark client (Phase 14).

Metrics collected:
- Throughput: requests/second (QPS)
- Latency percentiles: p50, p95, p99, p99.9
- Error count
- CPU usage (approximate via process timing)
- Memory usage (approximate via process size)

Workloads:
- --workload get     : 100% GET
- --workload set     : 100% SET
- --workload mixed80 : 80% GET / 20% SET
- --workload mixed50 : 50% GET / 50% SET

Concurrency levels: 1, 5, 10, 25, 50, 100, 250, 500

Environment recorded for every benchmark run:
- CPU: <model>
- RAM: <GB>
- OS:  Windows <build>
- Compiler: GCC <version> (WinLibs MinGW-w64 UCRT)
- Flags: -O2 -std=c++20
- Build: Release

Fairness rules:
- Same machine for all comparisons
- Same client count, request count, key/value sizes
- Warmup before measurement
- No invented numbers - all from actual execution

---

## 0.11 Phase-by-Phase Build Order

```
Phase 0  -> Design (this document)
Phase 1  -> Store (HashMap, SET/GET/DEL/EXISTS)
Phase 2  -> Parser (text protocol, error handling)
Phase 3  -> TCP Server (single-threaded, Winsock)
Phase 4  -> Client CLI (pulsekv-client)
Phase 5  -> Baseline Benchmark (single-thread numbers)
Phase 6  -> Concurrent clients (thread-per-connection)
Phase 7  -> Thread Pool
Phase 8  -> Thread-safe Storage (mutex -> shared_mutex)
Phase 9  -> Concurrency Benchmark (compare all three)
Phase 10 -> TTL (lazy + background cleanup)
Phase 11 -> LRU Eviction (unordered_map + doubly linked list)
Phase 12 -> WAL (append-only log for SET/DEL)
Phase 13 -> Crash / Recovery Testing
Phase 14 -> Benchmark Harness (full configurable client)
Phase 15 -> Fair Benchmarking (documented methodology)
Phase 16 -> External Comparison (vs Memcached if practical)
Phase 17 -> Bottleneck Analysis
Phase 18 -> Sharding (only if lock contention measured)
Phase 19 -> Sanitizers (ASAN/UBSAN/TSAN where supported)
Phase 20 -> Final Test Suite
```

Rule: Implement exactly one phase at a time. Do not proceed automatically.

---

## 0.12 Key Design Decisions and Rationale

| Decision    | Choice                       | Rationale                                           |
|-------------|------------------------------|-----------------------------------------------------|
| Language    | C++20                        | Modern features (jthread, concepts) no runtime cost |
| Networking  | Winsock2 (Windows only)      | Raw socket API, educational, no framework bloat     |
| Protocol    | Line-oriented text           | Debuggable with telnet, simple to parse             |
| Storage     | std::unordered_map           | Standard, well-understood, O(1) average             |
| Threading   | Thread pool (Phase 7+)       | Bounded resources vs unbounded thread-per-connection|
| Locking     | mutex then shared_mutex      | Don't optimize before measuring                     |
| Eviction    | LRU (map + doubly linked)    | Classic O(1) eviction - interview-standard          |
| Persistence | WAL (append-only)            | Simple to implement, crash-safe replay              |
| Testing     | Manual asserts then Catch2   | Keep it simple, grow as needed                      |
| Benchmarking| Custom C++ harness           | Control over workload, no external dependencies     |

---

*Last updated: Phase 17 complete. Next: Phase 19 — Sanitizers / Quality Verification.*
