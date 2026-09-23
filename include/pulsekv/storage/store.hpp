#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <list>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace pulsekv {

// ---------------------------------------------------------------------------
enum class LockStrategy {
    MUTEX,          // Exclusive lock for all operations (Version B)
    SHARED_MUTEX    // Shared lock for reads, exclusive for writes (Version C)
};

// ---------------------------------------------------------------------------
// Store — Phase 11: Thread-safe in-memory key-value store with TTL + LRU
//
// LRU Eviction (enabled when max_capacity > 0):
//
//   Data structure:
//     lru_order_  : std::list<std::string>
//                   front = Most Recently Used, back = Least Recently Used
//     table_      : unordered_map<key, TableEntry{Entry, list::iterator}>
//                   O(1) lookup + O(1) splice-to-front on GET via cached iterator
//
//   Complexity (expected/average):
//     GET     O(1)  — hash lookup + list::splice to front
//     SET     O(1)  — hash insert/update + list::push_front; evict tail if full
//     DEL     O(1)  — hash erase + list erase via cached iterator
//     EVICT   O(1)  — pop_back from list + hash erase
//
//   Note: GET requires an exclusive lock when LRU is active (list mutation).
//   The SHARED_MUTEX read optimisation is preserved only when max_capacity==0.
// ---------------------------------------------------------------------------

class Store {
public:
    // max_capacity == 0  →  unlimited (LRU eviction disabled).
    explicit Store(LockStrategy strategy             = LockStrategy::SHARED_MUTEX,
                   bool         enable_background_cleanup = true,
                   std::size_t  max_capacity         = 0);

    // Non-copyable, non-movable.
    Store(const Store&)            = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&&)                 = delete;
    Store& operator=(Store&&)      = delete;

    ~Store();

    void set_lock_strategy(LockStrategy strategy) noexcept;
    [[nodiscard]] LockStrategy lock_strategy()  const noexcept;
    [[nodiscard]] std::size_t  max_capacity()   const noexcept;
    [[nodiscard]] std::size_t  evicted_count()  const noexcept;

    // Store key → value with optional TTL in seconds (0 = no expiry).
    // If max_capacity > 0 and the store is full, the LRU entry is evicted first.
    void set(std::string key, std::string value, long long ttl_seconds = 0);

    // Store key → value with an explicit expiry time point.
    void set_with_expiry(std::string key, std::string value,
                         std::chrono::steady_clock::time_point expiry);

    // Retrieve value for key.
    //   - Lazily expires the entry if its TTL has passed.
    //   - Moves the key to the MRU position when LRU is active.
    // Declared const but may mutate mutable lru_order_ / expired entries.
    [[nodiscard]] std::optional<std::string> get(const std::string& key) const;

    // Delete a key. Returns true if removed (and not already expired).
    bool del(const std::string& key);

    // Check whether key exists and is not expired. Does NOT update LRU order.
    [[nodiscard]] bool exists(const std::string& key) const;

    // Return count of non-expired keys.
    [[nodiscard]] std::size_t size() const noexcept;

    // Scan and purge up to max_keys expired entries (0 = all). Returns count purged.
    std::size_t purge_expired(std::size_t max_keys = 0);

    // Stop the background cleanup thread.
    void stop_background_cleanup();

    // Set or clear TTL on an existing key.
    //   ttl_seconds > 0  →  apply TTL
    //   ttl_seconds == 0 →  remove TTL (make persistent)
    // Returns true if key existed and was not expired.
    bool expire(const std::string& key, long long ttl_seconds);

    // Return remaining TTL in seconds.
    //   >= 0   : seconds until expiry
    //   -1     : key exists, no TTL (persistent)
    //   -2     : key not found or already expired
    [[nodiscard]] long long ttl(const std::string& key) const;

private:
    // ---- Internal types ------------------------------------------------

    struct Entry {
        std::string value;
        std::optional<std::chrono::steady_clock::time_point> expiry;

        [[nodiscard]] bool is_expired() const noexcept {
            if (!expiry.has_value()) return false;
            return std::chrono::steady_clock::now() >= expiry.value();
        }
    };

    // Table value: Entry bundled with its position in lru_order_.
    // Keeping the iterator in the map enables O(1) list::splice on access.
    struct TableEntry {
        Entry                            entry;
        std::list<std::string>::iterator pos;   // position in lru_order_
    };

    // ---- LRU helpers (caller must hold exclusive rw_mutex_) --------

    // Splice key to the front (MRU) of lru_order_. Iterator remains valid.
    void touch_locked(std::unordered_map<std::string, TableEntry>::iterator it) const;

    // Evict the tail (LRU) entry. Noop if lru_order_ is empty.
    void evict_lru_locked() const;

    // ---- Background cleanup ----------------------------------------
    void cleanup_worker();

    // ---- Members ---------------------------------------------------

    LockStrategy strategy_     = LockStrategy::SHARED_MUTEX;
    std::size_t  max_capacity_ = 0;   // 0 = unlimited

    mutable std::shared_mutex                            rw_mutex_;
    mutable std::unordered_map<std::string, TableEntry>  table_;
    mutable std::list<std::string>                       lru_order_;
    mutable std::atomic<std::size_t>                     evicted_count_{0};

    // Background cleanup
    std::atomic<bool>       stop_cleanup_{false};
    std::mutex              cleanup_mutex_;
    std::condition_variable cleanup_cv_;
    std::thread             cleanup_thread_;
};

} // namespace pulsekv
