# PulseKV — Durability Guarantees & Recovery Model (Phase 13)

## 1. Overview

PulseKV provides an append-only **Write-Ahead Log (WAL)** for state persistence across server restarts and crashes. Mutating commands (`SET`, `DEL`) are written to disk before/alongside modifying in-memory state.

---

## 2. Durability Guarantees

### What PulseKV Guarantees:
- **Sequential Replay Order:** On startup, the recovery engine reads the WAL sequentially from start to end and replays all mutations in the exact order they were logged.
- **Atomic Line Delimitation:** Every WAL record is terminated with `\r\n`.
- **Torn Write Resilience:** If the server or OS crashes mid-write resulting in a partial or incomplete final line, the recovery parser detects incomplete tokenization and safely skips the truncated record without crashing or aborting the replay of previously committed records.
- **Corrupted Line Tolerance:** If individual lines are corrupted (e.g. disk bad block or bad format), the parser logs a warning, skips the single bad record, and resumes replaying subsequent valid entries.
- **Multi-Session Continuity:** When the server restarts with an existing WAL, new writes are appended to the end of the existing log.

### What PulseKV Does NOT Guarantee (Documented Limitations):
- **Synchronous Disk Flush per Request:** PulseKV calls `file_.flush()` on every write, which commits the data to user-space CRT / OS file buffers. It does not invoke synchronous hardware disk sync (`FlushFileBuffers` / `fsync`) on every single client request by default, prioritizing high write throughput over strict ACID fsync guarantees.
- **Relative TTL Preservation:** When a key with a TTL is recovered, its remaining duration is reset to the original TTL relative to the *recovery time* (wall-clock timestamp offsets are not serialized).
- **Log Compaction / Snapshotting:** PulseKV currently uses an unbounded append-only log; log compaction / snapshotting is out of current scope.

---

## 3. Crash & Recovery Test Matrix

| Scenario | Behavior | Test Verification |
| :--- | :--- | :--- |
| **Clean Shutdown** | All buffered records flushed, file closed cleanly. | `test_clean_shutdown_restart()` |
| **Torn Final Write** | Partial record cut off mid-flight is safely discarded; all prior records recover. | `test_torn_write_incomplete_record()` |
| **Corrupted Record in Middle** | Malformed line is ignored; valid preceding and succeeding entries recover. | `test_corrupted_middle_record()` |
| **Multi-Crash Lifecycle** | Log persists across multiple consecutive crash/restart cycles without loss. | `test_multiple_crash_restart_cycles()` |
| **High Mutation Volume** | Thousands of rapid SET/DEL entries replay deterministically into memory. | `test_high_volume_recovery()` |
| **Empty / Whitespace Log** | Safe no-op clean start. | `test_empty_and_whitespace_wal()` |
