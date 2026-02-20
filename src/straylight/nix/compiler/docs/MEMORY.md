# Memory Architecture

This document describes the memory layout and allocation strategy for the Nix-to-WASM compiler and
runtime.

______________________________________________________________________

## Overview

The system has two phases of memory management:

1. **Compile-time**: The compiler allocates space in the WASM data segment for static data (strings,
   initial attrset layouts, closure environments for statically-known closures)

2. **Runtime**: The runtime allocates memory dynamically for values created during evaluation
   (thunks, new attrsets, concatenated strings, etc.)

Both phases share a single WASM linear memory.

______________________________________________________________________

## Unified Memory Architecture

The runtime uses a single source of truth for all memory access:

```
┌───────────────────────────────────────────────────────────────────────┐
│                        MEMORY ARCHITECTURE                            │
├───────────────────────────────────────────────────────────────────────┤
│                                                                       │
│  ┌──────────────────────────────────────────────────────────────────┐ │
│  │                    WASM Linear Memory                            │ │
│  │                    (wasmtime::Memory)                            │ │
│  │                                                                  │ │
│  │  - Single source of truth                                        │ │
│  │  - WASM code: i32.load / i32.store                               │ │
│  │  - Host functions: wasm_memory->read_*() / write_*()             │ │
│  │  - runtime_context::mem points here                              │ │
│  └──────────────────────────────────────────────────────────────────┘ │
│                              ▲                                        │
│                              │                                        │
│         ┌────────────────────┼────────────────────┐                   │
│         │                    │                    │                   │
│         ▼                    ▼                    ▼                   │
│  ┌────────────┐     ┌────────────────┐    ┌────────────────┐          │
│  │  WASM code │     │  Host funcs    │    │  wasm_memory   │          │
│  │  (direct)  │     │  (via ctx.mem) │    │  (allocator)   │          │
│  └────────────┘     └────────────────┘    └────────────────┘          │
│                                                                       │
└───────────────────────────────────────────────────────────────────────┘
```

### Key Design Points

1. **`wasm_memory` class** provides handle-based access to WASM linear memory

   - Callbacks fetch current memory pointer on every access
   - Safe across memory growth (no stale pointers)
   - Built-in bump allocator for heap region

2. **`runtime_context::mem`** points to the `wasm_memory` instance

   - All host function reads/writes go through this
   - No separate buffer, no syncing needed

3. **`mem_offset`** is a stable handle (32-bit offset)

   - Always valid regardless of memory growth
   - Never store raw pointers across allocating operations

### Implementation

```cpp
// runtime.h - runtime_context uses wasm_memory directly
struct runtime_context {
  wasm_memory* mem = nullptr;  // single source of truth
  
  // All memory access delegates to wasm_memory
  auto read_i32(uint32_t offset) const -> int32_t {
    return mem->read_i32(mem_offset{offset});
  }
  auto allocate(uint32_t size) -> uint32_t {
    return mem->allocate(size).raw();
  }
  // ... etc
};

// wasm_executor.cpp - setup
wasm_mem_ = std::make_unique<wasm_memory>(
    [this]() { return memory_->data(store_->context()); },  // get_memory
    [this](uint32_t pages) { return memory_->grow(...); },  // grow_memory
    mem::HEAP_BASE);
ctx_.mem = wasm_mem_.get();
```

______________________________________________________________________

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
│         │  - Builtins attrset (allocated at init)             │ │
│         │  (grows up from 0x20000)                            │ │
│ 0xFFFFF └─────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### Region Boundaries

| Region | Start | End | Size | Purpose | |--------|-------|-----|------|---------| | Data Segment |
0x00000 | 0x0FFFF | 64 KB | Compile-time static data | | Stack | 0x0F000 | 0x10000 | 4 KB | WASM
call stack (grows down) | | Reserved | 0x10000 | 0x1FFFF | 64 KB | Future use / guard region | |
Runtime Heap | 0x20000 | 0xFFFFF | 896 KB | Runtime allocations |

**Total minimum memory: 1 MB (16 WASM pages)**

______________________________________________________________________

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

| Tag | Type | Payload Meaning | |-----|------|-----------------| | 0 | null | Always 0 | | 1 | bool
| 0 = false, 1 = true | | 2 | int | Signed 32-bit integer value | | 3 | float | Pointer to f64 in
memory | | 4 | string | Pointer to null-terminated string | | 5 | path | Pointer to null-terminated
path string | | 6 | list | Pointer to list header | | 7 | attrset | Pointer to attrset header | | 8
| lambda | Pointer to closure struct | | 9 | thunk | Pointer to thunk struct | | 10 | primop |
Builtin function index |

______________________________________________________________________

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

Used when attribute names are computed at runtime. Same layout as static keys, but entries are
sorted by key string for binary search.

### Closure

```
offset+0:  func_index (i32) - encoded as (module_id << 16) | local_func_index
offset+4:  capture_count (i32)
offset+8:  capture[0] (nix_value, 8 bytes)
offset+16: capture[1] (nix_value, 8 bytes)
...
```

Total size: `8 + capture_count * 8` bytes

### Thunk

```
offset+0:  func_index (i32) - encoded as (module_id << 16) | local_func_index
offset+4:  capture_count (i32)
offset+8:  cached_value (nix_value, 8 bytes) - initially null (tag=0)
offset+16: capture[0] (nix_value, 8 bytes)
offset+24: capture[1] (nix_value, 8 bytes)
...
```

Total size: `16 + capture_count * 8` bytes

When a thunk is forced:

1. Check if `cached_value` is non-null → return it
2. Otherwise, call the thunk function with captures
3. Store result in `cached_value`
4. Return result

______________________________________________________________________

## Initialization Order

At executor startup:

1. Create wasmtime Engine and Store
2. Create WASM Memory (16 pages = 1MB)
3. Create `wasm_memory` with callbacks to access WASM memory
4. Point `runtime_context::mem` to `wasm_memory`
5. Compile and instantiate the WASM module (data segment written to memory)
6. Call `rt_init_builtins()` to allocate builtins attrset on heap
7. Set up WASM function callbacks for lambda/thunk evaluation
8. Execute main function

**Critical**: `rt_init_builtins()` must be called AFTER the module is instantiated, so the builtins
attrset is allocated at the heap base (0x20000) and doesn't conflict with the data segment.

______________________________________________________________________

## Memory Constants

Defined in `memory_layout.h`:

```cpp
constexpr uint32_t DATA_SEGMENT_BASE = 0x00000;
constexpr uint32_t DATA_SEGMENT_LIMIT = 0x10000;  // 64 KB
constexpr uint32_t STACK_TOP = 0x10000;
constexpr uint32_t STACK_SIZE = 0x01000;          // 4 KB
constexpr uint32_t HEAP_BASE = 0x20000;           // 128 KB
constexpr uint32_t DEFAULT_HEAP_SIZE = 0xE0000;   // 896 KB
constexpr uint32_t DEFAULT_MEMORY_SIZE = 0x100000; // 1 MB
constexpr uint32_t WASM_PAGE_SIZE = 0x10000;      // 64 KB
constexpr uint32_t VALUE_SIZE = 8;
constexpr uint32_t ALIGNMENT = 8;
```
