// straylight::nix::primitives::lru_cache
//
// High-performance LRU (Least Recently Used) cache primitive.
//
// Features:
//   - O(1) get, put, contains, erase operations
//   - Thread-safe variant (LRUCacheSafe) using std::shared_mutex
//   - Move-semantics friendly
//   - Iterator support for inspection
//   - Optional eviction callback
//   - Intrusive data structures (std::list + std::unordered_map)
//
// Usage:
//   LRUCache<std::string, int> cache(100);
//   cache.put("key", 42);
//   auto val = cache.get("key");  // std::optional<int>
//
// Thread-safe usage:
//   LRUCacheSafe<std::string, int> cache(100);
//   cache.put("key", 42);
//   auto val = cache.get("key");  // Returns by value, safe

#pragma once

#include <cstddef>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace straylight::nix::data {

// ─────────────────────────────────────────────────────────────────────────────
// LRUCache - Non-thread-safe LRU cache
// ─────────────────────────────────────────────────────────────────────────────

/// High-performance LRU cache with O(1) operations.
///
/// Implementation:
///   - std::unordered_map for O(1) key lookup
///   - std::list for O(1) LRU ordering (splice to front on access)
///   - List iterators stored in map for O(1) removal
///
/// @tparam Key    Key type (must be hashable)
/// @tparam Value  Value type (should be move-constructible)
/// @tparam Hash   Hash function for keys (default: std::hash<Key>)
/// @tparam KeyEq  Key equality predicate (default: std::equal_to<Key>)
template <typename Key, typename Value, typename Hash = std::hash<Key>,
          typename KeyEq = std::equal_to<Key>>
class LRUCache {
public:
  using key_type = Key;
  using mapped_type = Value;
  using value_type = std::pair<const Key, Value>;
  using size_type = std::size_t;
  using hasher = Hash;
  using key_equal = KeyEq;

  /// Callback type for eviction notification
  /// Called with (key, value) when an entry is evicted
  using eviction_callback = std::function<void(const Key&, Value&)>;

private:
  // Entry in the LRU list: stores key-value pair
  struct Entry {
    Key key;
    Value value;

    template <typename K, typename V>
    Entry(K&& k, V&& v) : key(std::forward<K>(k)), value(std::forward<V>(v)) {}
  };

  // LRU list: front = most recently used, back = least recently used
  using list_type = std::list<Entry>;
  using list_iterator = typename list_type::iterator;

  // Map: key -> iterator into list (for O(1) lookup and removal)
  using map_type = std::unordered_map<Key, list_iterator, Hash, KeyEq>;

  list_type list_;
  map_type map_;
  size_type capacity_;
  eviction_callback on_evict_;

  /// Move entry to front of LRU list (most recently used)
  void promote(list_iterator it) {
    // splice to front is O(1) and doesn't invalidate iterators
    list_.splice(list_.begin(), list_, it);
  }

  /// Evict the least recently used entry
  void evict_one() {
    if (list_.empty()) {
      return;
    }
    auto& entry = list_.back();
    if (on_evict_) {
      on_evict_(entry.key, entry.value);
    }
    map_.erase(entry.key);
    list_.pop_back();
  }

public:
  // ─────────────────────────────────────────────────────────────────────────
  // Constructors
  // ─────────────────────────────────────────────────────────────────────────

  /// Construct with maximum capacity
  explicit LRUCache(size_type capacity) : capacity_(capacity) { map_.reserve(capacity); }

  /// Construct with capacity and eviction callback
  LRUCache(size_type capacity, eviction_callback on_evict)
      : capacity_(capacity), on_evict_(std::move(on_evict)) {
    map_.reserve(capacity);
  }

  // Default move operations
  LRUCache(LRUCache&&) noexcept = default;
  LRUCache& operator=(LRUCache&&) noexcept = default;

  // Delete copy operations (iterators would be invalidated)
  LRUCache(const LRUCache&) = delete;
  LRUCache& operator=(const LRUCache&) = delete;

  // ─────────────────────────────────────────────────────────────────────────
  // Core operations
  // ─────────────────────────────────────────────────────────────────────────

  /// Insert or update a key-value pair.
  /// If key exists, updates value and promotes to front.
  /// If cache is full, evicts least recently used entry.
  ///
  /// @return true if new entry was inserted, false if existing was updated
  template <typename K, typename V>
  bool put(K&& key, V&& value) {
    if (capacity_ == 0) {
      return false;
    }

    auto it = map_.find(key);
    if (it != map_.end()) {
      // Update existing entry
      it->second->value = std::forward<V>(value);
      promote(it->second);
      return false;
    }

    // Make room if needed - evict one entry when at capacity
    while (list_.size() >= capacity_) {
      evict_one();
    }

    // Insert new entry at front
    list_.emplace_front(std::forward<K>(key), std::forward<V>(value));
    map_.emplace(list_.front().key, list_.begin());
    return true;
  }

  /// Get value for key, promoting to front if found.
  ///
  /// @return std::optional containing value copy if found, std::nullopt otherwise
  template <typename K>
  [[nodiscard]] std::optional<Value> get(const K& key) {
    auto it = map_.find(key);
    if (it == map_.end()) {
      return std::nullopt;
    }
    promote(it->second);
    return it->second->value;
  }

  /// Get pointer to value for key, promoting to front if found.
  /// The pointer is valid until the entry is evicted or erased.
  ///
  /// @return Pointer to value if found, nullptr otherwise
  template <typename K>
  [[nodiscard]] Value* get_ptr(const K& key) {
    auto it = map_.find(key);
    if (it == map_.end()) {
      return nullptr;
    }
    promote(it->second);
    return &it->second->value;
  }

  /// Get const pointer to value for key, promoting to front if found.
  template <typename K>
  [[nodiscard]] const Value* get_ptr(const K& key) const {
    auto it = map_.find(key);
    if (it == map_.end()) {
      return nullptr;
    }
    // Note: const version still promotes (mutable list_)
    const_cast<LRUCache*>(this)->promote(it->second);
    return &it->second->value;
  }

  /// Peek at value without promoting (doesn't affect LRU order).
  ///
  /// @return std::optional containing value copy if found
  template <typename K>
  [[nodiscard]] std::optional<Value> peek(const K& key) const {
    auto it = map_.find(key);
    if (it == map_.end()) {
      return std::nullopt;
    }
    return it->second->value;
  }

  /// Check if key exists in cache (without promoting).
  template <typename K>
  [[nodiscard]] bool contains(const K& key) const {
    return map_.find(key) != map_.end();
  }

  /// Erase entry by key.
  ///
  /// @return true if entry was found and erased
  template <typename K>
  bool erase(const K& key) {
    auto it = map_.find(key);
    if (it == map_.end()) {
      return false;
    }
    list_.erase(it->second);
    map_.erase(it);
    return true;
  }

  /// Get or insert: returns existing value or inserts new one.
  ///
  /// @param key    The key to look up
  /// @param value  Value to insert if key doesn't exist
  /// @return Reference to the value (existing or newly inserted)
  template <typename K, typename V>
  Value& get_or_put(K&& key, V&& value) {
    auto it = map_.find(key);
    if (it != map_.end()) {
      promote(it->second);
      return it->second->value;
    }

    if (capacity_ == 0) {
      // Edge case: zero capacity, can't store anything
      // Return reference to temporary (UB if used, but capacity 0 is unusual)
      static Value dummy{};
      return dummy;
    }

    while (list_.size() >= capacity_) {
      evict_one();
    }

    list_.emplace_front(std::forward<K>(key), std::forward<V>(value));
    map_.emplace(list_.front().key, list_.begin());
    return list_.front().value;
  }

  /// Try to emplace a new entry.
  /// Does nothing if key already exists.
  ///
  /// @return pair of (reference to value, was_inserted)
  template <typename K, typename... Args>
  std::pair<Value&, bool> try_emplace(K&& key, Args&&... args) {
    auto it = map_.find(key);
    if (it != map_.end()) {
      promote(it->second);
      return {it->second->value, false};
    }

    if (capacity_ == 0) {
      static Value dummy{};
      return {dummy, false};
    }

    while (list_.size() >= capacity_) {
      evict_one();
    }

    list_.emplace_front(std::forward<K>(key), Value(std::forward<Args>(args)...));
    map_.emplace(list_.front().key, list_.begin());
    return {list_.front().value, true};
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Capacity
  // ─────────────────────────────────────────────────────────────────────────

  /// Current number of entries
  [[nodiscard]] size_type size() const noexcept { return list_.size(); }

  /// Maximum capacity
  [[nodiscard]] size_type capacity() const noexcept { return capacity_; }

  /// Check if cache is empty
  [[nodiscard]] bool empty() const noexcept { return list_.empty(); }

  /// Check if cache is at capacity
  [[nodiscard]] bool full() const noexcept { return list_.size() >= capacity_; }

  /// Clear all entries (calls eviction callback for each)
  void clear() {
    if (on_evict_) {
      for (auto& entry : list_) {
        on_evict_(entry.key, entry.value);
      }
    }
    list_.clear();
    map_.clear();
  }

  /// Resize capacity. If new capacity is smaller, evicts LRU entries.
  void resize(size_type new_capacity) {
    capacity_ = new_capacity;
    // Evict entries until we're at or below new capacity
    while (list_.size() > capacity_) {
      evict_one();
    }
    map_.reserve(new_capacity);
  }

  /// Set eviction callback
  void set_eviction_callback(eviction_callback cb) { on_evict_ = std::move(cb); }

  // ─────────────────────────────────────────────────────────────────────────
  // Iterators (for inspection, not modification)
  //
  // Iteration order: most recently used -> least recently used
  // ─────────────────────────────────────────────────────────────────────────

  class const_iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = std::pair<const Key&, const Value&>;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type*;
    using reference = value_type;

  private:
    typename list_type::const_iterator it_;

  public:
    explicit const_iterator(typename list_type::const_iterator it) : it_(it) {}

    reference operator*() const { return {it_->key, it_->value}; }

    const_iterator& operator++() {
      ++it_;
      return *this;
    }

    const_iterator operator++(int) {
      const_iterator tmp = *this;
      ++it_;
      return tmp;
    }

    bool operator==(const const_iterator& other) const { return it_ == other.it_; }
    bool operator!=(const const_iterator& other) const { return it_ != other.it_; }
  };

  /// Begin iterator (most recently used)
  [[nodiscard]] const_iterator begin() const { return const_iterator(list_.begin()); }

  /// End iterator
  [[nodiscard]] const_iterator end() const { return const_iterator(list_.end()); }

  /// Begin iterator (most recently used)
  [[nodiscard]] const_iterator cbegin() const { return begin(); }

  /// End iterator
  [[nodiscard]] const_iterator cend() const { return end(); }

  // ─────────────────────────────────────────────────────────────────────────
  // Statistics (for debugging/monitoring)
  // ─────────────────────────────────────────────────────────────────────────

  /// Get the most recently used key-value pair
  [[nodiscard]] std::optional<std::pair<Key, Value>> front() const {
    if (list_.empty()) {
      return std::nullopt;
    }
    return std::make_pair(list_.front().key, list_.front().value);
  }

  /// Get the least recently used key-value pair
  [[nodiscard]] std::optional<std::pair<Key, Value>> back() const {
    if (list_.empty()) {
      return std::nullopt;
    }
    return std::make_pair(list_.back().key, list_.back().value);
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// LRUCacheSafe - Thread-safe LRU cache using std::shared_mutex
// ─────────────────────────────────────────────────────────────────────────────

/// Thread-safe LRU cache wrapper.
///
/// Uses std::shared_mutex for read-write locking:
///   - Multiple concurrent readers (shared lock)
///   - Exclusive access for writers (unique lock)
///
/// Note: get() returns by value (copy) for thread safety.
/// For high-performance scenarios with large values, consider using
/// the non-thread-safe version with external synchronization.
///
/// @tparam Key    Key type (must be hashable and copyable)
/// @tparam Value  Value type (must be copyable)
/// @tparam Hash   Hash function for keys
/// @tparam KeyEq  Key equality predicate
template <typename Key, typename Value, typename Hash = std::hash<Key>,
          typename KeyEq = std::equal_to<Key>>
class LRUCacheSafe {
public:
  using key_type = Key;
  using mapped_type = Value;
  using size_type = std::size_t;
  using eviction_callback = typename LRUCache<Key, Value, Hash, KeyEq>::eviction_callback;

private:
  mutable std::shared_mutex mutex_;
  LRUCache<Key, Value, Hash, KeyEq> cache_;

public:
  // ─────────────────────────────────────────────────────────────────────────
  // Constructors
  // ─────────────────────────────────────────────────────────────────────────

  explicit LRUCacheSafe(size_type capacity) : cache_(capacity) {}

  LRUCacheSafe(size_type capacity, eviction_callback on_evict)
      : cache_(capacity, std::move(on_evict)) {}

  // Non-copyable, non-movable (due to mutex)
  LRUCacheSafe(const LRUCacheSafe&) = delete;
  LRUCacheSafe& operator=(const LRUCacheSafe&) = delete;
  LRUCacheSafe(LRUCacheSafe&&) = delete;
  LRUCacheSafe& operator=(LRUCacheSafe&&) = delete;

  // ─────────────────────────────────────────────────────────────────────────
  // Core operations
  // ─────────────────────────────────────────────────────────────────────────

  /// Insert or update a key-value pair (exclusive lock).
  template <typename K, typename V>
  bool put(K&& key, V&& value) {
    std::unique_lock lock(mutex_);
    return cache_.put(std::forward<K>(key), std::forward<V>(value));
  }

  /// Get value for key (exclusive lock due to LRU promotion).
  /// Returns copy for thread safety.
  template <typename K>
  [[nodiscard]] std::optional<Value> get(const K& key) {
    std::unique_lock lock(mutex_);
    return cache_.get(key);
  }

  /// Peek at value without promoting (shared lock).
  template <typename K>
  [[nodiscard]] std::optional<Value> peek(const K& key) const {
    std::shared_lock lock(mutex_);
    return cache_.peek(key);
  }

  /// Check if key exists (shared lock).
  template <typename K>
  [[nodiscard]] bool contains(const K& key) const {
    std::shared_lock lock(mutex_);
    return cache_.contains(key);
  }

  /// Erase entry by key (exclusive lock).
  template <typename K>
  bool erase(const K& key) {
    std::unique_lock lock(mutex_);
    return cache_.erase(key);
  }

  /// Get or insert (exclusive lock).
  /// Returns copy for thread safety.
  template <typename K, typename V>
  Value get_or_put(K&& key, V&& value) {
    std::unique_lock lock(mutex_);
    return cache_.get_or_put(std::forward<K>(key), std::forward<V>(value));
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Capacity (shared lock for reads)
  // ─────────────────────────────────────────────────────────────────────────

  [[nodiscard]] size_type size() const {
    std::shared_lock lock(mutex_);
    return cache_.size();
  }

  [[nodiscard]] size_type capacity() const {
    std::shared_lock lock(mutex_);
    return cache_.capacity();
  }

  [[nodiscard]] bool empty() const {
    std::shared_lock lock(mutex_);
    return cache_.empty();
  }

  [[nodiscard]] bool full() const {
    std::shared_lock lock(mutex_);
    return cache_.full();
  }

  void clear() {
    std::unique_lock lock(mutex_);
    cache_.clear();
  }

  void resize(size_type new_capacity) {
    std::unique_lock lock(mutex_);
    cache_.resize(new_capacity);
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Batch operations (single lock for multiple operations)
  // ─────────────────────────────────────────────────────────────────────────

  /// Execute a function with exclusive access to the underlying cache.
  /// Useful for batch operations or custom logic.
  ///
  /// @param fn Function taking LRUCache<Key, Value, Hash, KeyEq>&
  template <typename Fn>
  auto with_lock(Fn&& fn) -> decltype(fn(std::declval<LRUCache<Key, Value, Hash, KeyEq>&>())) {
    std::unique_lock lock(mutex_);
    return fn(cache_);
  }

  /// Execute a function with shared access to the underlying cache.
  template <typename Fn>
  auto with_shared_lock(Fn&& fn) const
      -> decltype(fn(std::declval<const LRUCache<Key, Value, Hash, KeyEq>&>())) {
    std::shared_lock lock(mutex_);
    return fn(cache_);
  }
};

} // namespace straylight::nix::data
