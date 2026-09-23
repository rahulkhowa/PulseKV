#include "pulsekv/storage/store.hpp"

namespace pulsekv {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

Store::Store(LockStrategy strategy, bool enable_background_cleanup, std::size_t max_capacity)
    : strategy_(strategy), max_capacity_(max_capacity) {
    if (enable_background_cleanup) {
        cleanup_thread_ = std::thread(&Store::cleanup_worker, this);
    }
}

Store::~Store() {
    stop_background_cleanup();
}

// ---------------------------------------------------------------------------
// Property accessors
// ---------------------------------------------------------------------------

void Store::set_lock_strategy(LockStrategy strategy) noexcept { strategy_ = strategy; }
LockStrategy Store::lock_strategy()  const noexcept { return strategy_; }
std::size_t  Store::max_capacity()   const noexcept { return max_capacity_; }
std::size_t  Store::evicted_count()  const noexcept {
    return evicted_count_.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Background cleanup
// ---------------------------------------------------------------------------

void Store::stop_background_cleanup() {
    {
        std::lock_guard<std::mutex> lock(cleanup_mutex_);
        if (stop_cleanup_) return;
        stop_cleanup_ = true;
    }
    cleanup_cv_.notify_all();
    if (cleanup_thread_.joinable()) cleanup_thread_.join();
}

void Store::cleanup_worker() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(cleanup_mutex_);
            cleanup_cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
                return stop_cleanup_.load();
            });
            if (stop_cleanup_) return;
        }
        purge_expired(200);
    }
}

// ---------------------------------------------------------------------------
// LRU helpers  (caller must hold exclusive rw_mutex_)
// ---------------------------------------------------------------------------

// Splice the entry's list node to the front (MRU) without invalidating its iterator.
void Store::touch_locked(std::unordered_map<std::string, TableEntry>::iterator it) const {
    lru_order_.splice(lru_order_.begin(), lru_order_, it->second.pos);
    // After splice the iterator is still valid and points to lru_order_.begin().
}

// Evict the LRU (tail) entry.
void Store::evict_lru_locked() const {
    while (!lru_order_.empty()) {
        const std::string& lru_key = lru_order_.back();
        auto it = table_.find(lru_key);
        if (it != table_.end()) {
            table_.erase(it);
            lru_order_.pop_back();
            evicted_count_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Key was already removed by expiry — discard stale list node.
        lru_order_.pop_back();
    }
}

// ---------------------------------------------------------------------------
// set
// ---------------------------------------------------------------------------

void Store::set(std::string key, std::string value, long long ttl_seconds) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    auto it = table_.find(key);
    if (it != table_.end()) {
        // Existing key: update in-place, move to MRU.
        it->second.entry.value = std::move(value);
        if (ttl_seconds > 0)
            it->second.entry.expiry = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(ttl_seconds);
        else
            it->second.entry.expiry.reset();
        lru_order_.splice(lru_order_.begin(), lru_order_, it->second.pos);
        return;
    }

    // New key: evict LRU if at capacity.
    if (max_capacity_ > 0 && table_.size() >= max_capacity_) {
        evict_lru_locked();
    }

    // Insert at MRU (front of list). list::push_front copies the key string.
    lru_order_.push_front(key);

    Entry e;
    e.value = std::move(value);
    if (ttl_seconds > 0)
        e.expiry = std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds);

    // table_ key is an independent copy of key (key string may have been moved by caller).
    table_.emplace(std::move(key), TableEntry{std::move(e), lru_order_.begin()});
}

void Store::set_with_expiry(std::string key, std::string value,
                             std::chrono::steady_clock::time_point expiry) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    auto it = table_.find(key);
    if (it != table_.end()) {
        it->second.entry.value  = std::move(value);
        it->second.entry.expiry = expiry;
        lru_order_.splice(lru_order_.begin(), lru_order_, it->second.pos);
        return;
    }

    if (max_capacity_ > 0 && table_.size() >= max_capacity_) {
        evict_lru_locked();
    }

    lru_order_.push_front(key);
    Entry e;
    e.value  = std::move(value);
    e.expiry = expiry;
    table_.emplace(std::move(key), TableEntry{std::move(e), lru_order_.begin()});
}

// ---------------------------------------------------------------------------
// get
// ---------------------------------------------------------------------------

std::optional<std::string> Store::get(const std::string& key) const {
    // When LRU is active every GET is a write (list mutation), so we always
    // need an exclusive lock.  When LRU is off and strategy == SHARED_MUTEX
    // we preserve the fast shared-lock path for the common (non-expired) case.

    if (max_capacity_ == 0 && strategy_ == LockStrategy::SHARED_MUTEX) {
        // ----- SHARED_MUTEX fast path (no LRU) -----
        {
            std::shared_lock<std::shared_mutex> sl(rw_mutex_);
            auto it = table_.find(key);
            if (it == table_.end()) return std::nullopt;
            if (!it->second.entry.is_expired()) return it->second.entry.value;
        }
        // Expired: upgrade to exclusive lock for lazy erasure.
        std::unique_lock<std::shared_mutex> ul(rw_mutex_);
        auto it = table_.find(key);
        if (it == table_.end()) return std::nullopt;
        if (it->second.entry.is_expired()) {
            lru_order_.erase(it->second.pos);
            table_.erase(it);
            return std::nullopt;
        }
        return it->second.entry.value;
    }

    // ----- Exclusive path (LRU active or MUTEX strategy) -----
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = table_.find(key);
    if (it == table_.end()) return std::nullopt;
    if (it->second.entry.is_expired()) {
        lru_order_.erase(it->second.pos);
        table_.erase(it);
        return std::nullopt;
    }
    // Move to MRU only when LRU is active.
    if (max_capacity_ > 0) {
        lru_order_.splice(lru_order_.begin(), lru_order_, it->second.pos);
    }
    return it->second.entry.value;
}

// ---------------------------------------------------------------------------
// del
// ---------------------------------------------------------------------------

bool Store::del(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = table_.find(key);
    if (it == table_.end()) return false;
    lru_order_.erase(it->second.pos);
    table_.erase(it);
    return true;
}

// ---------------------------------------------------------------------------
// exists  (does NOT update LRU order — matches Redis semantics)
// ---------------------------------------------------------------------------

bool Store::exists(const std::string& key) const {
    if (strategy_ == LockStrategy::SHARED_MUTEX) {
        {
            std::shared_lock<std::shared_mutex> lock(rw_mutex_);
            auto it = table_.find(key);
            if (it == table_.end()) return false;
            if (!it->second.entry.is_expired()) return true;
        }
        // Expired: upgrade to exclusive for erasure.
        std::unique_lock<std::shared_mutex> lock(rw_mutex_);
        auto it = table_.find(key);
        if (it != table_.end() && it->second.entry.is_expired()) {
            lru_order_.erase(it->second.pos);
            table_.erase(it);
            return false;
        }
        return it != table_.end();
    } else {
        std::unique_lock<std::shared_mutex> lock(rw_mutex_);
        auto it = table_.find(key);
        if (it == table_.end()) return false;
        if (it->second.entry.is_expired()) {
            lru_order_.erase(it->second.pos);
            table_.erase(it);
            return false;
        }
        return true;
    }
}

// ---------------------------------------------------------------------------
// size
// ---------------------------------------------------------------------------

std::size_t Store::size() const noexcept {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::size_t count = 0;
    auto now = std::chrono::steady_clock::now();
    for (const auto& [k, te] : table_) {
        if (!te.entry.expiry.has_value() || te.entry.expiry.value() > now) {
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// purge_expired
// ---------------------------------------------------------------------------

std::size_t Store::purge_expired(std::size_t max_keys) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    std::size_t purged = 0;
    auto now = std::chrono::steady_clock::now();

    for (auto it = table_.begin(); it != table_.end(); ) {
        if (it->second.entry.expiry.has_value() && now >= it->second.entry.expiry.value()) {
            lru_order_.erase(it->second.pos);
            it = table_.erase(it);
            ++purged;
            if (max_keys > 0 && purged >= max_keys) break;
        } else {
            ++it;
        }
    }
    return purged;
}

// ---------------------------------------------------------------------------
// expire
// ---------------------------------------------------------------------------

bool Store::expire(const std::string& key, long long ttl_seconds) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = table_.find(key);
    if (it == table_.end()) return false;
    if (it->second.entry.is_expired()) {
        lru_order_.erase(it->second.pos);
        table_.erase(it);
        return false;
    }
    if (ttl_seconds > 0)
        it->second.entry.expiry = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(ttl_seconds);
    else
        it->second.entry.expiry.reset();
    return true;
}

// ---------------------------------------------------------------------------
// ttl
// ---------------------------------------------------------------------------

long long Store::ttl(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = table_.find(key);
    if (it == table_.end()) return -2LL;
    if (it->second.entry.is_expired())  return -2LL;
    if (!it->second.entry.expiry.has_value()) return -1LL;
    auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        it->second.entry.expiry.value() - std::chrono::steady_clock::now()).count();
    return (remaining < 0) ? -2LL : remaining;
}

} // namespace pulsekv
