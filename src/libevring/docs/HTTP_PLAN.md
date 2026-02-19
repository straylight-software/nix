# libevring HTTP: The Plan

## The Pitch

**What if HTTP clients were as testable as pure functions?**

Every HTTP library is the same story: callbacks, mutable state, mocking nightmares. You want to test your retry logic? Spin up a server. Test timeout handling? Sleep in your tests. Test connection pooling under load? Good luck.

libevring already solved this for file I/O. The same insight applies to networking:

```cpp
// This is a pure function. No sockets. No DNS. No TLS.
auto [new_state, ops] = http_machine.step(state, event);

// Test your entire HTTP stack with fake events
vector<event> scenario = {
  tcp_connect_success(),
  tls_handshake_complete("h2"),
  h2_settings_frame(max_streams=100),
  h2_headers_frame(stream=1, status=200),
  h2_data_frame(stream=1, body="hello"),
};
auto final = replay(http_client, scenario);
assert(final.responses[0].body == "hello");
```

Test edge cases that are nearly impossible otherwise:
- Server sends GOAWAY mid-request
- TLS renegotiation during data transfer
- HTTP/2 flow control backpressure
- Connection dies after headers, before body
- HPACK dynamic table corruption

All without touching the network. All deterministic. All fast.

---

## Architecture Overview

```
                                  ┌─────────────────────────────────────┐
                                  │           http_client               │
                                  │  (connection pooling, redirects)    │
                                  └──────────────┬──────────────────────┘
                                                 │
                          ┌──────────────────────┼──────────────────────┐
                          │                      │                      │
                          ▼                      ▼                      ▼
                 ┌─────────────────┐   ┌─────────────────┐   ┌─────────────────┐
                 │ http_connection │   │ http_connection │   │ http_connection │
                 │   (origin A)    │   │   (origin B)    │   │   (origin C)    │
                 └────────┬────────┘   └────────┬────────┘   └────────┬────────┘
                          │                     │                      │
                          ▼                     ▼                      ▼
                 ┌─────────────────┐   ┌─────────────────┐   ┌─────────────────┐
                 │   tls_layer     │   │   tls_layer     │   │   (plain)       │
                 │  (encrypt/      │   │                 │   │                 │
                 │   decrypt)      │   │                 │   │                 │
                 └────────┬────────┘   └────────┬────────┘   └────────┬────────┘
                          │                     │                      │
                          ▼                     ▼                      ▼
                 ┌─────────────────┐   ┌─────────────────┐   ┌─────────────────┐
                 │  tcp_socket     │   │  tcp_socket     │   │  tcp_socket     │
                 │  (io_uring)     │   │  (io_uring)     │   │  (io_uring)     │
                 └─────────────────┘   └─────────────────┘   └─────────────────┘
```

Each layer is a state machine. Composition via events flowing up, operations flowing down.

---

## Phase 1: Socket Foundation

**Goal:** TCP connect/accept/send/recv via io_uring

**Why first:** Everything else builds on this. Can't do HTTP without sockets.

### 1.1 Socket Operations in io_uring_ring

Add to `io_uring_ring.cpp`:

```cpp
case operation_type::connect: {
  auto& params = get<connect_parameters>(op.parameters);
  io_uring_prep_connect(sqe, fd, params.address, params.address_len);
  break;
}

case operation_type::accept: {
  auto& params = get<accept_parameters>(op.parameters);
  io_uring_prep_accept(sqe, fd, params.address, params.address_len, 0);
  break;
}

case operation_type::send: {
  auto& params = get<send_parameters>(op.parameters);
  io_uring_prep_send(sqe, fd, params.buffer, params.length, params.flags);
  break;
}

case operation_type::recv: {
  auto& params = get<recv_parameters>(op.parameters);
  io_uring_prep_recv(sqe, fd, params.buffer, params.length, params.flags);
  break;
}
```

### 1.2 DNS Resolution

Options:
- **Blocking getaddrinfo** in thread pool (simple, matches most libraries)
- **c-ares** async DNS (complex, but no threads)
- **Stub resolver** parsing /etc/resolv.conf + UDP via io_uring (hardcore mode)

Recommendation: Start with **blocking getaddrinfo** wrapped in a thread, exposed as an event. Upgrade later if needed.

```cpp
struct dns_resolve_operation {
  string hostname;
  uint16_t port;
};

struct dns_resolve_event {
  vector<sockaddr_storage> addresses;  // or error
};
```

### 1.3 TCP Client Machine

```cpp
struct tcp_connection_state {
  enum class phase { resolving, connecting, connected, error, closed };
  phase current;
  handle socket_handle;
  sockaddr_storage peer_addr;
  vector<byte> recv_buffer;
  deque<span<const byte>> send_queue;
};

struct tcp_client_machine {
  using state_type = tcp_connection_state;

  string host;
  uint16_t port;

  auto step(state_type s, event e) -> step_result<state_type> {
    switch (s.current) {
      case phase::resolving:
        // DNS completed -> start connect
      case phase::connecting:
        // connect completed -> ready for send/recv
      case phase::connected:
        // handle send/recv completions
      // ...
    }
  }
};
```

### Deliverables

- [ ] `operation_type::connect`, `accept`, `send`, `recv`, `shutdown`
- [ ] Socket parameter structs in `event.h`
- [ ] `operation::make_connect()`, etc. builders
- [ ] `tcp_client_machine` with DNS resolution
- [ ] `tcp_server_machine` (accept loop)
- [ ] Tests: connect to localhost, echo server
- [ ] Replay tests for connection failure, partial sends

---

## Phase 2: TLS Layer

**Goal:** TLS handshake and encryption as a pure state machine

**Why second:** HTTPS is table stakes. Also unlocks ALPN for HTTP/2 negotiation.

### 2.1 TLS State Machine

```cpp
struct tls_state {
  enum class phase {
    handshaking,     // ClientHello/ServerHello exchange
    established,     // Encrypted channel ready
    shutdown_sent,   // close_notify sent
    shutdown_recv,   // close_notify received
    closed,
    error
  };

  phase current;
  SSL* ssl;                    // OpenSSL handle
  BIO* rbio;                   // Read BIO (network -> OpenSSL)
  BIO* wbio;                   // Write BIO (OpenSSL -> network)

  vector<byte> plaintext_in;   // Decrypted data for upper layer
  vector<byte> plaintext_out;  // Data to encrypt and send
  vector<byte> ciphertext_out; // Encrypted data to send to network

  string alpn_result;          // "h2", "http/1.1", or empty
  string sni_hostname;
  int error_code;
};
```

### 2.2 BIO Integration

OpenSSL's BIO abstraction lets us do async TLS:

```cpp
// When socket has data:
BIO_write(state.rbio, socket_data.data(), socket_data.size());
int n = SSL_read(state.ssl, plaintext_buf, sizeof(plaintext_buf));
// n > 0: decrypted data available
// n == 0: clean shutdown
// n < 0: check SSL_get_error for WANT_READ/WANT_WRITE

// When upper layer has data:
SSL_write(state.ssl, data.data(), data.size());
int pending = BIO_pending(state.wbio);
BIO_read(state.wbio, ciphertext_out.data(), pending);
// Now send ciphertext_out over socket
```

### 2.3 The TLS Machine

```cpp
struct tls_machine {
  using state_type = tls_state;

  SSL_CTX* ctx;  // Shared context with certs, ALPN config

  auto step(state_type s, event e) -> step_result<state_type> {
    vector<operation> ops;

    // Pump data through OpenSSL
    if (e.operation == operation_type::recv && e.result > 0) {
      BIO_write(s.rbio, e.data.data(), e.data.size());
    }

    // Drive the handshake or read/write
    if (s.current == phase::handshaking) {
      int ret = SSL_do_handshake(s.ssl);
      if (ret == 1) {
        s.current = phase::established;
        // Extract ALPN result
        const unsigned char* alpn;
        unsigned int alpn_len;
        SSL_get0_alpn_selected(s.ssl, &alpn, &alpn_len);
        s.alpn_result = string(alpn, alpn + alpn_len);
      }
    }

    // Flush ciphertext to network
    int pending = BIO_pending(s.wbio);
    if (pending > 0) {
      s.ciphertext_out.resize(pending);
      BIO_read(s.wbio, s.ciphertext_out.data(), pending);
      ops.push_back(operation::make_send(socket, s.ciphertext_out));
    }

    // If handshake needs more data, recv
    if (SSL_get_error(s.ssl, ret) == SSL_ERROR_WANT_READ) {
      ops.push_back(operation::make_recv(socket, s.recv_buffer));
    }

    return {move(s), move(ops)};
  }
};
```

### 2.4 Composing TCP + TLS

```cpp
struct tls_connection_state {
  tcp_connection_state tcp;
  tls_state tls;
};

struct tls_connection_machine {
  tcp_client_machine tcp_machine;
  tls_machine tls_machine;

  auto step(state_type s, event e) -> step_result<state_type> {
    // Route events to appropriate layer
    if (is_tcp_event(e)) {
      auto [new_tcp, tcp_ops] = tcp_machine.step(s.tcp, e);
      s.tcp = move(new_tcp);
      // Forward TCP recv data to TLS
      // ...
    }
    // TLS produces operations that become TCP sends
    // TLS produces plaintext that goes to HTTP layer
  }
};
```

### Deliverables

- [ ] `tls_state` and `tls_machine`
- [ ] OpenSSL BIO integration
- [ ] ALPN configuration and extraction
- [ ] SNI support
- [ ] Certificate verification (with bypass for testing)
- [ ] `tls_context` for managing SSL_CTX lifecycle
- [ ] Tests: handshake with real server
- [ ] Replay tests: handshake failure, certificate error, ALPN mismatch

---

## Phase 3: HTTP/1.1

**Goal:** Basic HTTP/1.1 client with keep-alive

**Why before HTTP/2:** Simpler protocol, validates the architecture, still widely needed (HTTP/2 upgrade, fallback).

### 3.1 HTTP Types

```cpp
// include/evring/http/types.h

struct http_header {
  string name;
  string value;
};

struct http_request {
  string method = "GET";
  string path = "/";
  string host;
  vector<http_header> headers;
  vector<byte> body;

  // Computed
  auto content_length() const -> optional<size_t>;
  auto header(string_view name) const -> optional<string_view>;
};

struct http_response {
  int status_code = 0;
  string status_text;
  vector<http_header> headers;
  vector<byte> body;

  auto ok() const -> bool { return status_code >= 200 && status_code < 300; }
  auto content_length() const -> optional<size_t>;
  auto header(string_view name) const -> optional<string_view>;
};
```

### 3.2 HTTP/1.1 Parser

Zero-copy, streaming parser:

```cpp
struct h1_parser {
  enum class state {
    status_line,
    headers,
    body_identity,
    body_chunked,
    chunk_size,
    chunk_data,
    trailers,
    complete,
    error
  };

  state current = state::status_line;
  http_response response;
  size_t content_length = 0;
  size_t body_received = 0;
  size_t chunk_remaining = 0;
  string error_message;

  // Returns bytes consumed
  auto feed(span<const byte> data) -> size_t;

  auto done() const -> bool { return current == state::complete; }
  auto failed() const -> bool { return current == state::error; }
};
```

### 3.3 HTTP/1.1 Serializer

```cpp
struct h1_serializer {
  static auto serialize(http_request const& req) -> vector<byte> {
    string out;
    out += req.method + " " + req.path + " HTTP/1.1\r\n";
    out += "Host: " + req.host + "\r\n";
    for (auto& h : req.headers) {
      out += h.name + ": " + h.value + "\r\n";
    }
    if (!req.body.empty()) {
      out += "Content-Length: " + to_string(req.body.size()) + "\r\n";
    }
    out += "\r\n";
    vector<byte> result(out.begin(), out.end());
    result.insert(result.end(), req.body.begin(), req.body.end());
    return result;
  }
};
```

### 3.4 HTTP/1.1 Connection Machine

```cpp
struct h1_connection_state {
  enum class phase {
    idle,
    sending_request,
    receiving_response,
    error,
    closed
  };

  phase current = phase::idle;
  deque<http_request> pending_requests;
  http_request active_request;
  h1_parser parser;
  http_response completed_response;
  bool keep_alive = true;
};

struct h1_connection_machine {
  using state_type = h1_connection_state;

  auto step(state_type s, event e) -> step_result<state_type> {
    vector<operation> ops;

    switch (s.current) {
      case phase::idle:
        if (!s.pending_requests.empty()) {
          s.active_request = move(s.pending_requests.front());
          s.pending_requests.pop_front();
          s.current = phase::sending_request;
          auto data = h1_serializer::serialize(s.active_request);
          ops.push_back(make_send(socket, data));
        }
        break;

      case phase::sending_request:
        if (e.operation == send && e.ok()) {
          s.current = phase::receiving_response;
          s.parser = h1_parser{};
          ops.push_back(make_recv(socket, recv_buffer));
        }
        break;

      case phase::receiving_response:
        if (e.operation == recv && e.result > 0) {
          s.parser.feed(e.data);
          if (s.parser.done()) {
            s.completed_response = move(s.parser.response);
            s.current = phase::idle;
            // Check Connection: close header
            if (!s.keep_alive) {
              s.current = phase::closed;
            }
          } else if (s.parser.failed()) {
            s.current = phase::error;
          } else {
            ops.push_back(make_recv(socket, recv_buffer));
          }
        }
        break;
    }

    return {move(s), move(ops)};
  }
};
```

### Deliverables

- [ ] `http_request`, `http_response`, `http_header` types
- [ ] `h1_parser` - streaming HTTP/1.1 response parser
- [ ] `h1_serializer` - request serialization
- [ ] `h1_connection_machine` - single connection state machine
- [ ] Keep-alive support
- [ ] Chunked transfer encoding
- [ ] Tests: parse various responses (200, 404, chunked, etc.)
- [ ] Replay tests: partial responses, connection close mid-body

---

## Phase 4: HTTP/2

**Goal:** Full HTTP/2 client with multiplexing

**Why:** Performance. 100 concurrent requests on one connection.

### 4.1 nghttp2 Integration Strategy

nghttp2 is a proven HTTP/2 implementation. We wrap it rather than reimplement:

```cpp
struct h2_session {
  nghttp2_session* session;

  // Callbacks bridge nghttp2 to our event model
  static ssize_t send_callback(nghttp2_session*, const uint8_t* data,
                                size_t len, int flags, void* user);
  static int on_frame_recv(nghttp2_session*, const nghttp2_frame*, void*);
  static int on_data_chunk(nghttp2_session*, uint8_t flags, int32_t stream_id,
                           const uint8_t* data, size_t len, void*);
  static int on_stream_close(nghttp2_session*, int32_t stream_id,
                             uint32_t error_code, void*);
  static int on_header(nghttp2_session*, const nghttp2_frame*,
                       const uint8_t* name, size_t namelen,
                       const uint8_t* value, size_t valuelen,
                       uint8_t flags, void*);
};
```

### 4.2 HTTP/2 State

```cpp
struct h2_stream {
  int32_t id;
  enum class state { idle, open, half_closed_local, half_closed_remote, closed };
  state current;

  http_request request;
  http_response response;
  vector<byte> body_buffer;

  // Flow control
  int32_t local_window;
  int32_t remote_window;
};

struct h2_connection_state {
  enum class phase {
    sending_preface,
    active,
    goaway_sent,
    goaway_received,
    closed,
    error
  };

  phase current = phase::sending_preface;
  h2_session session;

  handle_table<h2_stream> streams;
  flat_hash_map<int32_t, handle> stream_id_to_handle;

  // Peer settings
  uint32_t max_concurrent_streams = 100;
  uint32_t initial_window_size = 65535;
  uint32_t max_frame_size = 16384;

  // Connection-level flow control
  int32_t connection_window;

  // Output buffer (from nghttp2 send callback)
  vector<byte> send_buffer;

  // Completed responses ready for delivery
  deque<pair<handle, http_response>> completed;
};
```

### 4.3 HTTP/2 Machine

```cpp
struct h2_connection_machine {
  using state_type = h2_connection_state;

  auto step(state_type s, event e) -> step_result<state_type> {
    vector<operation> ops;

    switch (s.current) {
      case phase::sending_preface:
        // Send connection preface + SETTINGS
        nghttp2_submit_settings(s.session, ...);
        nghttp2_session_send(s.session);  // Fills send_buffer via callback
        ops.push_back(make_send(socket, s.send_buffer));
        s.current = phase::active;
        ops.push_back(make_recv(socket, recv_buffer));
        break;

      case phase::active:
        if (e.operation == recv && e.result > 0) {
          // Feed data to nghttp2
          nghttp2_session_mem_recv(s.session, e.data.data(), e.data.size());

          // nghttp2 callbacks update our state:
          // - on_header -> s.streams[id].response.headers
          // - on_data_chunk -> s.streams[id].body_buffer
          // - on_stream_close -> move to completed

          // Send any pending frames (WINDOW_UPDATE, etc.)
          nghttp2_session_send(s.session);
          if (!s.send_buffer.empty()) {
            ops.push_back(make_send(socket, s.send_buffer));
            s.send_buffer.clear();
          }

          // Keep receiving
          ops.push_back(make_recv(socket, recv_buffer));
        }
        break;

      case phase::goaway_received:
        // No new streams, drain existing
        break;
    }

    return {move(s), move(ops)};
  }

  // Submit a new request
  static auto submit_request(state_type& s, http_request req) -> handle {
    auto stream_handle = s.streams.insert(h2_stream{});
    auto* stream = s.streams.get(stream_handle);
    stream->request = move(req);

    // Build nghttp2 headers
    vector<nghttp2_nv> nv = /* ... */;
    int32_t stream_id = nghttp2_submit_request(s.session, nullptr,
        nv.data(), nv.size(), nullptr, stream);
    stream->id = stream_id;
    stream->current = h2_stream::state::open;
    s.stream_id_to_handle[stream_id] = stream_handle;

    return stream_handle;
  }
};
```

### 4.4 Flow Control

HTTP/2 flow control is connection-level + stream-level:

```cpp
// In on_data_chunk callback:
stream.body_buffer.insert(end, data, data + len);
stream.local_window -= len;
s.connection_window -= len;

// Periodically send WINDOW_UPDATE
if (stream.local_window < threshold) {
  nghttp2_submit_window_update(s.session, 0, stream.id, increment);
  stream.local_window += increment;
}
```

### 4.5 HPACK

nghttp2 handles HPACK (header compression) internally. We just pass headers in/out.

### Deliverables

- [ ] `h2_session` - nghttp2 wrapper with callbacks
- [ ] `h2_stream` - per-stream state
- [ ] `h2_connection_state` - connection state
- [ ] `h2_connection_machine` - HTTP/2 state machine
- [ ] Stream multiplexing
- [ ] Flow control (connection + stream level)
- [ ] GOAWAY handling (graceful shutdown)
- [ ] Server push handling (or rejection)
- [ ] PING/PONG for keepalive
- [ ] Tests: basic request/response, multiple streams
- [ ] Replay tests: flow control, GOAWAY, RST_STREAM, SETTINGS changes

---

## Phase 5: HTTP Client

**Goal:** High-level client with connection pooling, automatic protocol selection, redirects

### 5.1 Origin-Based Pooling

```cpp
struct origin {
  string scheme;  // "http" or "https"
  string host;
  uint16_t port;

  auto operator==(origin const&) const -> bool = default;
};

template<> struct hash<origin> { /* ... */ };
```

### 5.2 Client State

```cpp
struct http_client_state {
  // Connection pool
  flat_hash_map<origin, vector<handle>> idle_connections;
  handle_table<http_connection> connections;

  // Pending requests waiting for connection
  deque<pending_request> waiting;

  // Configuration
  size_t max_connections_per_origin = 6;
  size_t max_total_connections = 100;
  chrono::seconds idle_timeout{60};
  chrono::seconds connect_timeout{10};
  chrono::seconds request_timeout{30};
  bool follow_redirects = true;
  int max_redirects = 10;
};
```

### 5.3 Connection Lifecycle

```cpp
struct http_connection {
  origin target;
  handle socket;
  optional<tls_state> tls;

  variant<h1_connection_state, h2_connection_state> protocol;

  chrono::steady_clock::time_point last_used;
  size_t requests_served = 0;
};
```

### 5.4 Protocol Selection

```
1. Check scheme (http vs https)
2. If https:
   a. TLS handshake with ALPN offering ["h2", "http/1.1"]
   b. Use negotiated protocol
3. If http:
   a. Default to HTTP/1.1
   b. (Optional: HTTP/2 with prior knowledge via "h2c")
```

### 5.5 Request Flow

```cpp
struct pending_request {
  http_request request;
  handle response_promise;  // For completion notification
  int redirect_count = 0;
};

auto http_client_machine::step(state_type s, event e) -> step_result<state_type> {
  // 1. Route event to appropriate connection
  // 2. Check for completed responses
  // 3. Handle redirects (3xx) by resubmitting
  // 4. Match waiting requests to idle connections
  // 5. Create new connections if needed
  // 6. Close idle connections past timeout
}
```

### 5.6 Redirect Handling

```cpp
auto handle_redirect(http_response const& resp, http_request const& req)
    -> optional<http_request> {
  if (resp.status_code < 300 || resp.status_code >= 400) {
    return nullopt;
  }

  auto location = resp.header("location");
  if (!location) return nullopt;

  http_request new_req = req;
  // Parse location, handle relative URLs
  // 307/308 preserve method and body
  // 301/302/303 convert to GET
  return new_req;
}
```

### Deliverables

- [ ] `http_client_state` and `http_client_machine`
- [ ] Connection pooling by origin
- [ ] Protocol auto-selection via ALPN
- [ ] Automatic redirect following
- [ ] Request/connect/idle timeouts
- [ ] Connection reuse (keep-alive, HTTP/2 multiplexing)
- [ ] Graceful connection shutdown
- [ ] Tests: connection reuse, pool limits
- [ ] Replay tests: redirect chains, mixed HTTP/1.1 and HTTP/2

---

## Phase 6: Bulk HTTP API

**Goal:** High-throughput batch HTTP operations (like bulk_stat for files)

### 6.1 Bulk Fetch

```cpp
struct bulk_fetch_result {
  size_t succeeded = 0;
  size_t failed = 0;
  vector<http_response> responses;
  vector<int> errors;  // indices of failed requests
};

auto bulk_fetch(ring& r, span<http_request> requests,
                bulk_fetch_options const& opts = {}) -> bulk_fetch_result;
```

### 6.2 Parallel Connections

```cpp
struct bulk_fetch_options {
  size_t max_connections = 10;
  size_t max_concurrent_per_connection = 100;  // HTTP/2 streams
  chrono::seconds timeout{30};
  bool fail_fast = false;  // Stop on first error
};
```

### 6.3 Implementation

```cpp
auto bulk_fetch(ring& r, span<http_request> requests,
                bulk_fetch_options const& opts) -> bulk_fetch_result {
  // Group requests by origin
  flat_hash_map<origin, vector<size_t>> by_origin;

  // Create connection pool
  // Submit requests as streams open
  // Harvest responses
  // Very similar to bulk_stat pattern: keep SQ full
}
```

### Deliverables

- [ ] `bulk_fetch()` - parallel HTTP requests
- [ ] `bulk_download()` - fetch to files
- [ ] Automatic connection scaling
- [ ] Progress callbacks
- [ ] Tests: fetch many URLs
- [ ] Benchmarks vs curl, wget, etc.

---

## Phase 7: Polish and Performance

### 7.1 Zero-Copy Receive

Use io_uring's buffer rings for zero-copy receive:

```cpp
// Register a pool of buffers
auto buf_ring = register_buffer_ring(ring, num_buffers, buffer_size);

// Receive into buffer ring
io_uring_prep_recv_multishot(sqe, fd, nullptr, 0, 0);
sqe->buf_group = buf_ring.group_id;

// Completion tells us which buffer was used
buffer_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
```

### 7.2 Multishot Accept

For servers, use multishot accept:

```cpp
io_uring_prep_multishot_accept(sqe, listen_fd, nullptr, nullptr, 0);
// Each CQE is a new connection, keeps re-arming automatically
```

### 7.3 Registered Sockets

Pre-register socket FDs for reduced overhead:

```cpp
auto reg = register_file_slots(ring, 1000);
// Use fixed file index instead of fd in operations
io_uring_prep_recv(sqe, fixed_index, ...);
sqe->flags |= IOSQE_FIXED_FILE;
```

### 7.4 Connection Coalescing

HTTP/2 allows connection coalescing for same-IP different-hostname:

```cpp
// If connection to A.com resolves to 1.2.3.4:443 with cert for *.example.com
// And B.example.com also resolves to 1.2.3.4:443
// We can reuse the connection
```

### Deliverables

- [ ] Zero-copy receive with buffer rings
- [ ] Multishot accept for servers
- [ ] Registered socket FDs
- [ ] Connection coalescing
- [ ] Benchmarks: requests/sec, latency percentiles, memory usage
- [ ] Comparison with beast, nghttp2 examples, curl

---

## Testing Strategy

### Unit Tests (No I/O)

```cpp
// Parser tests
TEST(H1Parser, ChunkedEncoding) {
  h1_parser parser;
  parser.feed("HTTP/1.1 200 OK\r\n"_bytes);
  parser.feed("Transfer-Encoding: chunked\r\n\r\n"_bytes);
  parser.feed("5\r\nhello\r\n"_bytes);
  parser.feed("0\r\n\r\n"_bytes);
  EXPECT_TRUE(parser.done());
  EXPECT_EQ(parser.response.body, "hello"_bytes);
}
```

### Replay Tests (State Machine)

```cpp
// Connection state machine tests
TEST(H2Connection, GoawayMidRequest) {
  vector<event> events = {
    recv(h2_settings_frame()),
    recv(h2_headers_frame(stream=1, status=200)),
    recv(h2_goaway_frame(last_stream=1)),  // Server shutting down
    recv(h2_data_frame(stream=1, data="partial")),
    // Connection should complete stream 1 but reject new requests
  };

  auto state = replay(h2_connection_machine{}, events);
  EXPECT_EQ(state.current, phase::goaway_received);
  EXPECT_EQ(state.completed.size(), 1);
}
```

### Integration Tests (Real I/O)

```cpp
TEST(HttpClient, RealRequest) {
  auto ring = make_io_uring_ring(64);
  auto client = http_client_machine{};
  auto state = client.initial();

  // Submit request
  http_request req{.method = "GET", .host = "httpbin.org", .path = "/get"};
  auto handle = http_client_machine::submit(state, req);

  // Run to completion
  state = run(client, *ring);

  auto* resp = state.get_response(handle);
  EXPECT_EQ(resp->status_code, 200);
}
```

### Fuzz Tests

```cpp
// Fuzz the HTTP/1.1 parser
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  h1_parser parser;
  parser.feed(span{reinterpret_cast<const byte*>(data), size});
  return 0;
}
```

---

## File Structure

```
include/evring/
  net/
    socket.h              # Socket operation types and builders
    tcp.h                 # TCP client/server machines
    dns.h                 # DNS resolution
  tls/
    tls.h                 # TLS state machine
    context.h             # SSL_CTX management
  http/
    http.h                # Main include (pulls in everything)
    types.h               # http_request, http_response
    client.h              # http_client_machine
    h1/
      parser.h            # HTTP/1.1 parser
      connection.h        # HTTP/1.1 connection machine
    h2/
      session.h           # nghttp2 wrapper
      connection.h        # HTTP/2 connection machine
      types.h             # HTTP/2 specific types (stream, settings)

src/
  net/
    socket.cpp            # Socket operations in io_uring
    tcp.cpp               # TCP machines
    dns.cpp               # DNS resolution
  tls/
    tls.cpp               # TLS state machine
    context.cpp           # SSL_CTX setup
  http/
    client.cpp            # HTTP client
    h1_parser.cpp         # HTTP/1.1 parsing
    h1_connection.cpp     # HTTP/1.1 connection
    h2_session.cpp        # nghttp2 integration
    h2_connection.cpp     # HTTP/2 connection
    bulk.cpp              # bulk_fetch, bulk_download

test/
  test_h1_parser.cpp
  test_h2_frames.cpp
  test_tls_handshake.cpp
  test_http_client.cpp
  test_http_replay.cpp

bench/
  bench_http.cpp          # HTTP benchmarks
```

---

## Dependencies

| Dependency | Purpose | Required? |
|------------|---------|-----------|
| liburing   | io_uring interface | Yes (existing) |
| OpenSSL    | TLS (libssl, libcrypto) | Yes |
| nghttp2    | HTTP/2 framing, HPACK | Yes |
| c-ares     | Async DNS | Optional |
| brotli     | Brotli decompression | Optional |
| zlib       | gzip/deflate decompression | Optional |

---

## Timeline Estimate

| Phase | Description | Effort |
|-------|-------------|--------|
| 1 | Socket foundation | 1 week |
| 2 | TLS layer | 1 week |
| 3 | HTTP/1.1 | 1 week |
| 4 | HTTP/2 | 2 weeks |
| 5 | HTTP client | 1 week |
| 6 | Bulk API | 1 week |
| 7 | Polish | 1 week |
| **Total** | | **8 weeks** |

---

## Success Criteria

1. **Testability**: Can test any HTTP scenario without network
2. **Performance**: Competitive with or faster than libcurl
3. **Correctness**: Pass h2spec compliance tests
4. **Simplicity**: Clear, minimal API surface
5. **Composability**: Each layer usable independently

---

## The Payoff

When this is done:

```cpp
// Fetch 1000 URLs in parallel, deterministically testable
auto results = bulk_fetch(ring, urls);

// Test your retry logic without sleeping
auto state = replay(my_http_app, {
  timeout_event(),
  timeout_event(),
  successful_response(200, "finally!"),
});
assert(state.retry_count == 2);

// Reproduce production bugs from event logs
auto events = load_trace("bug_report_12345.trace");
auto state = replay(http_client, events);
// Now step through in debugger
```

The same property that makes libevring great for file I/O - deterministic async via pure state machines - makes it great for networking too.

Let's build it.
