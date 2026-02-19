# Memory Architecture

This document describes the memory layout and allocation strategy for the Nix-to-WASM compiler and runtime.

---

## Overview

The system has two phases of memory management:

1. **Compile-time**: The compiler allocates space in the WASM data segment for static data (strings, initial attrset layouts, closure environments for statically-known closures)

2. **Runtime**: The runtime allocates memory dynamically for values created during evaluation (thunks, new attrsets, concatenated strings, etc.)

Both phases share a single WASM linear memory, so they must coordinate to avoid collisions.

---

## Memory Layout

```
┌─────────────────────────────────────────────────────────────────┐
│                     WASM Linear Memory (1MB+)                   │
├─────────────────────────────────────────────────────────────────┤
│ 0x00000 ┌─────────────────────────────────────────────────────┐ │
│         │            DATA SEGMENT (compile-time)              │ │
│         │  - Interned strings (null-terminated)               │ │
│         │  - Static attrset layouts                           │ │
│         │  - Static list layouts                              │ │
│         │  - Closure environments (for static closures)       │ │
│         │  - Thunk environments (for static thunks)           │ │
│ 0x10000 └─────────────────────────────────────────────────────┘ │
│         ┌─────────────────────────────────────────────────────┐ │
│         │                    STACK                            │ │
│         │  (grows down from 0x10000, used by WASM locals)     │ │
│ 0x0F000 └─────────────────────────────────────────────────────┘ │
│         ┌─────────────────────────────────────────────────────┐ │
│         │                  [RESERVED]                         │ │
│ 0x20000 └─────────────────────────────────────────────────────┘ │
│         ┌─────────────────────────────────────────────────────┐ │
│         │              RUNTIME HEAP                           │ │
│         │  - Dynamic closures                                 │ │
│         │  - Thunk headers + cached values                    │ │
│         │  - Runtime-created lists                            │ │
│         │  - Runtime-created attrsets                         │ │
│         │  - Concatenated strings                             │ │
│         │  (grows up from 0x20000)                            │ │
│ 0xFFFFF └─────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### Region Boundaries

| Region | Start | End | Size | Purpose |
|--------|-------|-----|------|---------|
| Data Segment | 0x00000 | 0x0FFFF | 64 KB | Compile-time static data |
| Stack | 0x0F000 | 0x10000 | 4 KB | WASM call stack (grows down) |
| Reserved | 0x10000 | 0x1FFFF | 64 KB | Future use / guard region |
| Runtime Heap | 0x20000 | 0xFFFFF | 896 KB | Runtime allocations |

**Total minimum memory: 1 MB (16 WASM pages)**

---

## Value Representation

All Nix values are represented as a single `i64`:

```
┌────────────────────────────────────────────────────────────────┐
│                         nix_value (i64)                        │
├────────────────────────────┬───────────────────────────────────┤
│    Payload (high 32 bits)  │      Tag (low 32 bits)            │
│    - int value             │      0 = null                     │
│    - bool (0/1)            │      1 = bool                     │
│    - memory offset         │      2 = int                      │
│    - func table index      │      3 = float                    │
│                            │      4 = string                   │
│                            │      5 = path                     │
│                            │      6 = list                     │
│                            │      7 = attrset                  │
│                            │      8 = lambda/closure           │
│                            │      9 = thunk                    │
│                            │     10 = primop                   │
└────────────────────────────┴───────────────────────────────────┘
```

### Payload Interpretation by Tag

| Tag | Type | Payload Meaning |
|-----|------|-----------------|
| 0 | null | Always 0 |
| 1 | bool | 0 = false, 1 = true |
| 2 | int | Signed 32-bit integer value |
| 3 | float | Pointer to f64 in memory |
| 4 | string | Pointer to null-terminated string |
| 5 | path | Pointer to null-terminated path string |
| 6 | list | Pointer to list header |
| 7 | attrset | Pointer to attrset header |
| 8 | lambda | Pointer to closure struct |
| 9 | thunk | Pointer to thunk struct |
| 10 | primop | Builtin function index |

---

## Data Structures in Memory

### String

Null-terminated, stored directly in memory:

```
offset: 'h' 'e' 'l' 'l' 'o' '\0'
        ─────────────────────────
        ^
        payload points here
```

### List

```
offset+0:  count (i32)
offset+4:  element[0] (nix_value, 8 bytes)
offset+12: element[1] (nix_value, 8 bytes)
...
```

Total size: `4 + count * 8` bytes

### Attrset (Static Keys)

Used when all attribute names are known at compile time:

```
offset+0:  count (i32)
offset+4:  entry[0]: key_offset (i32) + value (nix_value, 8 bytes) = 12 bytes
offset+16: entry[1]: key_offset (i32) + value (nix_value, 8 bytes) = 12 bytes
...
```

Total size: `4 + count * 12` bytes

### Attrset (Dynamic Keys)

Used when attribute names are computed at runtime:

```
offset+0:  count (i32)
offset+4:  entry[0]: key (nix_value, 8 bytes) + value (nix_value, 8 bytes) = 16 bytes
offset+20: entry[1]: ...
...
```

Total size: `4 + count * 16` bytes (converted to static layout at runtime)

### Closure

```
offset+0:  func_index (i32)      - index into WASM function table
offset+4:  capture_count (i32)   - number of captured variables
offset+8:  captures[0] (nix_value, 8 bytes)
offset+16: captures[1] (nix_value, 8 bytes)
...
```

Total size: `8 + capture_count * 8` bytes

### Thunk

```
offset+0:  func_index (i32)      - index into WASM function table
offset+4:  env_offset (i32)      - pointer to captured environment
offset+8:  state (i32)           - 0=pending, 1=evaluating, 2=evaluated
offset+12: cached_value (nix_value, 8 bytes)
```

Total size: 20 bytes

The environment at `env_offset` has the same layout as closure captures:

```
env_offset+0: capture_count (i32)
env_offset+4: captures[0] (nix_value, 8 bytes)
...
```

---

## Compile-Time Allocation

The compiler maintains a bump allocator `data_offset_` starting at 0:

```cpp
class compiler {
  std::uint32_t data_offset_ = 0;  // next free byte in data segment
  std::unordered_map<std::string, std::uint32_t> string_offsets_;  // dedup
  
  auto allocate_string(std::string_view str) -> std::uint32_t {
    // dedup: return existing offset if already allocated
    // otherwise: allocate at data_offset_, advance by len+1
  }
  
  auto allocate_bytes(std::uint32_t size) -> std::uint32_t {
    auto offset = data_offset_;
    data_offset_ += size;
    data_offset_ = (data_offset_ + 7) & ~7u;  // align to 8
    return offset;
  }
};
```

### Compile-Time Limit

The data segment MUST NOT exceed 64 KB (0x10000 bytes). The compiler should check this:

```cpp
if (data_offset_ >= 0x10000) {
  throw compilation_error("data segment overflow: expression too large");
}
```

---

## Runtime Allocation

The runtime uses a simple bump allocator starting at the heap base (0x20000):

```cpp
class runtime_heap {
  std::uint32_t next_free_ = 0x20000;  // HEAP_BASE
  std::uint32_t heap_end_ = 0x100000;  // 1 MB default
  
  auto allocate(std::uint32_t size) -> std::uint32_t {
    size = (size + 7) & ~7u;  // align to 8
    if (next_free_ + size > heap_end_) {
      throw runtime_error("out of memory");
    }
    auto ptr = next_free_;
    next_free_ += size;
    return ptr;
  }
};
```

### Runtime Allocations

| Operation | Allocation Size |
|-----------|----------------|
| `__makeClosure` | 8 + capture_count * 8 |
| `__makeThunk` | 20 bytes (header only; env already allocated) |
| `__makeList` | 4 + count * 8 |
| `__makeAttrs` | 4 + count * 12 |
| `__makeAttrsDynamic` | 4 + count * 12 |
| `__concatStrings` | result_length + 1 |
| `__update` | 4 + merged_count * 12 |
| `__concat` | 4 + combined_count * 8 |

---

## Garbage Collection

**Current status: None**

The bump allocator never frees memory. This is acceptable for:
- Single-expression evaluation
- Short-lived WASM instances

**Future: Arena-based collection**

For long-running evaluations (REPL, daemon), implement arena-based GC:

1. Allocate in arenas (e.g., 64 KB chunks)
2. After each top-level evaluation, scan for reachable values
3. Copy live values to new arena
4. Free old arenas

This avoids the complexity of tracing GC while allowing memory reuse.

**Future: WASM GC**

When WASM GC proposal is widely supported, migrate to GC-managed structs:

```wasm
(type $nix_value (struct (field $tag i32) (field $payload i32)))
(type $list (array (ref $nix_value)))
(type $closure (struct 
  (field $func_index i32)
  (field $captures (ref (array (ref $nix_value))))))
```

---

## Invariants

1. **Data segment boundary**: `data_offset_ < 0x10000` at compile completion
2. **Heap boundary**: `next_free_ < heap_end_` at every allocation
3. **No overlap**: Data segment [0, 0x10000) and heap [0x20000, heap_end_) never overlap
4. **Alignment**: All allocations are 8-byte aligned
5. **Thunk states**: 
   - 0 (pending) → 1 (evaluating) → 2 (evaluated)
   - 1 → 1 means infinite recursion (error)
6. **Closure safety**: Closure environments are always fully initialized before the closure value is returned

---

## Error Handling

| Error | Cause | Detection |
|-------|-------|-----------|
| Data segment overflow | Expression too complex | `data_offset_ >= 0x10000` at compile time |
| Out of memory | Too many runtime allocations | `next_free_ + size > heap_end_` |
| Infinite recursion | Thunk references itself | Thunk state is 1 when forcing |
| Invalid memory access | Bug or corruption | WASM trap on OOB access |

---

## Implementation Status

### Complete ✓

1. **Heap allocator** ✓
   - `heap_allocator` class provides bump allocator
   - `runtime_context` owns the allocator
   - All runtime allocations go through `ctx.allocate()`
   - Supports multiple WASM instances (each has own context)

2. **Thunk layout** ✓
   - Thunk struct: func_index(4) + env_ptr(4) + state(4) + cached(8) = 20 bytes
   - Uses `memory_layout::THUNK_*` constants consistently
   - `rt_force` reads/writes correct offsets
   - Infinite recursion detection via state field

3. **Heap bounds checking** ✓
   - `heap_allocator::allocate()` throws `oom_error` on overflow
   - Configurable heap limit in constructor

4. **Empty attrset representation** ✓
   - Empty `{}` has payload pointer 0
   - `find_attr` returns nullopt immediately for pointer 0
   - Fixes: `{}.x` correctly errors, `{} ? x` correctly returns false

5. **Memory layout property tests** ✓
   - `property_test.cpp` verifies alignment invariants
   - `property_test.cpp` verifies allocation non-overlap
   - `property_test.cpp` verifies structure sizes match formulas

### Not Yet Implemented

1. **Data segment limit check** in compiler
   - Compiler's `data_offset_` should check against `DATA_SEGMENT_LIMIT`
   - Currently no error if data segment exceeds 64KB

2. **Arena-based GC** for REPL use
   - Currently bump allocator never frees
   - For long-running sessions, need arena collection

3. **Memory growth**
   - Currently fixed at 1MB
   - Should grow on demand up to configurable limit

---

## Constants

See `runtime/memory_layout.hh` (~210 lines) for the authoritative constants. Summary:

```cpp
namespace nix::language::memory_layout {
  // Region boundaries
  constexpr std::uint32_t DATA_SEGMENT_BASE = 0x00000;
  constexpr std::uint32_t DATA_SEGMENT_LIMIT = 0x10000;  // 64 KB
  constexpr std::uint32_t STACK_TOP = 0x10000;           // grows down
  constexpr std::uint32_t STACK_SIZE = 0x01000;          // 4 KB
  constexpr std::uint32_t HEAP_BASE = 0x20000;           // 128 KB offset
  constexpr std::uint32_t DEFAULT_HEAP_SIZE = 0xE0000;   // 896 KB
  constexpr std::uint32_t DEFAULT_MEMORY_SIZE = 0x100000; // 1 MB (16 pages)
  constexpr std::uint32_t WASM_PAGE_SIZE = 0x10000;      // 64 KB
  
  // Alignment
  constexpr std::uint32_t ALIGNMENT = 8;
  auto align_up(std::uint32_t size) noexcept -> std::uint32_t;
  
  // Structure sizes and offsets
  constexpr std::uint32_t VALUE_SIZE = 8;
  constexpr std::uint32_t THUNK_SIZE = 20;
  constexpr std::uint32_t CLOSURE_HEADER_SIZE = 8;
  constexpr std::uint32_t LIST_HEADER_SIZE = 4;
  constexpr std::uint32_t ATTRSET_HEADER_SIZE = 4;
  constexpr std::uint32_t ATTRSET_ENTRY_SIZE = 12;
  constexpr std::uint32_t ATTRSET_DYNAMIC_ENTRY_SIZE = 16;
  constexpr std::uint32_t ENV_HEADER_SIZE = 4;
  constexpr std::uint32_t FLOAT_SIZE = 8;
  
  // Thunk states (for recursion detection)
  constexpr std::int32_t THUNK_STATE_PENDING = 0;
  constexpr std::int32_t THUNK_STATE_EVALUATING = 1;
  constexpr std::int32_t THUNK_STATE_EVALUATED = 2;
  
  // Size calculators
  auto list_size(std::uint32_t count) noexcept -> std::uint32_t;
  auto attrset_size(std::uint32_t count) noexcept -> std::uint32_t;
  auto closure_size(std::uint32_t capture_count) noexcept -> std::uint32_t;
  auto env_size(std::uint32_t capture_count) noexcept -> std::uint32_t;
}
```

### Special Values

| Value | Representation | Meaning |
|-------|----------------|---------|
| Empty attrset `{}` | `tag=7, payload=0` | Pointer 0 = empty |
| `null` | `tag=0, payload=0` | Always zero |
| `true` | `tag=1, payload=1` | Bool with payload 1 |
| `false` | `tag=1, payload=0` | Bool with payload 0 |
