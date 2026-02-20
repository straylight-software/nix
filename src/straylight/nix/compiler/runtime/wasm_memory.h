#pragma once
/// wasm_memory.h - Handle-based WASM memory access with compile-time safety
///
/// The Problem:
/// WASM linear memory can grow, invalidating any raw pointers into it.
/// If runtime code caches a pointer across an allocating operation,
/// it becomes a dangling pointer after memory growth.
///
/// The Solution:
/// All memory access goes through offsets (handles), never cached pointers.
/// - `mem_offset` is a 32-bit offset into WASM memory (always valid)
/// - `mem_ptr<T>` is a temporary pointer that MUST NOT be held across allocations
/// - The memory accessor always fetches the current base pointer
///
/// Invariant:
/// > Never store a raw pointer derived from WASM memory across any call
/// > that might allocate.

#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace straylight::nix::compiler::runtime {

// =============================================================================
// mem_offset - A stable handle into WASM memory
// =============================================================================

/// A 32-bit offset into WASM linear memory.
/// This is the stable identifier - always valid regardless of memory growth.
/// Zero is a valid offset (points to start of memory).
/// We use a null sentinel of 0xFFFFFFFF for "no value" cases.
struct mem_offset {
  std::uint32_t value;

  constexpr mem_offset() noexcept : value(0) {}
  constexpr explicit mem_offset(std::uint32_t v) noexcept : value(v) {}

  [[nodiscard]] constexpr auto raw() const noexcept -> std::uint32_t { return value; }

  constexpr auto operator+(std::uint32_t delta) const noexcept -> mem_offset {
    return mem_offset{value + delta};
  }

  constexpr auto operator-(mem_offset other) const noexcept -> std::uint32_t {
    return value - other.value;
  }

  constexpr auto operator==(mem_offset other) const noexcept -> bool {
    return value == other.value;
  }

  constexpr auto operator!=(mem_offset other) const noexcept -> bool {
    return value != other.value;
  }

  constexpr auto operator<(mem_offset other) const noexcept -> bool { return value < other.value; }

  /// Null sentinel for "no value" cases
  static constexpr auto null() noexcept -> mem_offset { return mem_offset{0xFFFFFFFF}; }

  [[nodiscard]] constexpr auto is_null() const noexcept -> bool { return value == 0xFFFFFFFF; }
};

// =============================================================================
// mem_ptr<T> - A temporary pointer that must not outlive its accessor scope
// =============================================================================

/// A temporary pointer into WASM memory.
/// This type exists to make it obvious that the pointer is ephemeral.
/// It MUST NOT be stored across any operation that might allocate.
///
/// The destructor is trivial so this can be used in expressions,
/// but the type name serves as documentation.
template <typename T>
class mem_ptr {
  T* ptr_;

public:
  constexpr explicit mem_ptr(T* p) noexcept : ptr_(p) {}

  [[nodiscard]] constexpr auto get() const noexcept -> T* { return ptr_; }
  [[nodiscard]] constexpr auto operator->() const noexcept -> T* { return ptr_; }
  [[nodiscard]] constexpr auto operator*() const noexcept -> T& { return *ptr_; }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return ptr_ != nullptr; }

  // Allow pointer arithmetic for array access
  [[nodiscard]] constexpr auto operator+(std::ptrdiff_t n) const noexcept -> mem_ptr<T> {
    return mem_ptr<T>{ptr_ + n};
  }

  [[nodiscard]] constexpr auto operator[](std::ptrdiff_t n) const noexcept -> T& { return ptr_[n]; }
};

// =============================================================================
// Memory access errors
// =============================================================================

/// Thrown when memory access is out of bounds
class memory_bounds_error : public std::runtime_error {
public:
  explicit memory_bounds_error(const std::string& msg) : std::runtime_error(msg) {}
};

/// Thrown when memory is exhausted
class memory_exhausted_error : public std::runtime_error {
public:
  explicit memory_exhausted_error(const std::string& msg) : std::runtime_error(msg) {}
};

// =============================================================================
// wasm_memory - The memory subsystem
// =============================================================================

/// Callback type for growing WASM memory.
/// Takes the number of pages to grow by, returns true on success.
using grow_memory_fn = std::function<bool(std::uint32_t pages)>;

/// Callback type for getting the current WASM memory.
/// Returns a span of the current memory (may change after grow).
using get_memory_fn = std::function<std::span<std::uint8_t>()>;

/// WASM page size in bytes
constexpr std::uint32_t WASM_PAGE_SIZE = 65536;

/// The WASM memory subsystem.
///
/// This class provides handle-based access to WASM linear memory.
/// All access goes through offsets, and raw pointers are only
/// obtained at the point of use via accessor methods.
///
/// The bump allocator is built-in and operates directly on the
/// WASM memory, eliminating any need for synchronization.
class wasm_memory {
public:
  /// Construct with callbacks to access the underlying WASM memory.
  /// @param get_mem Callback to get current memory span (called on every access)
  /// @param grow_mem Callback to grow memory (called when allocation needs more space)
  /// @param heap_base Starting offset for heap allocations
  wasm_memory(get_memory_fn get_mem, grow_memory_fn grow_mem, std::uint32_t heap_base)
      : get_memory_(std::move(get_mem)),
        grow_memory_(std::move(grow_mem)),
        heap_base_(heap_base),
        next_free_(heap_base) {}

  /// Non-copyable, non-movable (callbacks may capture `this`)
  wasm_memory(const wasm_memory&) = delete;
  wasm_memory(wasm_memory&&) = delete;
  auto operator=(const wasm_memory&) -> wasm_memory& = delete;
  auto operator=(wasm_memory&&) -> wasm_memory& = delete;

  // ===========================================================================
  // Allocation
  // ===========================================================================

  /// Allocate `size` bytes from the heap, returns offset to allocated block.
  /// Automatically grows memory if needed.
  /// @throws memory_exhausted_error if memory cannot be grown
  [[nodiscard]] auto allocate(std::uint32_t size) -> mem_offset {
    // Align to 8 bytes
    size = align_up(size);

    auto mem = get_memory_();
    auto needed = next_free_ + size;

    // Grow if needed
    while (needed > mem.size()) {
      auto current_pages = static_cast<std::uint32_t>(mem.size() / WASM_PAGE_SIZE);
      auto needed_pages = (needed + WASM_PAGE_SIZE - 1) / WASM_PAGE_SIZE;
      auto grow_by = needed_pages - current_pages;

      if (!grow_memory_(grow_by)) {
        throw memory_exhausted_error("failed to grow WASM memory by " + std::to_string(grow_by) +
                                     " pages");
      }

      // Re-fetch memory after growth
      mem = get_memory_();
    }

    auto offset = mem_offset{next_free_};
    next_free_ += size;
    return offset;
  }

  /// Reset the heap to its initial state (for arena-style allocation).
  void reset_heap() noexcept { next_free_ = heap_base_; }

  /// Get the current heap high-water mark
  [[nodiscard]] auto heap_used() const noexcept -> std::uint32_t { return next_free_ - heap_base_; }

  /// Get the heap base offset
  [[nodiscard]] auto heap_base() const noexcept -> mem_offset { return mem_offset{heap_base_}; }

  // ===========================================================================
  // Memory access - these always fetch fresh memory
  // ===========================================================================

  /// Get a temporary pointer to memory at the given offset.
  /// The returned pointer MUST NOT be stored across allocating operations.
  template <typename T = std::uint8_t>
  [[nodiscard]] auto ptr(mem_offset offset) -> mem_ptr<T> {
    auto mem = get_memory_();
    check_bounds(offset.raw(), sizeof(T), mem.size());
    return mem_ptr<T>{reinterpret_cast<T*>(mem.data() + offset.raw())};
  }

  /// Get a const temporary pointer
  template <typename T = std::uint8_t>
  [[nodiscard]] auto ptr(mem_offset offset) const -> mem_ptr<const T> {
    auto mem = get_memory_();
    check_bounds(offset.raw(), sizeof(T), mem.size());
    return mem_ptr<const T>{reinterpret_cast<const T*>(mem.data() + offset.raw())};
  }

  /// Read a value at the given offset
  template <typename T>
  [[nodiscard]] auto read(mem_offset offset) const -> T {
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
    auto mem = get_memory_();
    check_bounds(offset.raw(), sizeof(T), mem.size());
    T value;
    std::memcpy(&value, mem.data() + offset.raw(), sizeof(T));
    return value;
  }

  /// Write a value at the given offset
  template <typename T>
  void write(mem_offset offset, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
    auto mem = get_memory_();
    check_bounds(offset.raw(), sizeof(T), mem.size());
    std::memcpy(mem.data() + offset.raw(), &value, sizeof(T));
  }

  /// Read a null-terminated string starting at offset
  [[nodiscard]] auto read_string(mem_offset offset) const -> std::string_view {
    auto mem = get_memory_();
    if (offset.raw() >= mem.size()) {
      throw memory_bounds_error("string offset out of bounds");
    }
    const char* start = reinterpret_cast<const char*>(mem.data() + offset.raw());
    // Find null terminator within bounds
    std::size_t max_len = mem.size() - offset.raw();
    std::size_t len = 0;
    while (len < max_len && start[len] != '\0') {
      ++len;
    }
    if (len == max_len) {
      throw memory_bounds_error("string not null-terminated within memory bounds");
    }
    return std::string_view{start, len};
  }

  /// Write a string at offset (including null terminator)
  void write_string(mem_offset offset, std::string_view str) {
    auto mem = get_memory_();
    auto size = str.size() + 1; // include null terminator
    check_bounds(offset.raw(), static_cast<std::uint32_t>(size), mem.size());
    std::memcpy(mem.data() + offset.raw(), str.data(), str.size());
    mem[offset.raw() + str.size()] = '\0';
  }

  /// Allocate and write a string, returns offset to the allocated string
  [[nodiscard]] auto alloc_string(std::string_view str) -> mem_offset {
    auto size = static_cast<std::uint32_t>(str.size() + 1); // include null terminator
    auto offset = allocate(size);
    write_string(offset, str);
    return offset;
  }

  /// Copy bytes from one offset to another
  void copy(mem_offset dst, mem_offset src, std::uint32_t size) {
    auto mem = get_memory_();
    check_bounds(src.raw(), size, mem.size());
    check_bounds(dst.raw(), size, mem.size());
    // Use memmove to handle overlapping regions
    std::memmove(mem.data() + dst.raw(), mem.data() + src.raw(), size);
  }

  /// Get current memory size in bytes
  [[nodiscard]] auto size() const -> std::uint32_t {
    return static_cast<std::uint32_t>(get_memory_().size());
  }

  /// Get current memory size in pages
  [[nodiscard]] auto pages() const -> std::uint32_t { return size() / WASM_PAGE_SIZE; }

  // ===========================================================================
  // Convenience accessors for common types
  // ===========================================================================

  [[nodiscard]] auto read_i32(mem_offset offset) const -> std::int32_t {
    return read<std::int32_t>(offset);
  }

  [[nodiscard]] auto read_u32(mem_offset offset) const -> std::uint32_t {
    return read<std::uint32_t>(offset);
  }

  [[nodiscard]] auto read_i64(mem_offset offset) const -> std::int64_t {
    return read<std::int64_t>(offset);
  }

  [[nodiscard]] auto read_u64(mem_offset offset) const -> std::uint64_t {
    return read<std::uint64_t>(offset);
  }

  [[nodiscard]] auto read_f64(mem_offset offset) const -> double { return read<double>(offset); }

  void write_i32(mem_offset offset, std::int32_t value) { write(offset, value); }
  void write_u32(mem_offset offset, std::uint32_t value) { write(offset, value); }
  void write_i64(mem_offset offset, std::int64_t value) { write(offset, value); }
  void write_u64(mem_offset offset, std::uint64_t value) { write(offset, value); }
  void write_f64(mem_offset offset, double value) { write(offset, value); }

private:
  get_memory_fn get_memory_;
  grow_memory_fn grow_memory_;
  std::uint32_t heap_base_;
  std::uint32_t next_free_;

  static constexpr auto align_up(std::uint32_t size) noexcept -> std::uint32_t {
    return (size + 7) & ~7u;
  }

  static void check_bounds(std::uint32_t offset, std::uint32_t size, std::size_t mem_size) {
    if (offset > mem_size || size > mem_size - offset) {
      throw memory_bounds_error("memory access out of bounds: offset=" + std::to_string(offset) +
                                " size=" + std::to_string(size) +
                                " mem_size=" + std::to_string(mem_size));
    }
  }
};

// =============================================================================
// Testing support - standalone memory for unit tests
// =============================================================================

/// A standalone memory implementation for testing without WASM.
/// Uses a std::vector internally and can simulate memory growth.
class test_memory {
public:
  explicit test_memory(std::uint32_t initial_pages = 1, std::uint32_t max_pages = 256,
                       std::uint32_t heap_base = 0x20000)
      : data_(initial_pages * WASM_PAGE_SIZE, 0),
        max_pages_(max_pages),
        memory_([this]() -> std::span<std::uint8_t> { return std::span{data_}; },
                [this](std::uint32_t pages) -> bool { return grow(pages); }, heap_base) {}

  /// Get the wasm_memory interface
  [[nodiscard]] auto memory() -> wasm_memory& { return memory_; }
  [[nodiscard]] auto memory() const -> const wasm_memory& { return memory_; }

  /// Direct access to underlying data (for test verification)
  [[nodiscard]] auto data() -> std::span<std::uint8_t> { return std::span{data_}; }
  [[nodiscard]] auto data() const -> std::span<const std::uint8_t> { return std::span{data_}; }

  /// Current size in pages
  [[nodiscard]] auto pages() const -> std::uint32_t {
    return static_cast<std::uint32_t>(data_.size() / WASM_PAGE_SIZE);
  }

  /// Simulate memory growth
  [[nodiscard]] auto grow(std::uint32_t pages) -> bool {
    auto new_pages = this->pages() + pages;
    if (new_pages > max_pages_) {
      return false;
    }
    data_.resize(new_pages * WASM_PAGE_SIZE, 0);
    return true;
  }

  /// Count how many times grow was called (for testing)
  [[nodiscard]] auto grow_count() const -> std::uint32_t { return grow_count_; }

private:
  std::vector<std::uint8_t> data_;
  std::uint32_t max_pages_;
  std::uint32_t grow_count_ = 0;
  wasm_memory memory_;
};

} // namespace straylight::nix::compiler::runtime
