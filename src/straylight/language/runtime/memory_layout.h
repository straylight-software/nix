#pragma once
///@file straylight/language/runtime/memory_layout.h
/// Memory layout constants shared between compiler and runtime.
///
/// See MEMORY.md for detailed documentation.

#include <cstdint>

namespace straylight::language::memory_layout {

// =============================================================================
// Region Boundaries
// =============================================================================

/// Base address of data segment (compile-time allocations)
constexpr std::uint32_t DATA_SEGMENT_BASE = 0x00000;

/// Maximum size of data segment (64 KB)
constexpr std::uint32_t DATA_SEGMENT_LIMIT = 0x10000;

/// Top of WASM stack (grows down from here)
constexpr std::uint32_t STACK_TOP = 0x10000;

/// Size of WASM stack (4 KB)
constexpr std::uint32_t STACK_SIZE = 0x01000;

/// Base address of runtime heap (runtime allocations)
constexpr std::uint32_t HEAP_BASE = 0x20000;

/// Default heap size (896 KB)
constexpr std::uint32_t DEFAULT_HEAP_SIZE = 0xE0000;

/// Default total memory size (1 MB = 16 WASM pages)
constexpr std::uint32_t DEFAULT_MEMORY_SIZE = 0x100000;

/// WASM page size
constexpr std::uint32_t WASM_PAGE_SIZE = 0x10000; // 64 KB

// =============================================================================
// Alignment
// =============================================================================

/// All allocations are aligned to this boundary
constexpr std::uint32_t ALIGNMENT = 8;

/// Align a size up to the alignment boundary
[[nodiscard]] constexpr auto align_up(std::uint32_t size) noexcept -> std::uint32_t {
  return (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

// =============================================================================
// Value Representation
// =============================================================================

/// Size of a nix_value (i64 = 8 bytes)
constexpr std::uint32_t VALUE_SIZE = 8;

// =============================================================================
// String Layout
// =============================================================================

// Strings are null-terminated, stored directly in memory.
// The payload points to the first character.
// No length prefix (use strlen or store length separately if needed).

// =============================================================================
// List Layout
// =============================================================================

/// Offset of count field in list
constexpr std::uint32_t LIST_COUNT_OFFSET = 0;

/// Offset of first element in list
constexpr std::uint32_t LIST_ELEMENTS_OFFSET = 4;

/// Size of list header (just the count)
constexpr std::uint32_t LIST_HEADER_SIZE = 4;

/// Calculate total size of a list
[[nodiscard]] constexpr auto list_size(std::uint32_t count) noexcept -> std::uint32_t {
  return LIST_HEADER_SIZE + count * VALUE_SIZE;
}

// =============================================================================
// Attrset Layout (Static Keys)
// =============================================================================

// Layout: count (i32) + entries[]
// Entry: key_offset (i32) + value (nix_value, 8 bytes) = 12 bytes

/// Offset of count field in attrset
constexpr std::uint32_t ATTRSET_COUNT_OFFSET = 0;

/// Offset of first entry in attrset
constexpr std::uint32_t ATTRSET_ENTRIES_OFFSET = 4;

/// Size of one attrset entry
constexpr std::uint32_t ATTRSET_ENTRY_SIZE = 12;

/// Offset of key within an entry
constexpr std::uint32_t ATTRSET_ENTRY_KEY_OFFSET = 0;

/// Offset of value within an entry
constexpr std::uint32_t ATTRSET_ENTRY_VALUE_OFFSET = 4;

/// Size of attrset header (just the count)
constexpr std::uint32_t ATTRSET_HEADER_SIZE = 4;

/// Calculate total size of an attrset
[[nodiscard]] constexpr auto attrset_size(std::uint32_t count) noexcept -> std::uint32_t {
  return ATTRSET_HEADER_SIZE + count * ATTRSET_ENTRY_SIZE;
}

// =============================================================================
// Attrset Layout (Dynamic Keys)
// =============================================================================

// Temporary layout used during construction when keys are nix_values.
// Entry: key (nix_value, 8 bytes) + value (nix_value, 8 bytes) = 16 bytes
// Converted to static layout at runtime.

/// Size of one dynamic attrset entry
constexpr std::uint32_t ATTRSET_DYNAMIC_ENTRY_SIZE = 16;

// =============================================================================
// Closure Layout
// =============================================================================

// Layout: func_index (i32) + capture_count (i32) + captures[]

/// Offset of func_index in closure
constexpr std::uint32_t CLOSURE_FUNC_INDEX_OFFSET = 0;

/// Offset of capture_count in closure
constexpr std::uint32_t CLOSURE_CAPTURE_COUNT_OFFSET = 4;

/// Offset of first capture in closure
constexpr std::uint32_t CLOSURE_CAPTURES_OFFSET = 8;

/// Size of closure header (func_index + capture_count)
constexpr std::uint32_t CLOSURE_HEADER_SIZE = 8;

/// Calculate total size of a closure
[[nodiscard]] constexpr auto closure_size(std::uint32_t capture_count) noexcept -> std::uint32_t {
  return CLOSURE_HEADER_SIZE + capture_count * VALUE_SIZE;
}

// =============================================================================
// Thunk Layout
// =============================================================================

// Layout: func_index (i32) + env_ptr (i32) + state (i32) + cached_value (nix_value)

/// Offset of func_index in thunk
constexpr std::uint32_t THUNK_FUNC_INDEX_OFFSET = 0;

/// Offset of env_ptr in thunk
constexpr std::uint32_t THUNK_ENV_PTR_OFFSET = 4;

/// Offset of state in thunk
constexpr std::uint32_t THUNK_STATE_OFFSET = 8;

/// Offset of cached_value in thunk
constexpr std::uint32_t THUNK_CACHED_VALUE_OFFSET = 12;

/// Total size of thunk header
constexpr std::uint32_t THUNK_SIZE = 20;

/// Thunk state: not yet evaluated
constexpr std::int32_t THUNK_STATE_PENDING = 0;

/// Thunk state: currently being evaluated (recursion detection)
constexpr std::int32_t THUNK_STATE_EVALUATING = 1;

/// Thunk state: evaluation complete, cached_value is valid
constexpr std::int32_t THUNK_STATE_EVALUATED = 2;

// =============================================================================
// Environment Layout (for thunks and closures with captures)
// =============================================================================

// Layout: capture_count (i32) + captures[]

/// Offset of capture_count in environment
constexpr std::uint32_t ENV_CAPTURE_COUNT_OFFSET = 0;

/// Offset of first capture in environment
constexpr std::uint32_t ENV_CAPTURES_OFFSET = 4;

/// Size of environment header
constexpr std::uint32_t ENV_HEADER_SIZE = 4;

/// Calculate total size of an environment
[[nodiscard]] constexpr auto env_size(std::uint32_t capture_count) noexcept -> std::uint32_t {
  return ENV_HEADER_SIZE + capture_count * VALUE_SIZE;
}

// =============================================================================
// Float Layout
// =============================================================================

// Floats are stored as f64 (8 bytes) in memory.
// The payload is a pointer to the f64.

/// Size of a float in memory
constexpr std::uint32_t FLOAT_SIZE = 8;

} // namespace straylight::language::memory_layout
