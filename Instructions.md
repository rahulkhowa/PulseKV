PulseKV — Concurrent Key-Value Store & Benchmarking Laboratory

The important part is that PulseKV is your implementation, while the benchmarking methodology compares it against established systems.

Save the following as Implementation.md.

# PulseKV — Implementation Plan

## Concurrent In-Memory Key-Value Store & Benchmarking Laboratory

---

# 0. PROJECT PURPOSE

PulseKV is an educational but technically serious key-value store implemented from scratch in modern C++.

The project is designed to demonstrate:

- Modern C++
- TCP networking
- Client/server architecture
- Data structures
- Multithreading
- Thread pools
- Synchronization
- Concurrent data structures
- TTL expiration
- LRU eviction
- Persistence
- Crash recovery
- Benchmarking
- Performance analysis
- Systems design

The project is NOT intended to replace Redis, Memcached, Dragonfly, or another production database.

The purpose is:

> Build a small systems-oriented storage engine from first principles, measure its behavior under load, identify bottlenecks, optimize it, and compare it fairly against established systems.

---

# 1. CORE PRINCIPLES

## 1.1 Build From First Principles

Do not use Redis internally.

Do not depend on Memcached internally.

Do not use an existing key-value database.

The storage engine must be implemented by us.

---

## 1.2 Keep the Project Understandable

This is an interview project.

Avoid unnecessary:

- factories
- managers
- registries
- providers
- dependency injection frameworks
- excessive inheritance
- generic abstractions
- complicated configuration
- unnecessary third-party libraries

Prefer simple and explicit C++.

---

## 1.3 Do Not Optimize Before Measuring

The project must evolve through measurable stages.

The intended development cycle is:

```text
Implement
   ↓
Test
   ↓
Benchmark
   ↓
Identify bottleneck
   ↓
Optimize
   ↓
Benchmark again
   ↓
Compare

Do not make claims such as "high performance" without measurements.

1.4 Do Not Fake Benchmarks

Never:

invent benchmark numbers
copy benchmark numbers from another project
claim performance without executing the benchmark
compare different workloads and call them equivalent
hide machine specifications
cherry-pick favorable results

All performance numbers must come from actual executions.

2. TARGET ARCHITECTURE

The project should evolve toward:

                         Clients
                            |
                            | TCP
                            ↓
                  +---------------------+
                  |   PulseKV Server    |
                  +----------+----------+
                             |
                     Connection Layer
                             |
                       Thread Pool
                             |
                       Command Parser
                             |
                       Storage Engine
                             |
              +--------------+--------------+
              |              |              |
           Hash Map         TTL            LRU
              |              |              |
              +--------------+--------------+
                             |
                           WAL
                             |
                            Disk

The architecture should be implemented incrementally.

Do NOT build everything at once.

3. TECHNOLOGY

Primary language:

C++20

Build system:

CMake

Testing:

Use a lightweight C++ testing framework only if genuinely useful.

Benchmarking:

Build a dedicated benchmark client/harness.

Networking:

Use native socket APIs.

On Linux/macOS:

POSIX sockets

On Windows:

Winsock

If cross-platform support is practical, abstract only the minimal platform-specific socket operations.

Do not introduce a large networking framework merely for convenience.

4. REPOSITORY STRUCTURE

Target a structure approximately like:

PulseKV/
│
├── CMakeLists.txt
├── README.md
├── Implementation.md
├── INTERVIEW_GUIDE.md
├── BENCHMARKS.md
├── .gitignore
│
├── include/
│   └── pulsekv/
│       ├── server/
│       ├── storage/
│       ├── protocol/
│       ├── concurrency/
│       └── persistence/
│
├── src/
│   ├── server/
│   ├── storage/
│   ├── protocol/
│   ├── concurrency/
│   └── persistence/
│
├── tests/
│
├── benchmark/
│
└── client/

The exact structure may be simplified if fewer directories are sufficient.

Do not create empty architectural layers.

5. PHASE 0 — DESIGN

Before implementation:

Inspect the development environment.
Decide supported operating systems.
Define the protocol.
Define the storage API.
Define the concurrency model.
Define ownership/lifetime rules.
Define error handling.
Define shutdown behavior.
Define testing strategy.
Define benchmarking methodology.

Produce a concise design document.

Do not write implementation code yet.

6. PHASE 1 — BASIC STORAGE ENGINE

Implement an in-memory key-value store.

Initial data structure:

std::unordered_map<std::string, std::string>

Support:

SET key value
GET key
DEL key
EXISTS key

Example:

SET name Rahul
GET name
DEL name
EXISTS name

Expected complexity:

SET     average O(1)
GET     average O(1)
DEL     average O(1)
EXISTS  average O(1)

Document that these are expected/average hash-table complexities, not absolute guarantees.

7. PHASE 2 — COMMAND PARSER

Implement a simple text protocol.

Example:

SET name Rahul
GET name
DEL name
EXISTS name

Responses should be simple and deterministic.

Handle:

empty commands
unknown commands
missing arguments
excessive arguments
malformed requests

Do not build a complicated parser.

8. PHASE 3 — TCP SERVER

Build a TCP server around the storage engine.

Architecture:

TCP Client
    ↓
accept()
    ↓
receive()
    ↓
parse()
    ↓
execute()
    ↓
response

The server should:

bind
listen
accept
receive
process commands
send responses
close connections correctly
shut down gracefully

Start with a simple single-threaded implementation.

This is intentional.

9. PHASE 4 — CLIENT

Build a minimal command-line client.

Example:

pulsekv-client

Allow:

SET name Rahul
GET name
DEL name

The client should communicate with the server over TCP.

Do not build a GUI.

10. PHASE 5 — FIRST BASELINE

Before introducing concurrency, benchmark the single-threaded implementation.

Measure:

throughput
p50 latency
p95 latency
p99 latency
p99.9 latency where practical
errors
CPU usage
memory usage

Run workloads such as:

GET
SET
80% GET / 20% SET
50% GET / 50% SET

Record:

CPU
RAM
OS
compiler
build mode
request count
value size
client count
benchmark duration

This becomes the baseline.

11. PHASE 6 — CONCURRENT CLIENTS

Modify the server to support multiple clients.

Do NOT immediately optimize the storage engine.

First introduce concurrency around the existing storage.

Investigate:

thread-per-connection
thread pool

Prefer a thread pool for the final implementation if measurements and complexity justify it.

Document the trade-off.

12. PHASE 7 — THREAD POOL

Implement a simple thread pool.

Required concepts:

Worker threads
Task queue
Mutex
Condition variable
Graceful shutdown

Architecture:

                    Server
                      |
                 Accept loop
                      |
                 Task Queue
                /     |     \
               ↓      ↓      ↓
           Worker   Worker   Worker
               \      |      /
                    Store

Use:

std::thread
std::mutex
std::condition_variable
std::atomic

where appropriate.

Avoid unnecessary abstractions.

13. PHASE 8 — THREAD-SAFE STORAGE

Make the storage engine safe for concurrent access.

Start with a simple locking strategy.

Investigate:

std::mutex

and, where appropriate:

std::shared_mutex

Benchmark the change.

Document:

read/write behavior
lock contention
critical sections
race conditions
why a particular locking strategy was selected
14. PHASE 9 — CONCURRENCY BENCHMARK

Run the same benchmark against:

Version A:
Single-threaded

Version B:
Thread pool + mutex

Version C:
Thread pool + shared_mutex

Do not assume which one is faster.

Measure it.

Produce a table:

Implementation | Clients | QPS | p50 | p95 | p99
---------------------------------------------------
Single-thread | ...
Mutex         | ...
Shared mutex  | ...

If a supposedly optimized version performs worse, document why.

That is a valid engineering result.

15. PHASE 10 — TTL

Add expiration.

Example:

SET session Rahul EX 60

The key expires after 60 seconds.

Consider:

lazy expiration
background cleanup
concurrent reads
concurrent writes
clock handling

Prefer the simplest correct design.

Test:

SET
wait
GET

and verify expiration.

16. PHASE 11 — LRU EVICTION

Add optional bounded-memory mode.

Use:

unordered_map
+
doubly linked list

Target:

GET      average O(1)
SET      average O(1)
EVICT    O(1)

Explain:

Hash map
   ↓
key → node

Doubly linked list
   ↓
Most Recently Used
       ...
Least Recently Used

Benchmark memory behavior.

17. PHASE 12 — WRITE-AHEAD LOG

Add optional persistence.

For mutating operations:

SET
DEL

append records to a WAL.

Example conceptual record:

SET user Rahul

On restart:

WAL
 ↓
Replay
 ↓
Reconstruct memory

Test:

Start server
SET values
Stop server
Start server
GET values

The values should be recovered.

18. PHASE 13 — CRASH / RECOVERY TESTING

Test:

clean shutdown
restart
malformed WAL record
incomplete final record
empty WAL
repeated operations

Do not promise full database-grade durability unless it is actually implemented.

Document the durability guarantees.

19. PHASE 14 — BENCHMARK HARNESS

Build a dedicated benchmark client.

It should support configurable:

server address
port
number of clients
number of requests
number of threads
workload
key distribution
value size
GET/SET ratio
warmup duration
benchmark duration

Example:

pulsekv-benchmark \
    --clients 50 \
    --requests 100000 \
    --workload get

Output:

Benchmark Results
=================

Workload: GET
Clients: 50
Requests: 100000

Throughput:
XX,XXX requests/sec

Latency:
p50:   X.XX ms
p95:   X.XX ms
p99:   X.XX ms
p99.9: X.XX ms
max:   X.XX ms

Errors:
0
20. PHASE 15 — FAIR BENCHMARKING

The benchmark methodology must be reproducible.

Record:

Hardware
--------
CPU
RAM
OS
Compiler

Software
--------
Optimization flags
Build type
Version

Workload
--------
Requests
Clients
Threads
Key size
Value size
GET/SET ratio
Duration
Warmup

Never compare two systems using different workloads.

21. PHASE 16 — EXTERNAL COMPARISON

After PulseKV is stable, benchmark it against established systems where practical.

Potential comparison:

PulseKV
Memcached

Optionally another appropriate local key-value store if the environment permits a fair comparison.

The objective is NOT:

Beat every production system.

The objective is:

Understand how a small C++ implementation behaves compared with established systems under the same workload.

Use the same:

machine
benchmark client/workload where possible
request count
concurrency
key/value sizes
GET/SET ratio
measurement method

Clearly state limitations.

22. PHASE 17 — BOTTLENECK ANALYSIS

Use benchmark results to identify bottlenecks.

Possible bottlenecks:

socket I/O
parser
thread scheduling
lock contention
hash table
memory allocation
network bandwidth
WAL
client implementation

Do not assume the bottleneck.

Measure and reason about it.

23. PHASE 18 — OPTIONAL SHARDING

Only implement this if the benchmark demonstrates lock contention or another clear reason.

Partition storage:

hash(key) % N

Example:

             Storage
                |
       +--------+--------+
       |        |        |
    Shard 0  Shard 1  Shard 2
       |        |        |
    Lock 0   Lock 1   Lock 2

The objective is to reduce contention.

Benchmark:

Global lock
     vs
Sharded locks

Do not add sharding merely because it sounds advanced.

24. PHASE 19 — SANITIZERS

Where supported, run:

AddressSanitizer
UndefinedBehaviorSanitizer
ThreadSanitizer

Use them to detect:

memory errors
undefined behavior
data races

Fix actual issues.

Do not ignore sanitizer failures.

25. PHASE 20 — FINAL TEST SUITE

Test:

Storage
SET
GET
DEL
EXISTS
Parser
valid
invalid
malformed
TCP
single client
multiple clients
disconnect
malformed request
Concurrency
concurrent reads
concurrent writes
mixed workload
TTL
expiration
non-expiration
LRU
eviction
access ordering
Persistence
write
restart
recovery
malformed record
Server
startup
shutdown
error handling
26. PERFORMANCE REPORT

Create:

BENCHMARKS.md

It should contain:

Environment
CPU:
RAM:
OS:
Compiler:
Build:
Workloads
GET
SET
80/20
50/50
Concurrency
1
5
10
25
50
100
250
500

where practical.

Results

Include:

QPS
p50
p95
p99
errors
CPU
memory
Internal Comparison
Single thread
vs
Mutex
vs
Shared mutex
vs
Sharding

Only include implementations that were actually built.

External Comparison

Document:

PulseKV
vs
Memcached

or another system if actually benchmarked.

Include methodology and limitations.

27. GRAPHS

Generate useful graphs such as:

Throughput vs concurrent clients
X = concurrent clients
Y = requests/sec
p99 latency vs concurrent clients
X = concurrent clients
Y = p99 latency
Implementation comparison
Single-thread
Mutex
Shared mutex
Sharded

Graphs must be generated from actual benchmark data.

Do not manually fabricate data.

28. README

Create a concise README.

Include:

What is PulseKV?

A C++ concurrent in-memory key-value server built from scratch to study networking, concurrency, storage, persistence, and performance.

Features

Only list implemented features.

Architecture

Include a simple architecture diagram.

Build

Explain CMake build.

Run

Explain server/client.

Commands
SET
GET
DEL
EXISTS
Benchmarking

Show how to execute the benchmark.

Results

Link to BENCHMARKS.md.

Do not claim performance numbers that haven't been measured.

29. INTERVIEW GUIDE

Create:

INTERVIEW_GUIDE.md

Include:

Project explanation

30-second explanation.

Architecture

Explain every major component.

Networking
TCP
socket lifecycle
client/server
connection handling
C++
RAII
ownership
smart pointers where used
move semantics where relevant
STL containers
Concurrency
threads
thread pool
mutex
shared_mutex
condition_variable
atomics
race conditions
deadlocks
Storage
unordered_map
hashing
complexity
TTL

Explain expiration design.

LRU

Explain why:

unordered_map + doubly linked list

gives O(1) operations.

Persistence

Explain WAL and recovery.

Performance

Explain:

throughput
latency
percentiles
QPS
benchmark methodology
Bottlenecks

Explain actual bottlenecks discovered through measurements.

Trade-offs

For every major design decision:

Problem
Decision
Why
Trade-off
Evidence
30. SYSTEM DESIGN QUESTIONS

Prepare answers for:

How would you scale PulseKV?
How would you handle 100,000 clients?
How would you distribute the data?
How would you implement replication?
How would you handle node failure?
How would you partition keys?
How would you handle hot keys?
How would you make persistence durable?
How would you reduce lock contention?
What is your current bottleneck?
Why not use Redis?
Why not use Memcached?
Why C++?
Why TCP?
Why a thread pool?
Why an unordered_map?
Why LRU?
How does TTL work?
What happens during a crash?
How would you benchmark it fairly?

Answers must distinguish:

CURRENT IMPLEMENTATION

from:

FUTURE SCALING DESIGN

Do not claim replication, clustering, distributed consensus, etc. unless actually implemented.

31. FUTURE SCALING DESIGN

Do NOT implement these unless explicitly added in a later phase.

Document conceptually:

                    Load Balancer
                         |
             +-----------+-----------+
             |           |           |
          Node 1       Node 2      Node 3
             |           |           |
          Shards       Shards      Shards
             |
        Replication
             |
       Persistent WAL

Possible future topics:

sharding
replication
consistent hashing
leader/follower
failover
distributed persistence
backpressure
load balancing

These belong in interview/system-design documentation unless deliberately implemented.

32. IMPLEMENTATION RULE FOR THE AGENT

When an agent is asked to continue this project:

FIRST

Read:

Implementation.md
THEN

Inspect the current repository.

THEN

Determine the current phase.

THEN

Implement ONLY the next incomplete phase.

Do not jump ahead.

Do not implement future phases automatically.

Do not redesign completed phases without evidence.

33. AFTER EACH PHASE

The agent must report:

Phase:
Status:

Implemented:
-

Files added:
-

Files modified:
-

Tests:
-

Benchmark:
-

Important design decisions:
-

Known issues:
-

Next phase:
-

Then stop.

Do not continue automatically into the next major phase.

34. FINAL QUALITY PRINCIPLE

PulseKV should be:

technically meaningful
small enough to understand
fast enough to benchmark
safe enough to demonstrate concurrency
measurable
reproducible
honest

The project should never become a giant artificial "enterprise database."

The goal is not:

"I implemented Redis."

The goal is:

"I built a concurrent key-value store from scratch, measured its behavior, identified bottlenecks, improved its concurrency model, implemented persistence and eviction, and compared the results against established systems."

That is the story this project should support.

35. FINAL SUCCESS CRITERIA

The project is complete when:

 C++20 project builds cleanly
 TCP server works
 Client works
 SET works
 GET works
 DEL works
 EXISTS works
 malformed commands are handled
 multiple clients work
 thread pool works
 concurrent storage is safe
 TTL works
 LRU works
 WAL works
 recovery works
 automated tests pass
 sanitizers have been run where practical
 benchmark harness works
 baseline measurements exist
 concurrency comparisons exist
 bottleneck analysis exists
 external comparison exists if practical
 README exists
 BENCHMARKS.md exists
 INTERVIEW_GUIDE.md exists
FINAL PRINCIPLE

Build it incrementally.

Measure everything.

Keep the implementation simple.

Do not fake performance.

Do not copy existing implementations.

Do not implement features merely because they sound impressive.

Every optimization must have a reason.

Every benchmark number must come from an actual experiment.

Every component must be explainable in an interview.


## How you should use it with Antigravity

Don't paste the entire thing every time.

Put it in:

```text
PulseKV/
└── Implementation.md

Then your Antigravity instruction can simply be:

Read Implementation.md. Determine the current completed phase. Implement only the next phase. Run the required tests/benchmarks, update the relevant documentation, report what changed, and stop. Do not proceed to the next phase automatically.

That is the workflow I'd use.

And one important change from our earlier plan

I would not start with TTL, LRU, WAL, sharding, etc.

Start with:

Phase 0
Design
  ↓
Phase 1
HashMap
  ↓
Phase 2
Parser
  ↓
Phase 3
TCP
  ↓
Phase 4
Client
  ↓
Phase 5
BASELINE BENCHMARK
  ↓
Phase 6
Concurrency
  ↓
Phase 7
Thread Pool
  ↓
Phase 8
Synchronization
  ↓
Phase 9
BENCHMARK
  ↓
Phase 10
TTL
  ↓
Phase 11
LRU
  ↓
Phase 12
WAL
  ↓
Phase 13
Recovery
  ↓
Phase 14
Benchmark Harness
  ↓
Phase 15
External Comparison
  ↓
Phase 16+
Optimization only if measurements justify it

That benchmark-first approach is the part I'd emphasize most. It means when an interviewer asks "What did you actually learn/build?", you have a progression backed by experiments rather than a list of features generated by an agent.