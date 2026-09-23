#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

namespace pulsekv {

// ---------------------------------------------------------------------------
// WALWriter — Phase 12: Append-only Write-Ahead Log
//
// Records every mutating operation (SET, DEL) to a plain-text log file
// BEFORE the operation is applied to the in-memory store.
//
// On server restart, WALReader replays the log to reconstruct state
// (see wal_recover() in wal.hpp).
//
// Record format (one per line, \r\n terminated):
//
//   SET <key> <value> <ttl_secs>\r\n   — ttl_secs=0 means no expiry
//   DEL <key>\r\n
//
// Thread-safety: WALWriter is safe for concurrent callers (internal mutex).
//
// Durability model:
//   By default each record is written and flushed to the OS buffer, but NOT
//   fsync'd.  This provides protection against process crashes but NOT against
//   power loss.  Call sync() explicitly if you need fsync guarantees.
// ---------------------------------------------------------------------------

class WALWriter {
public:
    // Open (or create) the WAL file at path. Throws std::runtime_error on failure.
    explicit WALWriter(const std::string& path);

    ~WALWriter();

    // Non-copyable, non-movable — owns the file handle.
    WALWriter(const WALWriter&)            = delete;
    WALWriter& operator=(const WALWriter&) = delete;
    WALWriter(WALWriter&&)                 = delete;
    WALWriter& operator=(WALWriter&&)      = delete;

    // Append a SET record to the log. ttl_seconds == 0 means no TTL.
    void log_set(const std::string& key, const std::string& value, long long ttl_seconds);

    // Append a DEL record to the log.
    void log_del(const std::string& key);

    // Flush the write buffer to the OS (does not guarantee disk durability).
    void flush();

    // Flush + fsync: guarantees the record reaches durable storage.
    // Expensive — only call when strong durability is required.
    void sync();

    // Return the file path this WALWriter is writing to.
    [[nodiscard]] const std::string& path() const noexcept;

    // Return total records written since construction.
    [[nodiscard]] std::size_t records_written() const noexcept;

private:
    std::string   path_;
    std::ofstream file_;
    std::mutex    mutex_;
    std::size_t   records_written_ = 0;
};

// ---------------------------------------------------------------------------
// wal_recover — replay a WAL file into a Store.
//
// Reads the WAL file at path line by line and re-executes each SET/DEL
// operation against the given store.  Records with unrecognised format are
// skipped and a warning is printed to stderr (best-effort recovery).
//
// Returns the number of records successfully replayed.
// Returns 0 without error if the file does not exist (clean start).
// ---------------------------------------------------------------------------

class Store;   // forward declaration

std::size_t wal_recover(const std::string& path, Store& store);

} // namespace pulsekv
