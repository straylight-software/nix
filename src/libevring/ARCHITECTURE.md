# libevring Architecture

## Overview

libevring is a C++23 library for deterministic async I/O built on Linux's `io_uring`. The core insight is that async programming becomes trivial to test when modeled as pure state machines:

```
State × Event → State × [Operation]
```

## Design Philosophy

### Deterministic by Construction

Traditional async code is hard to test because:
- I/O timing is non-deterministic
- Callbacks create implicit state
- Error paths are hard to exercise

libevring solves this by separating **what** from **how**:
- **Machines** define pure state transitions (testable without I/O)
- **Rings** execute operations against the kernel (real I/O)
- **Replay** runs machines against recorded event streams (no I/O)

### Two APIs for Different Needs

1. **State Machine API** - Deterministic, testable, composable
   - For complex async workflows with dependencies
   - For code that needs to be tested thoroughly
   - Overhead: one state transition per completion

2. **Bulk API** - Maximum throughput, bypasses state machine
   - For batch operations (stat 10k files, copy tree)
   - For performance-critical paths
   - 66x faster than POSIX for metadata operations

## Core Components

### handle.h - Generational Handles

```cpp
struct handle {
  uint32_t index;
  uint32_t generation;
};
```

Prevents ABA problems in async completion dispatch. When a handle is freed and reused, the generation increments, invalidating stale references.

`handle_table<T>` provides O(1) insert/remove/lookup with generation checking.

### event.h - Events and Operations

**Events** are completions from the kernel:
```cpp
struct event {
  handle resource_handle;      // which resource
  operation_type operation;    // what completed
  int64_t result;              // bytes transferred or -errno
  span<const byte> data;       // for reads: the data
  struct statx* statx_buffer;  // for statx: results
  uint64_t user_data;          // correlation token
};
```

**Operations** are submissions to the kernel:
```cpp
struct operation {
  handle resource_handle;
  operation_type type;
  uint64_t user_data;
  variant<...> parameters;     // type-specific params

  // Builders
  static auto make_open(...) -> operation;
  static auto make_read(...) -> operation;
  static auto make_write(...) -> operation;
  // ...
};
```

Supported operation types:
- File: `open`, `openat`, `close`, `read`, `write`, `fsync`, `fdatasync`
- Metadata: `statx`, `fstat`
- Directory: `mkdir`, `mkdirat`, `rmdir`, `unlink`, `unlinkat`, `rename`, `renameat`
- Links: `symlink`, `symlinkat`, `link`, `linkat`, `readlink`
- Timing: `timeout`, `cancel`
- Socket: `connect`, `accept`, `send`, `recv` (planned)

### machine.h - State Machine Concept

```cpp
template <typename M>
concept machine = requires(M m, typename M::state_type s, event e) {
  typename M::state_type;
  { m.initial() } -> same_as<typename M::state_type>;
  { m.step(s, e) } -> same_as<step_result<typename M::state_type>>;
  { m.done(s) } -> same_as<bool>;
};
```

A machine defines:
- `initial()` - Initial state before any I/O
- `step(state, event)` - Pure transition function
- `done(state)` - Termination condition

The `step` function returns:
```cpp
struct step_result<State> {
  State state;
  vector<operation> operations;  // what to submit next
};
```

### ring.h - I/O Execution

```cpp
class ring {
  virtual void enqueue(const operation&) = 0;
  virtual auto submit_and_wait(int min_completions = 1) -> span<event> = 0;
  virtual auto poll() -> span<event> = 0;
  virtual auto register_file_descriptor(int fd, resource_type) -> handle = 0;
  virtual auto get_file_descriptor(handle) const -> int = 0;
  virtual auto pending() const -> size_t = 0;
  virtual auto active_handles() const -> size_t = 0;
};
```

`io_uring_ring` implements this using Linux's `io_uring`:
- Enqueue operations to submission queue
- Submit and wait for completions
- Map between handles and file descriptors

**Ring flags** for performance tuning:
```cpp
enum class ring_flags {
  none,
  sqpoll,        // Kernel-side SQ polling (lowest latency, needs CAP_SYS_NICE)
  iopoll,        // Busy-wait for completions (NVMe)
  single_issuer, // Single thread optimization
  defer_taskrun, // Reduce interrupts
};
```

**Registered resources** for reduced overhead:
- `register_files()` - Pre-register FDs for faster submission
- `register_buffers()` - Pre-register memory for zero-copy I/O

### Execution Functions

```cpp
// Run a machine to completion with real I/O
template <machine M>
auto run(M& machine, ring& ring) -> typename M::state_type;

// Run with event capture for replay
template <machine M>
auto run_traced(M& machine, ring& ring)
    -> pair<typename M::state_type, trace>;

// Replay against captured events (no I/O)
template <machine M>
auto replay(M& machine, span<const event> events)
    -> typename M::state_type;

// Replay with operation capture (for assertions)
template <machine M>
auto replay_with_operations(M& machine, span<const event> events)
    -> pair<typename M::state_type, vector<vector<operation>>>;
```

### bulk.h - High-Performance Batch Operations

Bypasses the state machine for maximum throughput:

```cpp
auto bulk_stat(ring&, span<const char* const> paths,
               span<struct statx> buffers) -> bulk_result;

auto bulk_create_files(ring&, span<const char* const> paths,
                       mode_t mode = 0644) -> bulk_result;

auto copy_file(ring&, const char* source, const char* dest,
               copy_options const& = {}) -> bulk_result;

auto copy_tree(ring&, const char* source, const char* dest,
               copy_tree_options const& = {}) -> copy_tree_result;
```

Key optimizations in bulk operations:
1. Direct `io_uring` access (no ring abstraction overhead)
2. Keep SQ full at all times (maximum inflight operations)
3. Double-buffering for file copy (read-ahead while writing)
4. Minimal memory allocation in hot path

## Example: File Reader Machine

```cpp
struct file_reader_state {
  enum class phase { initial, opening, reading, done, error };
  phase current_phase{phase::initial};
  handle file_handle;
  vector<byte> content;
  vector<byte> read_buffer;
  int error_code{0};
};

struct file_reader_machine {
  using state_type = file_reader_state;
  const char* path_;
  size_t chunk_size_;

  auto initial() -> state_type {
    state_type state;
    state.read_buffer.resize(chunk_size_);
    return state;
  }

  auto step(state_type state, event e) -> step_result<state_type> {
    vector<operation> ops;

    switch (state.current_phase) {
      case phase::initial:
        state.current_phase = phase::opening;
        ops.push_back(operation::make_open(path_, O_RDONLY));
        break;

      case phase::opening:
        if (!e.ok()) {
          state.current_phase = phase::error;
          state.error_code = e.error_code();
        } else {
          state.file_handle = e.resource_handle;
          state.current_phase = phase::reading;
          ops.push_back(operation::make_read(state.file_handle,
              span{state.read_buffer}));
        }
        break;

      case phase::reading:
        if (!e.ok()) {
          state.current_phase = phase::error;
          state.error_code = e.error_code();
        } else if (e.result == 0) {  // EOF
          state.current_phase = phase::done;
          ops.push_back(operation::make_close(state.file_handle));
        } else {
          state.content.insert(state.content.end(),
              e.data.begin(), e.data.end());
          ops.push_back(operation::make_read(state.file_handle,
              span{state.read_buffer}));
        }
        break;

      case phase::done:
      case phase::error:
        break;
    }

    return {move(state), move(ops)};
  }

  auto done(const state_type& s) -> bool {
    return s.current_phase == phase::done ||
           s.current_phase == phase::error;
  }
};

// Real execution
auto ring = make_io_uring_ring(64);
auto final_state = run(file_reader{"/etc/hostname"}, *ring);

// Replay without I/O
vector<event> recorded_events = {...};
auto replayed_state = replay(file_reader{"/etc/hostname"}, recorded_events);
```

## Performance

Benchmarks on typical NVMe SSD:

| Operation          | POSIX      | evring bulk | Speedup |
|--------------------|------------|-------------|---------|
| stat 10k files     | 16k ops/s  | 1M+ ops/s   | 66x     |
| copy 1GB file      | 1.4 GB/s   | 4.2 GB/s    | 3x      |
| create 10k files   | 247k ops/s | 119k ops/s  | 0.5x*   |

*File creation is slower due to open+close overhead per file.

## Build System

Uses Buck2:

```python
cxx_library(
    name = "evring",
    srcs = ["src/bulk.cpp", "src/io_uring_ring.cpp"],
    exported_headers = {...},
    compiler_flags = ["-std=c++23"],
    exported_linker_flags = ["-luring"],
)

cxx_test(name = "test_replay", ...)
cxx_test(name = "test_io_uring", ...)
cxx_binary(name = "bench", ...)
```

## Quirks and Notes

### inflight_operations Vector

In `io_uring_ring.cpp`, we store `inflight_operation` contexts in a vector indexed by submission order. This works because:
- We clear on `pending_count_ == 0`
- Index is stored in SQE user_data
- Simple and fast, but wastes memory if operations complete out of order

A slot-based approach with freelist would be more memory-efficient for long-running rings.

### readlink Fallback

`io_uring` doesn't have native `readlink` support. `bulk_readlink` falls back to synchronous `readlink()`. Could potentially use `openat(O_PATH)` + read from `/proc/self/fd/N`.

### Timeout Storage

`__kernel_timespec` structs for timeouts are stored in a vector (`timeout_specs_`) to keep them alive until completion. Cleared along with `inflight_operations_`.

### Bulk API Creates Own Ring

Bulk operations create their own `io_uring` context internally rather than using the passed ring. This was a simplification but means:
- No resource sharing with machine-based code
- Redundant ring initialization
- The `ring&` parameter is currently unused

### Error Handling

Errors are reported via negative `result` in events (matching kernel convention). The `event::ok()` and `event::error_code()` helpers make this ergonomic:

```cpp
if (!event.ok()) {
  int err = event.error_code();  // positive errno
}
```

---

# HTTP/HTTP2 Plan

## Goals

Add HTTP/1.1 and HTTP/2 support to libevring, fitting the state machine model.

## Prior Art

The old `stellarwind` HTTP/2 implementation (`src-archive-0x01/ps-v4/stellarwind/src/net/http2`) shows the shape:

**Dependencies used:**
- `nghttp2` - HTTP/2 framing, HPACK, stream management
- `libhv` - Event loop (hloop_t, hio_t, timers)
- `openssl` - TLS/ALPN negotiation

**Architecture:**
```
async_http2_client
  └── async_http2_connection (per host:port, wraps nghttp2_session)
        └── http2_stream (per request)
```

**Issues in old implementation:**
- Timer leaks (commented out `htimer_del` due to libhv behavior)
- Inefficient body handling (copies to `shared_ptr<string>`)
- Callback soup (libhv → nghttp2 → user)
- Not testable without real I/O

## Design Principles for libevring HTTP

### 1. State Machine All The Way Down

HTTP connection as a machine:
```cpp
struct http_connection_state {
  // TLS state (if applicable)
  tls_state tls;

  // Protocol state
  variant<http1_state, http2_state> protocol;

  // Pending requests
  deque<http_request> pending;

  // Active streams (HTTP/2) or single request (HTTP/1.1)
  handle_table<stream_state> streams;
};

struct http_connection_machine {
  using state_type = http_connection_state;

  auto step(state_type, event) -> step_result<state_type>;
  // ...
};
```

### 2. TLS as a State Machine Layer

TLS handshake fits naturally:
```cpp
struct tls_state {
  enum class phase {
    handshaking,
    established,
    closing,
    closed
  };

  phase current_phase;
  // OpenSSL BIO buffers for async I/O
  vector<byte> encrypt_buffer;
  vector<byte> decrypt_buffer;
  // ALPN result
  string negotiated_protocol;  // "h2", "http/1.1"
};
```

Events: socket read/write completions
Operations: socket read/write with TLS-processed data

### 3. Protocol Negotiation

After TLS handshake, ALPN tells us which protocol:
```cpp
auto negotiate_protocol(tls_state const& tls) -> variant<http1_state, http2_state> {
  if (tls.negotiated_protocol == "h2") {
    return http2_state{};
  }
  return http1_state{};
}
```

### 4. HTTP/2 Framing

Two options:

**Option A: Use nghttp2**
- Proven, compliant, handles HPACK
- Integrate via callbacks that map to evring events/operations
- nghttp2 manages session state; we wrap it

**Option B: Roll our own**
- More control, simpler integration
- Significant work (HPACK is non-trivial)
- Only worthwhile if we need something nghttp2 can't do

Recommendation: **Start with nghttp2**, consider custom framing later if needed.

### 5. HTTP/2 Stream Management

```cpp
struct http2_stream {
  int32_t stream_id;
  stream_state state;  // idle, open, half_closed_*, closed

  // Request
  http_request request;

  // Response accumulator
  http_response response;
  vector<byte> body_buffer;

  // Flow control
  int32_t local_window;
  int32_t remote_window;

  // Timing
  steady_clock::time_point start_time;
  steady_clock::time_point first_byte_time;
};
```

### 6. Connection Pooling

```cpp
struct http_client_state {
  // Connections by origin
  handle_table<http_connection_state> connections;

  // Mapping: origin -> connection handle
  flat_hash_map<origin, handle> connection_map;

  // Pending requests waiting for connection
  deque<pending_request> waiting;
};
```

### 7. Testing Strategy

The machine model enables:

```cpp
// Test HTTP/2 frame parsing
vector<event> events = {
  // TCP connect succeeded
  event{.operation = connect, .result = 0},
  // TLS handshake data received
  event{.operation = read, .data = tls_server_hello},
  // ...
  // HTTP/2 SETTINGS frame received
  event{.operation = read, .data = h2_settings_frame},
  // Response HEADERS
  event{.operation = read, .data = h2_headers_frame},
  // Response DATA
  event{.operation = read, .data = h2_data_frame},
};

auto final_state = replay(http_machine, events);
assert(final_state.responses[0].status == 200);
```

## Proposed File Structure

```
include/evring/
  http/
    http.h           # Main include
    types.h          # http_request, http_response, etc.
    client.h         # http_client machine
    connection.h     # http_connection machine
    h1/
      parser.h       # HTTP/1.1 parsing
      serializer.h   # HTTP/1.1 serialization
    h2/
      session.h      # HTTP/2 session (wraps nghttp2)
      frame.h        # Frame types
      hpack.h        # Header compression
    tls/
      state.h        # TLS state machine
      context.h      # OpenSSL context management

src/
  http/
    client.cpp
    connection.cpp
    h1_parser.cpp
    h2_session.cpp
    tls.cpp
```

## Open Questions

1. **Socket operations** - `io_uring_ring.cpp` has TODO for connect/accept/send/recv. Need to implement these first.

2. **Buffer management** - HTTP needs efficient buffer handling. Consider:
   - Ring buffer for receive
   - Scatter-gather for send
   - Zero-copy with registered buffers

3. **Timeouts** - Per-request, per-connection, idle timeouts. The `timeout` operation exists but needs integration.

4. **Connection reuse** - HTTP/1.1 keep-alive, HTTP/2 multiplexing. State machine needs to handle connection lifecycle.

5. **Compression** - gzip/deflate/br for response bodies. Probably a separate transform layer.

6. **Redirects** - Automatic redirect following. Could be a wrapper machine.

## Next Steps

1. Implement socket operations in `io_uring_ring.cpp`
2. Design and implement TLS state machine
3. Implement HTTP/1.1 (simpler, validates design)
4. Implement HTTP/2 with nghttp2 integration
5. Add connection pooling
6. Benchmarks against curl, existing HTTP libraries
