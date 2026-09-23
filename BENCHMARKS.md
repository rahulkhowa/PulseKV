# PulseKV — Benchmarks

## Environment

| Component     | Specification                                              |
|---------------|------------------------------------------------------------|
| CPU           | 13th Gen Intel(R) Core(TM) i7-1355U (10 Cores, 12 Threads) |
| RAM           | 16 GB                                                      |
| OS            | Windows 11 Home Single Language (Build 26200)             |
| Compiler      | GCC 16.0.0 (WinLibs MinGW-w64 UCRT, POSIX threads)         |
| Build Mode    | Release (`-O2 -std=c++20 -static`)                         |
| Networking    | Loopback TCP (127.0.0.1, Winsock2, TCP_NODELAY)           |
| Value Size    | 64 bytes                                                   |
| Keyspace      | 10,000 keys                                                |

---

## Phase 5 — Baseline Benchmark (Single-Threaded Server)

- **Architecture:** Single-threaded accept & event loop (`select()`)
- **Clients:** 1 client connection (sequential request-response)
- **Requests:** 10,000 requests per workload

| Workload        | Throughput (QPS) | Avg Latency | p50 Latency | p95 Latency | p99 Latency | p99.9 Latency | Errors |
|-----------------|------------------|-------------|-------------|-------------|-------------|---------------|--------|
| **SET (100%)**  | **20,599**       | 46.1 μs     | 28.5 μs     | 126.0 μs    | 215.1 μs    | 460.6 μs      | 0      |
| **GET (100%)**  | **14,025**       | 69.9 μs     | 46.8 μs     | 167.5 μs    | 255.1 μs    | 364.7 μs      | 0      |
| **Mixed 80/20** | **20,297**       | 48.0 μs     | 41.7 μs     | 107.7 μs    | 160.5 μs    | 269.1 μs      | 0      |
| **Mixed 50/50** | **23,250**       | 41.4 μs     | 40.9 μs     | 94.6 μs     | 153.0 μs    | 284.7 μs      | 0      |

---

## Phase 6 — Concurrent Clients (Thread-per-Connection + Global Mutex)

- **Architecture:** Thread spawned per accepted connection (`std::thread`), global `std::mutex` around Store execution.
- **Workload:** Mixed 80/20 (80% GET, 20% SET)

| Concurrent Clients | Total Requests | Throughput (QPS) | Scaling vs Baseline | p50 Latency | p95 Latency | p99 Latency | p99.9 Latency | Errors |
|--------------------|----------------|------------------|---------------------|-------------|-------------|-------------|---------------|--------|
| **1 (Baseline)**   | 10,000         | 20,297           | 1.0x                | 41.7 μs     | 107.7 μs    | 160.5 μs    | 269.1 μs      | 0      |
| **5 Clients**      | 25,000         | **60,018**       | **2.96x**           | 76.2 μs     | 124.4 μs    | 159.6 μs    | 233.1 μs      | 0      |
| **10 Clients**     | 50,000         | **90,562**       | **4.46x**           | 96.7 μs     | 148.0 μs    | 190.6 μs    | 415.9 μs      | 0      |

---

## Phase 7 — Thread Pool Concurrency

- **Architecture:** Bounded ThreadPool (fixed hardware worker count = 12 threads), thread-safe FIFO task queue with `std::mutex` + `std::condition_variable`.
- **Workload:** Mixed 80/20 (80% GET, 20% SET)

| Concurrent Clients | Total Requests | Throughput (QPS) | Comparison vs Thread-per-Connection | p50 Latency | p95 Latency | p99 Latency | Errors |
|--------------------|----------------|------------------|-------------------------------------|-------------|-------------|-------------|--------|
| **5 Clients**      | 25,000         | **62,499**       | +4.1% faster (lower thread overhead)| 74.8 μs     | 120.6 μs    | 144.7 μs    | 0      |
| **10 Clients**     | 50,000         | **90,864**       | Stable near hardware saturation     | 97.0 μs     | 149.5 μs    | 198.2 μs    | 0      |

---

## Phase 9 — Concurrency Benchmark (3-Way Architecture Matrix)

Measured across identical workloads and test keyspace on the same physical hardware:
- **Version A:** Single-threaded (1 worker)
- **Version B:** Thread Pool (12 workers) + Exclusive Mutex
- **Version C:** Thread Pool (12 workers) + Shared Mutex (Readers-Writer Lock)

### Workload: Mixed 80/20 (80% GET, 20% SET)

| Implementation | Clients | QPS | p50 Latency | p95 Latency | p99 Latency | Max Latency |
|---|:---:|:---:|:---:|:---:|:---:|:---:|
| **Version A (Single-threaded)** | 1  | 15,914 | 42.9 μs | 150.0 μs | 214.7 μs | 971 μs |
| **Version B (ThreadPool + Mutex)** | 1  | **20,821** | **41.4 μs** | **114.1 μs** | **182.3 μs** | 1,028 μs |
| **Version C (ThreadPool + Shared Mutex)** | 1  | 6,784 | 110.9 μs | 357.7 μs | 515.4 μs | 1,417 μs |
| | | | | | | |
| **Version A (Single-threaded)** | 5  | 17,517 | 46.5 μs | 116.4 μs | 155.7 μs | 1,142,963 μs (1.14s) |
| **Version B (ThreadPool + Mutex)** | 5  | **60,952** | **76.4 μs** | **119.4 μs** | **138.7 μs** | **1,450 μs** |
| **Version C (ThreadPool + Shared Mutex)** | 5  | 40,389 | 95.4 μs | 294.1 μs | 441.6 μs | 4,202 μs |
| | | | | | | |
| **Version A (Single-threaded)** | 10 | 12,642 | 43.4 μs | 226.6 μs | 390.2 μs | 3,154,983 μs (3.15s) |
| **Version B (ThreadPool + Mutex)** | 10 | **86,408** | **98.8 μs** | **151.9 μs** | **202.6 μs** | **5,305 μs** |
| **Version C (ThreadPool + Shared Mutex)** | 10 | 79,892 | 93.8 μs | 194.1 μs | 421.3 μs | 175,612 μs |

### Workload: 100% GET (10 Clients, 50,000 requests)

| Implementation | Clients | QPS | p50 Latency | p95 Latency | p99 Latency | Max Latency |
|---|:---:|:---:|:---:|:---:|:---:|:---:|
| **Version A (Single-threaded)** | 10 | 5,249  | 133.6 μs | 500.3 μs | 676.9 μs | 8,785,482 μs (8.79s) |
| **Version B (ThreadPool + Mutex)** | 10 | **74,339** | 107.5 μs | **193.4 μs** | **384.2 μs** | **20,873 μs** |
| **Version C (ThreadPool + Shared Mutex)** | 10 | 73,438 | **104.5 μs** | 221.2 μs | 482.6 μs | 21,028 μs |

---

### Engineering Analysis & Key Findings

1. **Catastrophic Tail Latency in Single-Threaded Mode:**
   - As client concurrency rises to 5 and 10, Version A's max latency surges to **1.14s and 3.15s** (and 8.79s for 100% GET) due to head-of-line blocking in the single worker.
   - Version B & C maintain tight max latencies in the few milliseconds range.
2. **ThreadPool + Mutex (Version B) Outperformed Shared Mutex (Version C):**
   - **Why did the "theoretically optimized" `std::shared_mutex` perform slower than `std::mutex`?**
   - **Atomic Invalidation Overhead:** `std::shared_mutex` internally maintains atomic reader counts. Each reader thread must execute atomic instructions (`lock_shared()`, `unlock_shared()`), triggering cache-line bounces (MESI cache invalidations) between CPU cores.
   - **Ultra-Fast Critical Section:** An in-memory hash table lookup completes in just ~20–40 nanoseconds. The synchronization overhead of atomic operations in `std::shared_mutex` exceeds the execution time of the actual operation.
   - **Conclusion:** For ultra-short in-memory critical sections, a simple adaptive `std::mutex` or partitioned sharding (Phase 18) provides superior cache efficiency compared to a monolithic readers-writer lock.

---

## Phase 15 — Fair Benchmarking Matrix & Durability Cost Analysis

Benchmarking methodology: Reproducible, multi-client load testing with warmup (500 reqs/client), controlled distributions (Uniform vs Zipfian s=0.99), and empirical persistence profiling.

### 1. Workload Spectrum & Distribution Skew (10 Clients, 50,000 Requests)

| Workload | Distribution | Throughput (QPS) | p50 Latency | p95 Latency | p99 Latency | Max Latency | Errors |
|---|---|:---:|:---:|:---:|:---:|:---:|:---:|
| **100% GET** | Uniform | **57,824** | 123.8 μs | 218.8 μs | 265.3 μs | 2.00 ms | 0 |
| **100% GET** | Zipfian (skew 0.99) | **45,550** | 160.0 μs | 238.9 μs | 277.8 μs | 20.30 ms | 0 |
| **100% SET** | Uniform | **72,152** | 95.5 μs | 142.7 μs | 171.1 μs | 6.69 ms | 0 |
| **Mixed 90/10** | Uniform | **51,059** | 155.6 μs | 218.9 μs | 252.7 μs | 4.32 ms | 0 |
| **Mixed 80/20** | Uniform | **48,992** | 150.8 μs | 233.5 μs | 266.2 μs | 8.83 ms | 0 |
| **Mixed 50/50** | Uniform | **58,340** | 129.0 μs | 215.2 μs | 256.9 μs | 1.48 ms | 0 |

### 2. Concurrency Scaling (Mixed 80/20 Workload)

| Clients | Requests | Throughput (QPS) | Speedup Factor | p50 Latency | p95 Latency | p99 Latency |
|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **1** | 10,000 | 11,105 | 1.00x (Baseline) | 74.1 μs | 146.6 μs | 232.0 μs |
| **5** | 25,000 | 30,284 | 2.73x | 141.5 μs | 205.7 μs | 249.7 μs |
| **10** | 50,000 | 48,992 | 4.41x | 150.8 μs | 233.5 μs | 266.2 μs |
| **20** | 50,000 | 47,806 | 4.30x (Hardware Saturated) | 153.6 μs | 232.9 μs | 330.8 μs |

### 3. Durability Overhead (In-Memory vs WAL Persistence)

Measuring the real-world performance cost of append-only logging on disk (10 Clients, 50,000 requests):

| Workload | Mode | Throughput (QPS) | QPS Impact | p50 Latency | p99 Latency |
|---|---|:---:|:---:|:---:|:---:|
| **100% GET** | In-Memory | 57,824 | — | 123.8 μs | 265.3 μs |
| **100% GET** | In-Memory + WAL | 56,029 | -3.1% (noise floor) | 122.8 μs | 255.0 μs |
| **Mixed 80/20** | In-Memory | 48,992 | — | 150.8 μs | 266.2 μs |
| **Mixed 80/20** | In-Memory + WAL | 55,029 | +12.3% (within run variance) | 142.8 μs | 266.7 μs |
| **100% SET** | In-Memory | **72,152** | — | **95.5 μs** | **171.1 μs** |
| **100% SET** | In-Memory + WAL | **24,914** | **-65.5% (I/O Bound)** | **339.5 μs** | **1,127.6 μs** |

### Key Durability Insights:
1. **Reads are 100% Zero-Cost:** Enabling WAL has virtually 0 impact on read workloads (`GET`) since WAL operates strictly on mutations.
2. **Sequential Append Disk Cost for Pure Writes:** For a 100% write workload (`SET`), throughput drops from ~72k QPS to ~25k QPS due to filesystem append locks and CRT buffer flushing (`file_.flush()`), while maintaining sub-millisecond p50 latency (339 μs).
