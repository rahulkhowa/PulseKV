# PulseKV — Technical Interview & System Design Guide

---

## 1. 30-Second Elevator Pitch

> "PulseKV is a concurrent, in-memory key-value database built from scratch in C++20. It uses a bounded thread-pool architecture with Winsock TCP sockets, a readers-writer synchronized hash table (`std::shared_mutex` + `std::unordered_map`), $O(1)$ LRU capacity eviction via hash-linked lists, lazy and active background TTL expiration, and write-ahead log (WAL) crash recovery. I built this not as a toy clone, but as a benchmarking laboratory to measure real-world concurrency bottlenecks, evaluate lock contention empirically against single-threaded models, and understand the trade-offs between synchronization overhead and throughput."

---

## 2. Core Architecture & Component Breakdown

```
                       ┌───────────────────────┐
                       │   Client Application  │
                       └───────────┬───────────┘
                                   │ TCP (Wire Protocol)
                                   ▼
                       ┌───────────────────────┐
                       │     Server Core       │
                       │ (Winsock2 Accept Loop)│
                       └───────────┬───────────┘
                                   │ Dispatches Connection Tasks
                                   ▼
                       ┌───────────────────────┐
                       │      ThreadPool       │
                       │ (Task Queue + Mutex/CV│
                       │   + Worker Threads)   │
                       └───────────┬───────────┘
                                   │ Worker Executes handle_client()
                                   ▼
                       ┌───────────────────────┐
                       │    Protocol Parser    │
                       │ (Stateless Line Token)│
                       └───────────┬───────────┘
                                   │ Executes Command
                                   ▼
                       ┌───────────────────────┐       ┌───────────────────────┐
                       │     Store Engine      │──────>│   WAL Writer & Log    │
                       │ (std::shared_mutex    │  Log  │ (Append-Only Text Log │
                       │  std::unordered_map   │  SET/ │   Crash Replay Engine)│
                       │  std::list LRU Cache  │  DEL  └───────────────────────┘
                       │  TTL Background Purge)│
                       └───────────────────────┘
```

### Key Components:
1. **`Server` (`src/server/`):** Listens on TCP port via Winsock2, runs a non-blocking `select()` accept loop, and manages active client socket lifecycles.
2. **`ThreadPool` (`src/concurrency/`):** Bounded worker pool preventing thread exhaustion under heavy client loads. Uses a thread-safe task queue with `std::mutex` and `std::condition_variable`.
3. **`Parser` (`src/protocol/`):** Stateless, zero-allocation tokenizing parser validating arguments and wire-format requests.
4. **`Store` (`src/storage/`):** Thread-safe in-memory key-value engine supporting configurable locking (`std::mutex` vs `std::shared_mutex`), TTL timepoint metadata, and LRU linked-list ordering.
5. **`WALWriter` & `WALRecover` (`src/persistence/`):** Append-only durability log recording mutating operations prior to memory application, supporting crash resilience and replay reconstruction.

---

## 3. Deep-Dive Topics

### 3.1 Networking & Socket Lifecycle
- **Winsock API (Windows):** Built directly on raw OS socket primitives (`WSAStartup`, `socket`, `bind`, `listen`, `accept`, `recv`, `send`).
- **Nagle's Algorithm (`TCP_NODELAY`):** Enabled on all client and server sockets to avoid 40ms delayed-ACK buffering on small request/response packets.
- **Connection Model:** Main thread runs `select()` with a 500ms timeout on the listening socket. When a connection arrives, it is accepted and enqueued as a discrete work task to the ThreadPool.
- **Pipelining Support:** The server accumulates incoming TCP chunks into a per-connection stream buffer (`line_buf`), parsing and executing multiple complete newline-delimited commands in a single TCP receive call.

### 3.2 C++20 Idioms & Memory Safety
- **RAII Throughout:** Sockets, file descriptors, and synchronization locks are wrapped in RAII guards (`std::unique_lock`, `std::shared_lock`, `std::lock_guard`, custom Winsock guards).
- **Explicit Ownership:** The `Server` owns the `ThreadPool` and references the `Store`. No circular references; zero overhead from unnecessary `std::shared_ptr`.
- **Move Semantics:** String keys and values are moved into hash table entries to avoid redundant heap allocations.

### 3.3 Concurrency & Lock Contention
- **Exclusive Mutex (`std::mutex`):** Simple global lock serializing all operations. High contention under heavy parallel reads.
- **Readers-Writer Lock (`std::shared_mutex`):** Allows multiple concurrent read threads (`GET`, `EXISTS`) with `std::shared_lock`, upgrading to `std::unique_lock` only on mutating operations (`SET`, `DEL`, LRU touch).
- **Lock Contention Findings:** As measured in Phase 9, for read-heavy workloads (100% GET), `std::shared_mutex` scales near-linearly across cores. For write-heavy workloads (100% SET), readers-writer locks introduce slightly higher lock-acquisition overhead than a standard `std::mutex`.

### 3.4 Storage, TTL & LRU Design
- **Expected Hash Complexity:** Average $O(1)$ for `SET`, `GET`, `DEL`, `EXISTS` using `std::unordered_map`.
- **Dual Expiration Strategy (TTL):**
  - *Lazy Expiration:* Upon `GET` or `EXISTS`, the timestamp is checked against `std::chrono::steady_clock::now()`. If expired, the key is lazily purged and null is returned.
  - *Active Background Cleanup:* A dedicated background thread wakes every 100ms via condition variable and purges batches of expired keys to prevent memory leaks from inactive expired keys.
- **LRU Eviction ($O(1)$):**
  - Combines `std::unordered_map<string, TableEntry>` with a doubly-linked list (`std::list<string>`).
  - The map stores an iterator pointing directly to the key's node in the linked list (`std::list<string>::iterator pos`).
  - When accessed, `lru_order_.splice()` relocates the node to the front (MRU) in $O(1)$ without reallocating list memory. When capacity is exceeded, the back node (LRU) is popped in $O(1)$.

### 3.5 Write-Ahead Log (WAL) & Durability
- **Write-Ahead Principle:** In mutating commands (`SET`, `DEL`), the entry is appended to the WAL and flushed *before* the in-memory map is modified.
- **Crash Recovery:** On restart, `WALRecover::replay()` processes the log line-by-line, applying valid records and gracefully skipping truncated or corrupted trailing lines.

---

## 4. Design Decisions & Trade-Offs Matrix

| Component | Decision Taken | Alternatives Considered | Why This Choice? | Empirical / Practical Evidence |
|-----------|----------------|--------------------------|------------------|--------------------------------|
| **Threading Model** | Bounded `ThreadPool` | Thread-per-connection, Single-thread event loop | Avoids unbounded OS thread exhaustion under high client counts. | Scaled to 500+ concurrent clients with 0 errors. |
| **Locking Primitives** | `std::shared_mutex` | Global `std::mutex`, Fine-grained sharded locks | Optimized for common read-heavy cache workloads without premature sharding complexity. | Phase 9 benchmarks: 80/20 mixed read throughput increased 3.2x over global mutex. |
| **Memory Eviction** | Map + `std::list` (LRU) | Sampled LRU (Redis-style), Clock algorithm | Strict $O(1)$ operations with deterministic eviction behavior. | Maintained constant memory footprint during 100k-key insertions. |
| **Persistence** | Append-Only Text WAL | SQLite, Binary B-tree, Snapshots | Simple, crash-resilient, human-readable, zero external dependencies. | Phase 13 crash tests: 100% data recovery across simulated mid-write failures. |
| **Protocol** | Line-Oriented Text | Binary RESP, Protobuf, JSON | Easy to debug via telnet/CLI while supporting pipelining. | Verified ~80,000 QPS over TCP loopback. |

---

## 5. System Design Interview Questions & Answers

### Q1: How would you scale PulseKV to handle 100,000 concurrent clients?
- **Current Implementation:** PulseKV uses a bounded ThreadPool (e.g. 16–64 worker threads) serving connections sequentially over blocking Winsock sockets. While stable up to hundreds of clients, it cannot scale to 100,000 idle connections due to worker thread starvation.
- **Future Scaling Design:**
  1. *Asynchronous I/O:* Replace the thread-per-task model with an event-driven I/O multiplexer (Windows I/O Completion Ports / IOCP or Linux `epoll`).
  2. *Single-threaded or Reactor Model:* Utilize non-blocking socket reactors where worker threads process events rather than holding entire connection lifecycles.
  3. *Connection Proxy:* Place an edge layer (e.g., Envoy or a custom C++ proxy) to multiplex client TCP connections into pooled backend connections.

### Q2: How would you distribute data across multiple nodes?
- **Current Implementation:** Single-node in-memory store.
- **Future Scaling Design:**
  1. *Consistent Hashing:* Implement a distributed hash ring (e.g. Ketama hash ring with virtual nodes) to partition keys uniformly across $N$ storage nodes.
  2. *Routing:* Use smart client routing or proxy-based routing where `hash(key)` determines the target node.
  3. *Re-balancing:* When nodes join or leave, only $K/N$ keys are migrated between neighboring ring partitions.

### Q3: How would you implement Replication & High Availability?
- **Current Implementation:** Local append-only WAL log file.
- **Future Scaling Design:**
  1. *Primary-Replica Replication:* The primary node streams its WAL entries asynchronously or synchronously over a dedicated replication TCP socket to replica nodes.
  2. *Leader Election:* Implement a consensus algorithm (Raft) across 3 or 5 nodes to elect a leader and manage automated failover.
  3. *Read Replicas:* Direct `GET` queries to replicas while routing `SET`/`DEL` to the Raft leader.

### Q4: How would you handle Hot Keys?
- **Current Implementation:** All requests for a key hit the single `Store` map protected by `std::shared_mutex`.
- **Future Scaling Design:**
  1. *Client-side / Near-Cache:* Cache hot read keys in client memory for short TTL durations (e.g., 500ms).
  2. *Local Read Replicas:* Scale read replicas horizontally and balance reads across nodes.
  3. *Key Splitting:* For counters or aggregate keys, append random suffixes (e.g. `hot_key_1`, `hot_key_2`) and aggregate on read.

### Q5: What is your current system bottleneck?
- **Current Implementation Bottleneck:** As demonstrated in [BOTTLENECK_ANALYSIS.md](BOTTLENECK_ANALYSIS.md), loopback TCP socket syscall overhead (`recv`/`send`) and thread context-switching account for over 70% of total request latency, whereas storage map lookup takes under 15%.
- **Optimization Strategy:** Transition to kernel-bypass networking (io_uring on Linux / Registered I/O on Windows) and command batching/pipelining.
