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

`libevring` solves this by separating **what** from **how**:
- **Machines** define pure state transitions (testable without I/O)
- **Rings** execute operations against the kernel (real I/O)
- **Replay** runs machines against recorded event streams (no I/O)

### Generator Machines for High Throughput

For bulk operations (stat 10k files, copy tree), we use **generator machines** - a variant of regular machines that proactively fill the submission queue:

```cpp
template <typename M>
concept generator_machine = machine<M> && requires(M m, typename M::state_type s, std::size_t max_ops) {
  { m.wants_to_submit(s) } -> std::same_as<bool>;
  { m.generate(s, max_ops) } -> std::same_as<step_result<typename M::state_type>>;
};
```

Key differences from regular machines:
- `wants_to_submit(state)` - Returns true if more work can be generated
- `generate(state, max_ops)` - Produces up to `max_ops` operations without waiting for completions

This achieves the same throughput as bypassing the state machine (93-108% in benchmarks) while remaining fully replayable and testable.

Execution loop (`run_generate`):
1. Fill SQ using `generate()` while `wants_to_submit()` and SQ has space
2. Submit batch to kernel
3. Harvest completions from CQ
4. Feed completions to `step()` as normal
5. Repeat until done

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
  handle resource_handle;           // which resource
  operation_type operation;         // what completed
  std::int64_t result;              // bytes transferred, fd for open/accept, or -errno
  std::span<const std::byte> data;  // for reads: the data (view into caller's buffer)
  struct statx* statx_buffer;       // for statx: results (caller-owned)
  std::uint64_t user_data;          // correlation token

  [[nodiscard]] auto ok() const noexcept -> bool;        // result >= 0
  [[nodiscard]] auto error_code() const noexcept -> int; // positive errno or 0
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
- Socket: `socket`, `connect`, `accept`, `send`, `recv`, `shutdown`, `poll_add`

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

  // Additional methods for generator machines
  virtual auto submit() -> int = 0;                  // submit without waiting
  virtual auto harvest() -> span<event> = 0;         // harvest all ready completions
  virtual auto cq_ready() const -> size_t = 0;       // completions ready to harvest
  virtual auto sq_space() const -> size_t = 0;       // space in submission queue
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

// Run a generator machine (high throughput)
template <generator_machine M>
auto run_generate(M& machine, ring& ring) -> typename M::state_type;

// Run generator with event capture for replay
template <generator_machine M>
auto run_generate_traced(M& machine, ring& ring)
    -> pair<typename M::state_type, trace>;

// Replay generator machine (no I/O)
template <generator_machine M>
auto replay_generate(M& machine, span<const event> events)
    -> typename M::state_type;
```

### bulk.h - Legacy Batch Operations (Deprecated)

> **Deprecated**: Use generator machines instead. The bulk API bypasses the state machine, making operations non-replayable and untestable.

The bulk API exists for backwards compatibility:

```cpp
auto bulk_stat(ring&, span<const char* const> paths,
               span<struct statx> buffers) -> bulk_result;

auto bulk_create_files(ring&, span<const char* const> paths,
                       mode_t mode = 0644) -> bulk_result;
```

The key insight from bulk operations - keeping the SQ full at all times - is now available via generator machines with full replayability.

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

| Operation          | POSIX      | Generator Machine | Speedup |
|--------------------|------------|-------------------|---------|
| stat 10k files     | 16k ops/s  | 1M+ ops/s         | 66x     |
| copy 1GB file      | 1.4 GB/s   | 4.2 GB/s          | 3x      |
| create 10k files   | 247k ops/s | 119k ops/s        | 0.5x*   |

*File creation is slower due to open+close overhead per file.

Generator machines achieve 93-108% of the raw bulk API throughput while remaining fully replayable.

## File Structure

```
evring/
  ARCHITECTURE.md     # This file
  BUCK                # Build configuration
  evring.h            # Main include (aggregates all headers)

  # Core
  handle.h            # Generational handles, handle_table<T>
  event.h             # Events, operations, operation builders
  machine.h           # machine/generator_machine concepts, replay, trace
  ring.h              # ring interface, run/run_traced/run_generate

  # Implementation
  io_uring_ring.cpp   # Linux io_uring implementation of ring
  bulk.cpp            # Deprecated bulk API implementation
  bulk.h              # Deprecated bulk API (use generator machines)
  generators.h        # Generator machines: bulk_stat, bulk_unlink, etc.

  # TLS (libtls)
  tls.h               # TLS config, connection, state machines
  tls.cpp             # libtls integration

  # HTTP/2 (nghttp2)
  http2.h             # HTTP/2 session, request/response, state machines
  http2.cpp           # nghttp2 integration

  # Tests and benchmarks
  test/               # Unit tests
  bench/              # Performance benchmarks
  bin/                # Example binaries
  docs/               # Additional documentation
```

## Build System

Uses Buck2:

```python
cxx_library(
    name = "evring",
    srcs = ["bulk.cpp", "io_uring_ring.cpp", "tls.cpp", "http2.cpp"],
    exported_headers = {...},
    compiler_flags = ["-std=c++23"],
    exported_linker_flags = ["-luring", "-ltls", "-lnghttp2"],
)
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

### Error Handling

Errors are reported via negative `result` in events (matching kernel convention). The `event::ok()` and `event::error_code()` helpers make this ergonomic:

```cpp
if (!event.ok()) {
  int err = event.error_code();  // positive errno
}
```

---

# TLS Layer

## Overview

TLS support is implemented using **libtls** (LibreSSL's simplified TLS API) as state machines that yield `poll_add` operations when the underlying socket needs I/O.

## tls.h - TLS State Machines

### Configuration (RAII wrappers)

```cpp
// Client configuration
class tls_client_config {
  static auto create_default() -> tls_client_config;   // TLS 1.2/1.3, system CAs
  static auto create_insecure() -> tls_client_config;  // No verification (testing only)

  auto set_alpn(string_view protocols) -> bool;  // e.g., "h2,http/1.1"
  auto set_ca_file(const char* path) -> bool;
  auto set_keypair_file(const char* cert, const char* key) -> bool;
};

// Server configuration
class tls_server_config {
  static auto create() -> tls_server_config;
  auto set_alpn(string_view protocols) -> bool;
  auto set_keypair_file(const char* cert, const char* key) -> bool;
};
```

### Connection Context

```cpp
class tls_connection {
  auto alpn_selected() const -> const char*;  // "h2", "http/1.1", or nullptr
  auto version() const -> const char*;        // "TLSv1.3"
  auto cipher() const -> const char*;         // cipher suite name
  auto cipher_strength() const -> int;        // bits
  auto error() const -> const char*;
  auto raw() const noexcept -> struct tls*;   // for direct libtls calls
};
```

### State Machines

| Machine | Purpose |
|---------|---------|
| `tls_handshake_machine` | Client/server TLS handshake with ALPN |
| `tls_read_machine` | Decrypt and read data |
| `tls_write_machine` | Encrypt and write data |
| `tls_close_machine` | TLS shutdown handshake |

All machines yield `poll_add` operations when libtls returns `TLS_WANT_POLLIN` or `TLS_WANT_POLLOUT`.

### Example: TLS Client Handshake

```cpp
auto ring = evring::make_io_uring_ring(256);
auto config = evring::tls_client_config::create_default();
config.set_alpn("h2,http/1.1");

// After TCP connect completes:
evring::tls_handshake_machine handshake{socket_handle, *ring, config, "example.com"};
auto final_state = evring::run(handshake, *ring);

if (final_state.ok()) {
  auto tls_ctx = final_state.take_context();
  // Check negotiated protocol
  if (strcmp(tls_ctx.alpn_selected(), "h2") == 0) {
    // Use HTTP/2
  }
}
```

---

# HTTP/2 Layer

## Overview

HTTP/2 is implemented using **nghttp2** for framing and HPACK, wrapped as evring state machines. The implementation uses libtls for transport.

## http2.h - HTTP/2 State Machines

### Session Management

```cpp
class http2_session {
  auto init_client(const http2_settings& = {}) -> bool;

  // Request submission
  auto submit_request(const http2_request& req) -> int32_t;  // returns stream_id

  // I/O integration
  auto get_pending_data() -> vector<byte>;                    // data to send
  auto receive_data(span<const byte> data) -> int64_t;        // process received data
  auto wants_write() const noexcept -> bool;
  auto wants_read() const noexcept -> bool;

  // Stream tracking
  auto get_stream_response(int32_t stream_id) -> http2_response*;
  auto is_stream_closed(int32_t stream_id) const -> bool;
  auto get_stream_error(int32_t stream_id) const -> http2_error_code;

  // Callbacks for streaming
  using on_headers_callback = function<void(int32_t stream_id, const http2_headers&)>;
  using on_data_callback = function<void(int32_t stream_id, span<const byte>)>;
  using on_stream_close_callback = function<void(int32_t stream_id, http2_error_code)>;
};
```

### Request/Response Types

```cpp
struct http2_request {
  string method = "GET";
  string scheme = "https";
  string authority;  // host:port
  string path = "/";
  http2_headers headers;
  vector<byte> body;
};

struct http2_response {
  int status_code = 0;
  http2_headers headers;
  vector<byte> body;

  auto ok() const noexcept -> bool;  // 2xx status
};
```

### State Machines

| Machine | Purpose |
|---------|---------|
| `http2_connection_machine` | Send client preface + SETTINGS, receive server SETTINGS |
| `http2_request_machine` | Submit request, collect response (multiplexed) |

### Example: HTTP/2 Request

```cpp
auto ring = evring::make_io_uring_ring(256);

// 1. TLS handshake with ALPN
auto tls_config = evring::tls_client_config::create_default();
tls_config.set_alpn("h2");
evring::tls_handshake_machine tls_hs{socket, *ring, tls_config, "example.com"};
auto tls_state = evring::run(tls_hs, *ring);
auto tls_conn = tls_state.take_context();

// 2. HTTP/2 connection setup
evring::http2_session session;
session.init_client();
evring::http2_connection_machine conn{session, tls_conn, socket};
auto conn_state = evring::run(conn, *ring);

// 3. Send request
evring::http2_request req;
req.method = "GET";
req.authority = "example.com";
req.path = "/api/data";
req.headers = {{"accept", "application/json"}};

evring::http2_request_machine request{session, tls_conn, socket, req};
auto resp_state = evring::run(request, *ring);

if (resp_state.ok()) {
  // resp_state.response.status_code, .headers, .body
}
```

### Error Codes (RFC 7540)

```cpp
enum class http2_error_code : uint32_t {
  no_error = 0x0,
  protocol_error = 0x1,
  internal_error = 0x2,
  flow_control_error = 0x3,
  settings_timeout = 0x4,
  stream_closed = 0x5,
  frame_size_error = 0x6,
  refused_stream = 0x7,
  cancel = 0x8,
  compression_error = 0x9,
  connect_error = 0xa,
  enhance_your_calm = 0xb,
  inadequate_security = 0xc,
  http_1_1_required = 0xd,
  // Custom
  connection_closed = 0x100,
  tls_error = 0x101,
};
```

## Implementation Notes

### poll_add Integration

TLS and HTTP/2 machines use `poll_add` operations to wait for socket readiness:
- When `tls_read`/`tls_write` returns `TLS_WANT_POLLIN` → yield `poll_add(POLLIN)`
- When `tls_read`/`tls_write` returns `TLS_WANT_POLLOUT` → yield `poll_add(POLLOUT)`

This integrates cleanly with io_uring's poll mechanism.

### nghttp2 Callbacks

The `http2_session` class registers nghttp2 callbacks that:
- Accumulate headers per stream in `pending_headers_`
- Accumulate response body in `stream_responses_`
- Track stream close events in `closed_streams_`
- Buffer outgoing data in `send_buffer_`

### Replayability

TLS and HTTP/2 machines are replayable for testing:
```cpp
vector<event> events = {...};  // captured poll completions
auto replayed = evring::replay(http2_request_machine{...}, events);
```

## Future Work

- HTTP/1.1 support
- Connection pooling
- Request body streaming (POST/PUT with data provider)
- Automatic redirect following
- Response body decompression (gzip/br)
