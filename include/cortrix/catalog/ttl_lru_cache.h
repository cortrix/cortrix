#pragma once
#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <unordered_map>
#include <utility>

namespace cortrix::catalog {

/// Bounded LRU cache with per-entry TTL (F12 §6.3: NS metadata 10K/60s, Unit
/// descriptor 1K/60s). Lookups expire stale entries lazily; inserts evict the
/// least-recently-used entry once capacity is exceeded.
///
/// Time is injected as an int64 millisecond clock so tests are deterministic
/// (no wall-clock dependency). Not thread-safe — DefaultINSRouter holds it under
/// its own mutex.
template <typename K, typename V>
class TtlLruCache {
public:
    /// `capacity` max live entries (0 = disabled, never stores). `ttl_ms` entry
    /// lifetime; a lookup at or after insert_time + ttl_ms misses and evicts.
    TtlLruCache(std::size_t capacity, int64_t ttl_ms)
        : capacity_(capacity), ttl_ms_(ttl_ms) {}

    /// Look up `key` at logical time `now_ms`. Returns the value (and promotes it
    /// to most-recently-used) if present and not expired; std::nullopt otherwise.
    std::optional<V> Get(const K& key, int64_t now_ms) {
        auto it = index_.find(key);
        if (it == index_.end()) return std::nullopt;
        Node& node = *it->second;
        if (Expired(node, now_ms)) {
            Erase(it);
            return std::nullopt;
        }
        Touch(it->second);
        return node.value;
    }

    /// Insert or overwrite `key`→`value` stamped at `now_ms`. Evicts LRU entries
    /// while over capacity. A zero-capacity cache stores nothing.
    void Put(const K& key, V value, int64_t now_ms) {
        if (capacity_ == 0) return;
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->value = std::move(value);
            it->second->inserted_ms = now_ms;
            Touch(it->second);
            return;
        }
        order_.push_front(Node{key, std::move(value), now_ms});
        index_[key] = order_.begin();
        while (index_.size() > capacity_) {
            EvictLru();
        }
    }

    /// Drop `key` if present (e.g. on namespace update/delete).
    void Invalidate(const K& key) {
        auto it = index_.find(key);
        if (it != index_.end()) Erase(it);
    }

    /// Drop everything.
    void Clear() {
        order_.clear();
        index_.clear();
    }

    /// Current number of stored entries (expired-but-not-yet-evicted included).
    std::size_t size() const { return index_.size(); }
    std::size_t capacity() const { return capacity_; }
    int64_t ttl_ms() const { return ttl_ms_; }

private:
    struct Node {
        K key;
        V value;
        int64_t inserted_ms;
    };
    using ListIt = typename std::list<Node>::iterator;

    bool Expired(const Node& node, int64_t now_ms) const {
        return now_ms - node.inserted_ms >= ttl_ms_;
    }

    // Move the node to the front (most-recently-used).
    void Touch(ListIt node_it) {
        order_.splice(order_.begin(), order_, node_it);
    }

    void EvictLru() {
        if (order_.empty()) return;
        index_.erase(order_.back().key);
        order_.pop_back();
    }

    void Erase(typename std::unordered_map<K, ListIt>::iterator index_it) {
        order_.erase(index_it->second);
        index_.erase(index_it);
    }

    std::size_t capacity_;
    int64_t ttl_ms_;
    std::list<Node> order_;                      // front = MRU, back = LRU
    std::unordered_map<K, ListIt> index_;
};

}  // namespace cortrix::catalog
