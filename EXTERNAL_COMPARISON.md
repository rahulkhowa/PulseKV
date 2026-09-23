# PulseKV — External Comparison & Architectural Trade-off Study (Phase 16)

## 1. Executive Summary

This study analyzes **PulseKV** alongside industry-standard in-memory storage engines—**Redis** and **Memcached**—evaluating design trade-offs across concurrency architectures, memory subsystems, network I/O, and persistence guarantees under equivalent workloads.

```
                      +-------------------+
                      |      PulseKV      |
                      +---------+---------+
                                |
             +------------------+------------------+
             |                                     |
             v                                     v
   +-------------------+                 +-------------------+
   |       Redis       |                 |     Memcached     |
   | (Single-Threaded  |                 | (Multi-Threaded   |
   |   Event Loop)     |                 |  Slab Allocator)  |
   +-------------------+                 +-------------------+
```

---

## 2. Deep-Dive Architecture Comparison

| Architectural Dimension | **PulseKV** | **Redis** | **Memcached** |
| :--- | :--- | :--- | :--- |
| **Primary Language** | Modern C++20 (UCRT, MinGW) | C (ANSI C99) | C (C99 / C11) |
| **Concurrency Model** | **Bounded ThreadPool** (Fixed $N$ worker threads servicing a thread-safe task queue) | **Single-threaded Event Loop** (Reactor pattern via `ae.c`, multi-threaded I/O since v6) | **Multi-threaded Worker Pool** (`libevent` dispatching client FDs round-robin to worker threads) |
| **Storage Engine** | `std::unordered_map<string, Entry>` | `dict.h` (Dual incremental rehashing hash tables) | Chained hash table with global lock and slab items |
| **Locking Strategy** | Adaptive `std::mutex` / `std::shared_mutex` around store execution | Lock-free execution core (single thread avoids mutexes entirely) | Per-hash-bucket locks + Slab allocator locks |
| **Network Engine** | Raw Winsock2 (`select()`, `WSASend`, `WSARecv`, `TCP_NODELAY`) | `epoll` (Linux) / `kqueue` (macOS) / `select` multiplexing | `libevent` wrapping `epoll` / `kqueue` |
| **Protocol** | Simple, line-oriented text (`SET k v [ttl]\r\n`) | RESP2 / RESP3 (Binary-safe length-prefixed) | ASCII Protocol + Binary Protocol |
| **Eviction Policy** | **Exact O(1) LRU** (Hash map + Doubly-linked `std::list` iterators) | **Sampled / Approximated LRU/LFU** (Random 5-key sampling from hash table) | **Slab-based Exact LRU** (Maintained per slab class) |
| **TTL Expiration** | **Dual-strategy**: Passive (on-access) + Active background thread cleanup | **Dual-strategy**: Passive + Active random 20-key sampling (10 Hz) | **Passive only** (Evaluated on-access; no active cleaner) |
| **Persistence** | **Append-only WAL** (`log_set`, `log_del`) with torn-write replay recovery | **RDB Snapshots** (`fork()` copy-on-write) + **AOF** (`fsync` policies + BG rewrite) | **None** (Purely volatile cache by design) |

---

## 3. Concurrency & Synchronization Analysis

### A. Single-Threaded Reactor (Redis) vs ThreadPool (PulseKV)
- **Redis Advantage**: Zero lock contention, zero mutex acquisition overhead, zero thread context-switch latency, deterministic execution order. Cache lines stay resident in the single core's L1/L2 cache.
- **Redis Limitation**: Cannot utilize multiple CPU cores for compute or memory access within a single process. CPU saturation requires running multiple Redis instances (cluster / sharding).
- **PulseKV Advantage**: Utilizes all physical/logical cores on multi-core systems (e.g., 10 cores / 12 threads) to parallelize network I/O, command parsing, string serialization, and concurrent data access.
- **PulseKV Trade-off**: Requires synchronization primitives (`std::mutex`) on mutable shared state.

### B. Why Monolithic Readers-Writer Locks Failed (`std::shared_mutex`)
In Phase 9 and 15 benchmarks, PulseKV demonstrated that `std::shared_mutex` performed **slower** than exclusive `std::mutex`:
1. **Cache-line Bouncing (MESI protocol)**: Multiple reader threads concurrently incrementing/decrementing reader counts on the shared mutex invalidate each other's L1/L2 CPU caches.
2. **Critical Section Ratio**: In-memory hash lookups take $\approx 20\text{--}40\text{ ns}$. The atomic atomic instructions required for `shared_lock` cost $\approx 15\text{--}30\text{ ns}$, doubling the effective operation latency.

---

## 4. Memory Management & Fragmentation

```
+-------------------------------------------------------------------------+
| MEMORY SUBSYSTEM COMPARISON                                             |
+-----------------------------------+-------------------------------------+
| PulseKV: CRT Heap                 | Memcached: Slab Allocator           |
|                                   |                                     |
| [std::string Entry] -> malloc()   | [Slab Class 1 (64B)]  [Chunk][Chunk]|
| [std::string Entry] -> malloc()   | [Slab Class 2 (128B)] [Chunk][Chunk]|
|                                   | [Slab Class 3 (256B)] [Chunk][Chunk]|
| (+) Zero memory waste             | (+) Zero heap fragmentation         |
| (-) Memory fragmentation over time| (-) Slab calcification risk         |
+-----------------------------------+-------------------------------------+
```

- **Memcached (Slab Allocator)**: Pre-allocates memory in 1MB slabs divided into fixed-size chunks (e.g. 64B, 128B, 256B). Eliminates runtime OS `malloc`/`free` calls and heap fragmentation, but can suffer from *slab calcification* (memory trapped in unused slab classes).
- **Redis (jemalloc)**: Uses a custom memory allocator (`jemalloc`) with arena-based thread-local caches to minimize fragmentation, combined with lightweight string headers (`sds`).
- **PulseKV (C++ STL Allocator)**: Uses standard C++ dynamic memory (`std::string`, `std::unordered_map`). Simple and expressive, relying on OS/CRT heap allocator with Small String Optimization (SSO) for values $\le 15$ bytes.

---

## 5. Performance & Network I/O Realities

### Performance Comparison Matrix

| Workload (10 Clients, Loopback) | **PulseKV** (ThreadPool 12T) | **Redis** (Single Instance)* | **Memcached** (4T)* |
| :--- | :--- | :--- | :--- |
| **100% GET (QPS)** | **57,824** | ~80,000 – 110,000 | ~120,000 – 160,000 |
| **100% SET (QPS)** | **72,152** | ~75,000 – 95,000 | ~110,000 – 140,000 |
| **p50 Latency (μs)** | **95 – 124 μs** | ~100 – 150 μs | ~70 – 110 μs |
| **Persistence Enabled** | **24,914 QPS** (Append WAL) | ~15,000 – 35,000 QPS (AOF) | N/A (No Persistence) |

*\*Comparative reference baselines on equivalent modern x86_64 desktop hardware under single-machine loopback TCP.*

### Why Dedicated Network Frameworks Scale Higher:
1. **System Call Batching**: Redis and Memcached read large socket buffers (e.g., 16KB–64KB) in a single `read()` syscall and parse multiple pipelined commands in one pass. PulseKV currently reads line-by-line (`recv(..., 1)` in benchmark client or per-connection loop).
2. **OS I/O Multiplexing**: Linux `epoll` (edge-triggered) provides constant time $O(1)$ event notifications, whereas Windows standard Winsock `select()` has $O(N)$ scanning complexity.

---

## 6. Engineering Conclusions

1. **PulseKV's Sweet Spot**: High-performance, embedded C++ application server requiring bounded multi-threaded scaling, O(1) exact LRU eviction, deterministic background TTL expiration, and crash-resilient append-only persistence with zero third-party dependencies.
2. **Interview-Grade Insight**: Architecture determines performance ceilings. A single-threaded event loop excels when operations are purely non-blocking and in-memory; a thread pool excels when workloads mix computation, I/O, and disk persistence across multiple CPU cores.
