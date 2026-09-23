#include "pulsekv/persistence/wal.hpp"
#include "pulsekv/storage/store.hpp"

#include <charconv>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#  include <io.h>      // _get_osfhandle
#  include <windows.h> // FlushFileBuffers
#else
#  include <unistd.h>  // fsync
#endif

namespace pulsekv {

// ---------------------------------------------------------------------------
// WALWriter — construction / destruction
// ---------------------------------------------------------------------------

WALWriter::WALWriter(const std::string& path)
    : path_(path) {
    // Open in append mode so existing records are preserved across restarts.
    file_.open(path_, std::ios::app | std::ios::binary);
    if (!file_.is_open()) {
        throw std::runtime_error("[wal] Failed to open WAL file: " + path_);
    }
}

WALWriter::~WALWriter() {
    try {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
    } catch (...) {}
}

// ---------------------------------------------------------------------------
// log_set
// ---------------------------------------------------------------------------

void WALWriter::log_set(const std::string& key, const std::string& value,
                        long long ttl_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Format: SET <key> <value> <ttl_secs>\r\n
    file_ << "SET " << key << ' ' << value << ' ' << ttl_seconds << "\r\n";
    file_.flush();   // OS-level flush; not a full fsync
    ++records_written_;
}

// ---------------------------------------------------------------------------
// log_del
// ---------------------------------------------------------------------------

void WALWriter::log_del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Format: DEL <key>\r\n
    file_ << "DEL " << key << "\r\n";
    file_.flush();
    ++records_written_;
}

// ---------------------------------------------------------------------------
// flush / sync
// ---------------------------------------------------------------------------

void WALWriter::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.flush();
}

void WALWriter::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.flush();
    // Best-effort durability: flush the CRT buffers to the kernel.
    // Full fsync (FlushFileBuffers) would require extracting the raw HANDLE
    // from std::ofstream, which is non-portable. Documented limitation.
    _flushall();
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

const std::string& WALWriter::path() const noexcept { return path_; }

std::size_t WALWriter::records_written() const noexcept { return records_written_; }

// ---------------------------------------------------------------------------
// wal_recover — replay WAL into Store
// ---------------------------------------------------------------------------

std::size_t wal_recover(const std::string& path, Store& store) {
    std::ifstream file(path);
    if (!file.is_open()) {
        // No WAL file yet — clean start, not an error.
        return 0;
    }

    std::size_t replayed = 0;
    std::size_t skipped  = 0;
    std::string line;

    while (std::getline(file, line)) {
        // Strip trailing \r if present (CRLF records on Windows).
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        // Tokenise on whitespace.
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "SET") {
            std::string key, value;
            long long ttl_secs = 0;
            if (!(iss >> key >> value >> ttl_secs)) {
                std::cerr << "[wal_recover] Skipping malformed SET record: "
                          << line << "\n";
                ++skipped;
                continue;
            }
            store.set(std::move(key), std::move(value), ttl_secs);
            ++replayed;

        } else if (cmd == "DEL") {
            std::string key;
            if (!(iss >> key)) {
                std::cerr << "[wal_recover] Skipping malformed DEL record: "
                          << line << "\n";
                ++skipped;
                continue;
            }
            store.del(key);
            ++replayed;

        } else {
            std::cerr << "[wal_recover] Unknown record type '" << cmd
                      << "' — skipping.\n";
            ++skipped;
        }
    }

    std::cout << "[wal_recover] Replayed " << replayed << " records";
    if (skipped > 0) std::cout << " (" << skipped << " skipped/malformed)";
    std::cout << " from " << path << "\n";

    return replayed;
}

} // namespace pulsekv
