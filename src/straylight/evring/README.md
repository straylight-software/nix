# straylight::evring

Deterministic async I/O library built on Linux's io_uring with pure state machine architecture.

## Quick Start

```cpp
#include <straylight/evring/evring.h>

using namespace straylight::evring;

// Create io_uring ring
auto ring = make_io_uring_ring(256);

// Define a state machine
struct my_machine {
  using state_type = my_state;
  auto initial() const -> state_type { return {}; }
  auto step(state_type s, event e) const -> step_result<state_type>;
  auto done(const state_type& s) const -> bool;
};

// Run with real I/O
my_machine machine{};
auto final_state = run(machine, *ring);

// Replay without I/O (for testing)
std::vector<event> recorded_events = {...};
auto replayed_state = replay(machine, recorded_events);
```

## Features

- **Pure state machines**: `State x Event -> State x [Operation]` model
- **Deterministic replay**: Run machines against recorded events without I/O
- **Generator machines**: High-throughput batch operations (93-108% of raw io_uring)
- **Generational handles**: ABA-safe resource tracking via `handle_table<T>`
- **Buffer safety**: `stable_span`/`stable_ref` prevent async buffer corruption
- **TLS support**: libtls integration as state machines
- **HTTP/1.1**: llhttp-based request/response state machines
- **HTTP/2**: nghttp2-based multiplexed streams
- **HTTP/3**: ngtcp2/nghttp3 QUIC-based transport

## API Overview

| Type/Function | Description |
|---------------|-------------|
| `ring` | Abstract I/O ring interface |
| `make_io_uring_ring()` | Create io_uring-backed ring |
| `handle` | Generational resource handle |
| `handle_table<T>` | O(1) handle storage with generation checking |
| `event` | Completion from kernel (result, data, handle) |
| `operation` | Submission to kernel (make_read, make_write, ...) |
| `machine` | State machine concept |
| `generator_machine` | High-throughput batch machine concept |
| `run()` | Execute machine with real I/O |
| `run_traced()` | Execute with event capture |
| `replay()` | Replay machine against events (no I/O) |
| `run_generate()` | Execute generator machine |
| `stable_span<T>` | Buffer span with stable address assertion |
| `tls_handshake_machine` | TLS handshake state machine |
| `http1_tls_client_machine` | HTTP/1.1 over TLS |
| `http2_session` | HTTP/2 session management |
| `http3_session` | HTTP/3 QUIC session |

## Building

```bash
buck2 build //src/straylight/evring:evring
buck2 test //src/straylight/evring/test:...
```

## See Also

- [Architecture](ARCHITECTURE.md)
- [Examples](examples/)
