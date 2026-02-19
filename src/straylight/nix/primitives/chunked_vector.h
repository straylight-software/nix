// straylight::nix::primitives::ChunkedVector
//
// A chunked vector that provides stable references to elements.
// Unlike std::vector, pointers and references to elements remain valid
// after push_back/emplace_back operations. This is achieved by storing
// elements in fixed-size chunks that are never reallocated.
//
// Key properties:
// - O(1) amortized push_back/emplace_back
// - O(1) indexed access
// - Stable references: pointers/references never invalidate on growth
// - Random access iterator support
// - Memory overhead proportional to chunk count, not element count

#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace straylight::nix::primitives {

/// Default chunk size optimized for cache efficiency.
/// 8192 elements per chunk provides good balance between:
/// - Memory overhead (one pointer per chunk)
/// - Allocation frequency
/// - Cache locality within chunks
inline constexpr std::size_t kDefaultChunkSize = 8192;

/// A vector-like container with stable element references.
///
/// Elements are stored in fixed-size chunks. When the current chunk is full,
/// a new chunk is allocated. Existing chunks are never reallocated or moved,
/// guaranteeing that pointers and references to elements remain valid for
/// the lifetime of the container.
///
/// @tparam T Element type
/// @tparam ChunkSize Number of elements per chunk (default: 8192)
template <typename T, std::size_t ChunkSize = kDefaultChunkSize>
class ChunkedVector {
  static_assert(ChunkSize > 0, "ChunkSize must be positive");

public:
  // ─────────────────────────────────────────────────────────────────────────
  // Type aliases
  // ─────────────────────────────────────────────────────────────────────────

  using value_type = T;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference = T&;
  using const_reference = const T&;
  using pointer = T*;
  using const_pointer = const T*;

private:
  // ─────────────────────────────────────────────────────────────────────────
  // Chunk storage
  // ─────────────────────────────────────────────────────────────────────────

  /// Storage for a single chunk. Uses aligned storage to allow
  /// placement new construction without default-constructing elements.
  struct Chunk {
    alignas(T) std::byte storage[sizeof(T) * ChunkSize];

    /// Get pointer to element at index within this chunk.
    [[nodiscard]] T* data() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }

    [[nodiscard]] const T* data() const noexcept {
      return std::launder(reinterpret_cast<const T*>(storage));
    }

    T& operator[](std::size_t idx) noexcept { return data()[idx]; }

    const T& operator[](std::size_t idx) const noexcept { return data()[idx]; }
  };

  /// Vector of chunk pointers. Never shrinks, only grows.
  std::vector<std::unique_ptr<Chunk>> chunks_;

  /// Total number of constructed elements.
  std::size_t size_ = 0;

public:
  // ─────────────────────────────────────────────────────────────────────────
  // Iterator
  // ─────────────────────────────────────────────────────────────────────────

  template <bool IsConst>
  class Iterator {
  public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = std::conditional_t<IsConst, const T*, T*>;
    using reference = std::conditional_t<IsConst, const T&, T&>;

  private:
    using ChunkVec = std::conditional_t<IsConst, const std::vector<std::unique_ptr<Chunk>>,
                                        std::vector<std::unique_ptr<Chunk>>>;

    ChunkVec* chunks_ = nullptr;
    std::size_t index_ = 0;

    friend class ChunkedVector;

    Iterator(ChunkVec* chunks, std::size_t index) : chunks_(chunks), index_(index) {}

  public:
    Iterator() = default;

    // Allow conversion from non-const to const iterator
    template <bool WasConst, typename = std::enable_if_t<IsConst && !WasConst>>
    Iterator(const Iterator<WasConst>& other) : chunks_(other.chunks_), index_(other.index_) {}

    reference operator*() const {
      std::size_t chunk_idx = index_ / ChunkSize;
      std::size_t offset = index_ % ChunkSize;
      return (*chunks_)[chunk_idx]->operator[](offset);
    }

    pointer operator->() const { return &**this; }

    reference operator[](difference_type n) const { return *(*this + n); }

    Iterator& operator++() {
      ++index_;
      return *this;
    }

    Iterator operator++(int) {
      Iterator tmp = *this;
      ++*this;
      return tmp;
    }

    Iterator& operator--() {
      --index_;
      return *this;
    }

    Iterator operator--(int) {
      Iterator tmp = *this;
      --*this;
      return tmp;
    }

    Iterator& operator+=(difference_type n) {
      index_ = static_cast<std::size_t>(static_cast<difference_type>(index_) + n);
      return *this;
    }

    Iterator& operator-=(difference_type n) {
      index_ = static_cast<std::size_t>(static_cast<difference_type>(index_) - n);
      return *this;
    }

    friend Iterator operator+(Iterator it, difference_type n) {
      it += n;
      return it;
    }

    friend Iterator operator+(difference_type n, Iterator it) {
      it += n;
      return it;
    }

    friend Iterator operator-(Iterator it, difference_type n) {
      it -= n;
      return it;
    }

    friend difference_type operator-(const Iterator& a, const Iterator& b) {
      return static_cast<difference_type>(a.index_) - static_cast<difference_type>(b.index_);
    }

    friend bool operator==(const Iterator& a, const Iterator& b) { return a.index_ == b.index_; }

    friend bool operator!=(const Iterator& a, const Iterator& b) { return a.index_ != b.index_; }

    friend bool operator<(const Iterator& a, const Iterator& b) { return a.index_ < b.index_; }

    friend bool operator>(const Iterator& a, const Iterator& b) { return a.index_ > b.index_; }

    friend bool operator<=(const Iterator& a, const Iterator& b) { return a.index_ <= b.index_; }

    friend bool operator>=(const Iterator& a, const Iterator& b) { return a.index_ >= b.index_; }

    // Allow const iterator to access non-const iterator's private members
    template <bool>
    friend class Iterator;
  };

  using iterator = Iterator<false>;
  using const_iterator = Iterator<true>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  // ─────────────────────────────────────────────────────────────────────────
  // Constructors / Destructor
  // ─────────────────────────────────────────────────────────────────────────

  /// Default constructor. Creates an empty container.
  ChunkedVector() = default;

  /// Construct with reserved chunk capacity.
  /// @param chunk_reserve Number of chunks to reserve space for.
  explicit ChunkedVector(std::size_t chunk_reserve) { chunks_.reserve(chunk_reserve); }

  /// Destructor. Destroys all elements.
  ~ChunkedVector() { clear(); }

  // Non-copyable (could be made copyable if needed)
  ChunkedVector(const ChunkedVector&) = delete;
  ChunkedVector& operator=(const ChunkedVector&) = delete;

  // Movable
  ChunkedVector(ChunkedVector&& other) noexcept
      : chunks_(std::move(other.chunks_)), size_(other.size_) {
    other.size_ = 0;
  }

  ChunkedVector& operator=(ChunkedVector&& other) noexcept {
    if (this != &other) {
      clear();
      chunks_ = std::move(other.chunks_);
      size_ = other.size_;
      other.size_ = 0;
    }
    return *this;
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Capacity
  // ─────────────────────────────────────────────────────────────────────────

  /// Returns the number of elements.
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  /// Returns true if the container is empty.
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

  /// Returns the total capacity (number of allocated element slots).
  [[nodiscard]] std::size_t capacity() const noexcept { return chunks_.size() * ChunkSize; }

  /// Returns the number of chunks.
  [[nodiscard]] std::size_t chunk_count() const noexcept { return chunks_.size(); }

  /// Returns the chunk size.
  [[nodiscard]] static constexpr std::size_t chunk_size() noexcept { return ChunkSize; }

  // ─────────────────────────────────────────────────────────────────────────
  // Element access
  // ─────────────────────────────────────────────────────────────────────────

  /// Access element at index (unchecked).
  /// @pre idx < size()
  [[nodiscard]] T& operator[](std::size_t idx) noexcept {
    assert(idx < size_);
    std::size_t chunk_idx = idx / ChunkSize;
    std::size_t offset = idx % ChunkSize;
    return (*chunks_[chunk_idx])[offset];
  }

  [[nodiscard]] const T& operator[](std::size_t idx) const noexcept {
    assert(idx < size_);
    std::size_t chunk_idx = idx / ChunkSize;
    std::size_t offset = idx % ChunkSize;
    return (*chunks_[chunk_idx])[offset];
  }

  /// Access element at index (bounds-checked).
  /// @throws std::out_of_range if idx >= size()
  [[nodiscard]] T& at(std::size_t idx) {
    if (idx >= size_) {
      throw std::out_of_range("ChunkedVector::at: index out of range");
    }
    return (*this)[idx];
  }

  [[nodiscard]] const T& at(std::size_t idx) const {
    if (idx >= size_) {
      throw std::out_of_range("ChunkedVector::at: index out of range");
    }
    return (*this)[idx];
  }

  /// Access first element.
  /// @pre !empty()
  [[nodiscard]] T& front() noexcept {
    assert(!empty());
    return (*this)[0];
  }

  [[nodiscard]] const T& front() const noexcept {
    assert(!empty());
    return (*this)[0];
  }

  /// Access last element.
  /// @pre !empty()
  [[nodiscard]] T& back() noexcept {
    assert(!empty());
    return (*this)[size_ - 1];
  }

  [[nodiscard]] const T& back() const noexcept {
    assert(!empty());
    return (*this)[size_ - 1];
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Modifiers
  // ─────────────────────────────────────────────────────────────────────────

  /// Add element to the end (copy).
  /// @return Reference to the inserted element.
  T& push_back(const T& value) { return emplace_back(value); }

  /// Add element to the end (move).
  /// @return Reference to the inserted element.
  T& push_back(T&& value) { return emplace_back(std::move(value)); }

  /// Construct element in place at the end.
  /// @return Reference to the constructed element.
  template <typename... Args>
  T& emplace_back(Args&&... args) {
    ensure_capacity();
    std::size_t chunk_idx = size_ / ChunkSize;
    std::size_t offset = size_ % ChunkSize;
    T* ptr = chunks_[chunk_idx]->data() + offset;
    ::new (static_cast<void*>(ptr)) T(std::forward<Args>(args)...);
    ++size_;
    return *ptr;
  }

  /// Add element and return pair of reference and index.
  /// Compatible with the original Nix API.
  template <typename... Args>
  std::pair<T&, std::size_t> add(Args&&... args) {
    std::size_t idx = size_;
    T& ref = emplace_back(std::forward<Args>(args)...);
    return {ref, idx};
  }

  /// Remove all elements (destructs them, but keeps chunks allocated).
  void clear() noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      for (std::size_t i = 0; i < size_; ++i) {
        std::size_t chunk_idx = i / ChunkSize;
        std::size_t offset = i % ChunkSize;
        chunks_[chunk_idx]->data()[offset].~T();
      }
    }
    size_ = 0;
  }

  /// Remove all elements and deallocate all chunks.
  void shrink_to_fit() {
    clear();
    chunks_.clear();
    chunks_.shrink_to_fit();
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Iterators
  // ─────────────────────────────────────────────────────────────────────────

  [[nodiscard]] iterator begin() noexcept { return iterator(&chunks_, 0); }

  [[nodiscard]] iterator end() noexcept { return iterator(&chunks_, size_); }

  [[nodiscard]] const_iterator begin() const noexcept { return const_iterator(&chunks_, 0); }

  [[nodiscard]] const_iterator end() const noexcept { return const_iterator(&chunks_, size_); }

  [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }

  [[nodiscard]] const_iterator cend() const noexcept { return end(); }

  [[nodiscard]] reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }

  [[nodiscard]] reverse_iterator rend() noexcept { return reverse_iterator(begin()); }

  [[nodiscard]] const_reverse_iterator rbegin() const noexcept {
    return const_reverse_iterator(end());
  }

  [[nodiscard]] const_reverse_iterator rend() const noexcept {
    return const_reverse_iterator(begin());
  }

  [[nodiscard]] const_reverse_iterator crbegin() const noexcept { return rbegin(); }

  [[nodiscard]] const_reverse_iterator crend() const noexcept { return rend(); }

  // ─────────────────────────────────────────────────────────────────────────
  // Iteration utilities
  // ─────────────────────────────────────────────────────────────────────────

  /// Apply function to each element.
  /// Compatible with the original Nix API.
  template <typename Fn>
  void forEach(Fn&& fn) const {
    for (std::size_t i = 0; i < size_; ++i) {
      fn((*this)[i]);
    }
  }

  /// Apply function to each element (mutable).
  template <typename Fn>
  void forEach(Fn&& fn) {
    for (std::size_t i = 0; i < size_; ++i) {
      fn((*this)[i]);
    }
  }

private:
  // ─────────────────────────────────────────────────────────────────────────
  // Internal helpers
  // ─────────────────────────────────────────────────────────────────────────

  /// Ensure there's space for at least one more element.
  void ensure_capacity() {
    if (size_ >= capacity()) {
      add_chunk();
    }
  }

  /// Allocate a new chunk.
  [[gnu::noinline]] void add_chunk() { chunks_.push_back(std::make_unique<Chunk>()); }
};

} // namespace straylight::nix::primitives
