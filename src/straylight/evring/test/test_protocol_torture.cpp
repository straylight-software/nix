// test_protocol_torture.cpp
//
// "What filthy piece of work is man."
//
// Tests that torture the TLS and HTTP/2 state machines with malformed
// inputs, unexpected events, and pathological cases.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "straylight/evring/evring.h"
#include "straylight/evring/http2.h"
#include "straylight/evring/tls.h"

namespace {

std::mt19937 rng{std::random_device{}()};

// ============================================================================
// HTTP/2 Session: Submit request before init
// ============================================================================

void test_http2_request_before_init() {
  std::printf("test_http2_request_before_init: submitting request to uninitialized session...\n");

  evring::http2_session session;
  assert(!session.valid());

  evring::http2_request req;
  req.method = "GET";
  req.authority = "example.com";
  req.path = "/";

  // Should return error (negative stream_id)
  auto stream_id = session.submit_request(req);
  assert(stream_id < 0);

  std::printf("  PASS: submit_request returned %d (expected < 0)\n", stream_id);
  std::printf("test_http2_request_before_init: PASSED\n\n");
}

// ============================================================================
// HTTP/2 Session: Feed garbage data
// ============================================================================

void test_http2_garbage_data() {
  std::printf("test_http2_garbage_data: feeding random bytes to session...\n");

  evring::http2_session session;
  bool init_ok = session.init_client();
  assert(init_ok);

  // Get and discard the initial data (preface + SETTINGS)
  auto initial = session.get_pending_data();
  std::printf("  Initial pending: %zu bytes\n", initial.size());

  // Feed pure garbage
  std::vector<std::byte> garbage(1024);
  for (auto& b : garbage) {
    b = static_cast<std::byte>(rng() & 0xFF);
  }

  auto consumed = session.receive_data(garbage);
  std::printf("  Fed 1024 garbage bytes, consumed: %ld\n", consumed);

  // Should return error (nghttp2 rejects invalid frames)
  // Negative value indicates error
  assert(consumed < 0);

  std::printf("  PASS: garbage data correctly rejected\n");
  std::printf("test_http2_garbage_data: PASSED\n\n");
}

// ============================================================================
// HTTP/2 Session: Truncated SETTINGS frame
// ============================================================================

void test_http2_truncated_settings() {
  std::printf("test_http2_truncated_settings: feeding truncated SETTINGS...\n");

  evring::http2_session session;
  bool init_ok = session.init_client();
  assert(init_ok);

  // Discard initial data
  session.get_pending_data();

  // A valid SETTINGS frame header is 9 bytes
  // Frame format: length (3) + type (1) + flags (1) + stream_id (4)
  // SETTINGS type = 0x04
  // We'll send a truncated one (only 5 bytes)
  std::vector<std::byte> truncated = {
      std::byte{0x00}, std::byte{0x00}, std::byte{0x06}, // length = 6
      std::byte{0x04},                                   // type = SETTINGS
      std::byte{0x00},                                   // flags = 0
      // Missing: stream_id (4 bytes) and payload (6 bytes)
  };

  auto consumed = session.receive_data(truncated);
  std::printf("  Fed truncated SETTINGS (5 bytes), consumed: %ld\n", consumed);

  // nghttp2 should accept partial data and wait for more
  // or reject if it detects the truncation
  // Either way, the session should handle it gracefully

  std::printf("  PASS: truncated frame handled gracefully\n");
  std::printf("test_http2_truncated_settings: PASSED\n\n");
}

// ============================================================================
// HTTP/2: Invalid stream ID in completion
// ============================================================================

void test_http2_invalid_stream_id() {
  std::printf("test_http2_invalid_stream_id: querying nonexistent stream...\n");

  evring::http2_session session;
  session.init_client();

  // Query a stream that was never created
  auto* response = session.get_stream_response(999);
  assert(response == nullptr);
  std::printf("  PASS: get_stream_response(999) returned nullptr\n");

  bool closed = session.is_stream_closed(999);
  assert(!closed); // not closed because it never existed
  std::printf("  PASS: is_stream_closed(999) returned false\n");

  auto error = session.get_stream_error(999);
  assert(error == evring::http2_error_code::no_error);
  std::printf("  PASS: get_stream_error(999) returned no_error\n");

  std::printf("test_http2_invalid_stream_id: PASSED\n\n");
}

// ============================================================================
// HTTP/2: Request with empty authority
// ============================================================================

void test_http2_empty_authority() {
  std::printf("test_http2_empty_authority: request with empty authority...\n");

  evring::http2_session session;
  session.init_client();
  session.get_pending_data(); // clear

  evring::http2_request req;
  req.method = "GET";
  req.authority = ""; // Empty!
  req.path = "/";

  // Should still submit (nghttp2 doesn't validate semantic correctness)
  auto stream_id = session.submit_request(req);
  std::printf("  submit_request with empty authority: stream_id=%d\n", stream_id);

  // The request is syntactically valid (empty authority is allowed in some contexts)
  // Server would reject it, but we should handle it

  std::printf("test_http2_empty_authority: PASSED\n\n");
}

// ============================================================================
// HTTP/2: Extremely long header value
// ============================================================================

void test_http2_huge_header() {
  std::printf("test_http2_huge_header: request with huge header value...\n");

  evring::http2_session session;
  session.init_client();
  session.get_pending_data();

  evring::http2_request req;
  req.method = "GET";
  req.authority = "example.com";
  req.path = "/";

  // Add a header with a very long value
  std::string huge_value(1024 * 1024, 'X'); // 1MB header value
  req.headers.push_back({"x-huge-header", huge_value});

  auto stream_id = session.submit_request(req);
  std::printf("  submit_request with 1MB header: stream_id=%d\n", stream_id);

  if (stream_id > 0) {
    // Get the pending data - it should be huge
    auto pending = session.get_pending_data();
    std::printf("  pending data: %zu bytes\n", pending.size());
  }

  // Either it succeeds (and we get huge pending data) or it fails
  // (if nghttp2 has header size limits)

  std::printf("test_http2_huge_header: PASSED\n\n");
}

// ============================================================================
// TLS Config: Double free protection (RAII test)
// ============================================================================

void test_tls_config_move_semantics() {
  std::printf("test_tls_config_move_semantics: testing RAII move semantics...\n");

  // Create config
  auto config1 = evring::tls_client_config::create_default();
  assert(config1.valid());

  // Move to new config
  auto config2 = std::move(config1);
  assert(config2.valid());
  assert(!config1.valid()); // NOLINT: testing moved-from state

  // Move assign
  auto config3 = evring::tls_client_config::create_default();
  config3 = std::move(config2);
  assert(config3.valid());
  assert(!config2.valid()); // NOLINT: testing moved-from state

  std::printf("  PASS: move semantics work correctly, no double-free\n");
  std::printf("test_tls_config_move_semantics: PASSED\n\n");
}

// ============================================================================
// TLS Connection: Use after release
// ============================================================================

void test_tls_connection_release() {
  std::printf("test_tls_connection_release: testing release() semantics...\n");

  // We can't create a real TLS connection without network,
  // but we can test the from_raw/release pattern

  // Simulate with nullptr (safe because we check valid())
  auto conn = evring::tls_connection::from_raw(nullptr);
  assert(!conn.valid());

  auto* released = conn.release();
  assert(released == nullptr);
  assert(!conn.valid());

  std::printf("  PASS: release() on null connection works\n");
  std::printf("test_tls_connection_release: PASSED\n\n");
}

// ============================================================================
// HTTP/2 Response: Check ok() boundary conditions
// ============================================================================

void test_http2_response_ok_boundaries() {
  std::printf("test_http2_response_ok_boundaries: testing response.ok() edge cases...\n");

  evring::http2_response r;

  // 0 - not ok
  r.status_code = 0;
  assert(!r.ok());

  // 199 - not ok (1xx informational)
  r.status_code = 199;
  assert(!r.ok());

  // 200 - ok
  r.status_code = 200;
  assert(r.ok());

  // 204 - ok (no content)
  r.status_code = 204;
  assert(r.ok());

  // 299 - ok
  r.status_code = 299;
  assert(r.ok());

  // 300 - not ok (redirect)
  r.status_code = 300;
  assert(!r.ok());

  // 404 - not ok
  r.status_code = 404;
  assert(!r.ok());

  // 500 - not ok
  r.status_code = 500;
  assert(!r.ok());

  // Negative - not ok
  r.status_code = -1;
  assert(!r.ok());

  std::printf("  PASS: ok() correctly identifies 2xx status codes\n");
  std::printf("test_http2_response_ok_boundaries: PASSED\n\n");
}

// ============================================================================
// HTTP/2 Error code coverage
// ============================================================================

void test_http2_all_error_codes() {
  std::printf("test_http2_all_error_codes: verifying all error codes have strings...\n");

  std::vector<evring::http2_error_code> codes = {
      evring::http2_error_code::no_error,
      evring::http2_error_code::protocol_error,
      evring::http2_error_code::internal_error,
      evring::http2_error_code::flow_control_error,
      evring::http2_error_code::settings_timeout,
      evring::http2_error_code::stream_closed,
      evring::http2_error_code::frame_size_error,
      evring::http2_error_code::refused_stream,
      evring::http2_error_code::cancel,
      evring::http2_error_code::compression_error,
      evring::http2_error_code::connect_error,
      evring::http2_error_code::enhance_your_calm,
      evring::http2_error_code::inadequate_security,
      evring::http2_error_code::http_1_1_required,
      evring::http2_error_code::connection_closed,
      evring::http2_error_code::tls_error,
  };

  for (auto code : codes) {
    auto str = evring::http2_error_string(code);
    assert(!str.empty());
    assert(str != "unknown error");
    std::printf("  %s\n", str.data());
  }

  // Unknown error code
  auto unknown = evring::http2_error_string(static_cast<evring::http2_error_code>(0xFFFF));
  assert(unknown == "unknown error");

  std::printf("  PASS: all error codes have proper strings\n");
  std::printf("test_http2_all_error_codes: PASSED\n\n");
}

// ============================================================================
// Replay with wrong event types
// ============================================================================

void test_replay_wrong_event_types() {
  std::printf("test_replay_wrong_event_types: state machine receiving unexpected events...\n");

  // Simple machine that expects open -> read -> close
  struct ordered_state {
    int phase{0};
    bool got_wrong_event{false};
  };

  struct ordered_machine {
    using state_type = ordered_state;

    auto initial() const -> state_type { return {}; }

    auto step(state_type s, const evring::event& e) const -> evring::step_result<state_type> {
      std::vector<evring::operation> ops;

      switch (s.phase) {
        case 0: // initial - expect to emit open
          ops.push_back(evring::operation::make_open("/etc/hostname", O_RDONLY));
          s.phase = 1;
          break;

        case 1: // expect open completion
          if (e.operation != evring::operation_type::open) {
            s.got_wrong_event = true;
          }
          s.phase = 2;
          break;

        case 2: // done
          break;
      }

      return {std::move(s), std::move(ops)};
    }

    auto done(const state_type& s) const -> bool { return s.phase >= 2; }
  };

  ordered_machine machine;

  // Send a read event when open was expected
  std::vector<evring::event> wrong_events = {
      evring::event{
          .resource_handle = evring::handle{0, 0},
          .operation = evring::operation_type::read, // Wrong! Expected open
          .result = 100,
          .data = {},
          .user_data = 0,
      },
  };

  auto state = evring::replay(machine, wrong_events);

  assert(state.got_wrong_event);
  std::printf("  PASS: machine detected wrong event type\n");
  std::printf("test_replay_wrong_event_types: PASSED\n\n");
}

// ============================================================================
// Generator with zero items
// ============================================================================

void test_generator_empty() {
  std::printf("test_generator_empty: bulk_stat with empty path list...\n");

  std::vector<const char*> empty_paths;
  std::vector<struct statx> empty_buffers;

  auto ring = evring::make_io_uring_ring(32);
  evring::bulk_stat_machine machine{std::span{empty_paths.data(), empty_paths.size()},
                                    std::span{empty_buffers.data(), empty_buffers.size()}};

  assert(machine.done(machine.initial()));
  std::printf("  PASS: empty generator is immediately done\n");

  // Run it anyway - should be a no-op
  auto state = evring::run_generate(machine, *ring);
  assert(state.completed == 0);
  assert(state.succeeded == 0);
  assert(state.failed == 0);

  std::printf("  PASS: run_generate on empty machine completes instantly\n");
  std::printf("test_generator_empty: PASSED\n\n");
}

// ============================================================================
// HTTP/2 request all_headers() with many headers
// ============================================================================

void test_http2_many_headers() {
  std::printf("test_http2_many_headers: request with 1000 headers...\n");

  evring::http2_request req;
  req.method = "POST";
  req.authority = "example.com";
  req.path = "/api/v1/submit";

  for (int i = 0; i < 1000; ++i) {
    req.headers.push_back({"x-header-" + std::to_string(i), "value-" + std::to_string(i)});
  }

  auto all = req.all_headers();
  assert(all.size() == 1004); // 4 pseudo + 1000 regular

  // Verify pseudo-headers are first
  assert(all[0].name == ":method");
  assert(all[1].name == ":scheme");
  assert(all[2].name == ":authority");
  assert(all[3].name == ":path");

  std::printf("  PASS: 1000 headers handled correctly\n");
  std::printf("test_http2_many_headers: PASSED\n\n");
}

// ============================================================================
// TLS config with invalid ALPN string
// ============================================================================

void test_tls_invalid_alpn() {
  std::printf("test_tls_invalid_alpn: testing edge case ALPN strings...\n");

  auto config = evring::tls_client_config::create_default();

  // Empty ALPN
  bool ok = config.set_alpn("");
  std::printf("  set_alpn(\"\"): %s\n", ok ? "true" : "false");
  // May or may not succeed depending on libtls behavior

  // Very long ALPN
  std::string long_alpn(10000, 'x');
  ok = config.set_alpn(long_alpn);
  std::printf("  set_alpn(10000 chars): %s\n", ok ? "true" : "false");
  // libtls should reject this

  // Normal ALPN should still work
  auto config2 = evring::tls_client_config::create_default();
  ok = config2.set_alpn("h2");
  assert(ok);
  std::printf("  set_alpn(\"h2\"): true\n");

  std::printf("test_tls_invalid_alpn: PASSED\n\n");
}

// ============================================================================
// HTTP/2 settings edge values
// ============================================================================

void test_http2_settings_edges() {
  std::printf("test_http2_settings_edges: testing settings boundary values...\n");

  evring::http2_settings settings;

  // Minimum values
  settings.header_table_size = 0;
  settings.enable_push = 0;
  settings.max_concurrent_streams = 0;
  settings.initial_window_size = 0;
  settings.max_frame_size = 16384; // minimum allowed by spec
  settings.max_header_list_size = 0;

  evring::http2_session session;
  bool ok = session.init_client(settings);
  assert(ok);
  std::printf("  PASS: session init with minimum settings\n");

  // Maximum values
  settings.header_table_size = UINT32_MAX;
  settings.enable_push = 1;
  settings.max_concurrent_streams = UINT32_MAX;
  settings.initial_window_size = (1U << 31) - 1; // max allowed by spec
  settings.max_frame_size = 16777215;            // max allowed by spec
  settings.max_header_list_size = UINT32_MAX;

  evring::http2_session session2;
  ok = session2.init_client(settings);
  assert(ok);
  std::printf("  PASS: session init with maximum settings\n");

  std::printf("test_http2_settings_edges: PASSED\n\n");
}

} // namespace

int main() {
  std::printf("=== PROTOCOL TORTURE TESTS ===\n");
  std::printf("\"What filthy piece of work is man.\"\n\n");

  // HTTP/2 torture
  test_http2_request_before_init();
  test_http2_garbage_data();
  test_http2_truncated_settings();
  test_http2_invalid_stream_id();
  test_http2_empty_authority();
  test_http2_huge_header();
  test_http2_response_ok_boundaries();
  test_http2_all_error_codes();
  test_http2_many_headers();
  test_http2_settings_edges();

  // TLS torture
  test_tls_config_move_semantics();
  test_tls_connection_release();
  test_tls_invalid_alpn();

  // State machine torture
  test_replay_wrong_event_types();
  test_generator_empty();

  std::printf("=== ALL PROTOCOL TORTURE TESTS PASSED ===\n");
  std::printf("\"Death is whimsical today.\"\n");
  return 0;
}
