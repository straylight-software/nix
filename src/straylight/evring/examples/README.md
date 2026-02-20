# evring Examples

This directory contains example programs demonstrating the evring library's capabilities.

## Building

All examples can be built with Buck2:

```bash
# Build all examples
buck2 build //src/straylight/evring:example_file_reader
buck2 build //src/straylight/evring:example_file_copy
buck2 build //src/straylight/evring:example_bulk_stat
buck2 build //src/straylight/evring:example_http_get
buck2 build //src/straylight/evring:example_echo_server
buck2 build //src/straylight/evring:example_file_downloader
buck2 build //src/straylight/evring:example_directory_tree
```

## Examples

### File I/O

#### file_reader.cpp

Simple file reading with the state machine pattern. Demonstrates the basic evring state machine
lifecycle: `initial() -> step(state, event) -> done(state)`.

```bash
buck2 run //src/straylight/evring:example_file_reader -- /etc/hosts
```

#### file_copy.cpp

Copy a file with progress reporting. Shows multi-resource state machines (source and dest file
handles) and error handling with cleanup.

```bash
buck2 run //src/straylight/evring:example_file_copy -- source.txt dest.txt
```

#### bulk_stat.cpp

High-throughput file stat using generator machines. Demonstrates `stat_generator_machine` for bulk
operations with minimal state overhead.

```bash
buck2 run //src/straylight/evring:example_bulk_stat -- /usr/lib
```

#### directory_tree.cpp

Recursive directory listing with statx. Prints a tree view with file metadata (permissions, size).

```bash
buck2 run //src/straylight/evring:example_directory_tree -- /path/to/dir
buck2 run //src/straylight/evring:example_directory_tree -- /path/to/dir 3  # max depth 3
```

### Network I/O

#### http_get.cpp

HTTP GET requests using HTTP/1.1 and HTTP/2. Demonstrates:

- URL parsing
- TCP connect via io_uring
- TLS handshake with ALPN negotiation
- HTTP/1.1 client (llhttp)
- HTTP/2 client (nghttp2)

```bash
buck2 run //src/straylight/evring:example_http_get -- https://httpbin.org/get
buck2 run //src/straylight/evring:example_http_get -- -2 https://www.google.com/  # force HTTP/2
buck2 run //src/straylight/evring:example_http_get -- -1 https://httpbin.org/headers  # force HTTP/1.1
```

#### echo_server.cpp

TCP echo server. Demonstrates:

- Socket creation and binding
- Accept loop
- Concurrent client handling
- Recv/send pattern

```bash
buck2 run //src/straylight/evring:example_echo_server -- 8080
# In another terminal: nc localhost 8080
```

### Mixed Workloads

#### file_downloader.cpp

Download a file over HTTPS with progress display. Combines network and file I/O:

- HTTP download via HTTP/1.1
- Chunked file writing with progress bar

```bash
buck2 run //src/straylight/evring:example_file_downloader -- https://httpbin.org/bytes/10240 output.bin
```

## State Machine Pattern

All examples follow the evring state machine pattern:

```cpp
class my_machine {
public:
  using state_type = my_state;

  // Create initial state
  [[nodiscard]] auto initial() const -> state_type;

  // Process event, return new state and operations to submit
  [[nodiscard]] auto step(state_type s, const evring::event& e) const
      -> evring::step_result<state_type>;

  // Check if machine has reached terminal state
  [[nodiscard]] auto done(const state_type& s) const -> bool;
};
```

Run a machine with:

```cpp
auto ring = evring::make_io_uring_ring(256);
my_machine machine{...};
auto final_state = evring::run(machine, *ring);
```

## Key Concepts

- **States must be copyable**: For deterministic replay, states are copied at each step
- **Operations are pure data**: No side effects in step() - operations describe what to do
- **Events carry results**: Each event contains the result of the previous operation
- **Resource handles**: File descriptors are wrapped in `evring::handle` for tracking
