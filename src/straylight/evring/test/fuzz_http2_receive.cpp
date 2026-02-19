/// fuzz_http2_receive.cpp - Fuzz harness for HTTP/2 frame parsing
///
/// Target: http2_session::receive_data() which feeds raw bytes to nghttp2
/// Rationale: Network input is attacker-controlled. Malformed frames must not
///            crash, corrupt memory, or cause undefined behavior.
///
/// Compile with libFuzzer:
///   clang++ -fsanitize=fuzzer,address,undefined -std=c++23 \
///           -I../../.. fuzz_http2_receive.cpp http2.cpp \
///           -lnghttp2 -o fuzz_http2_receive
///
/// Compile for AFL:
///   afl-clang++ -std=c++23 -I../../.. fuzz_http2_receive.cpp http2.cpp \
///               -lnghttp2 -o fuzz_http2_receive_afl -DAFL_MAIN
///
/// Corpus seeds:
///   - HTTP/2 preface: "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
///   - SETTINGS frame: [0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00]
///   - HEADERS frame with valid HPACK
///   - DATA frames with various lengths
///
/// Copyright (c) 2024 Straylight
/// SPDX-License-Identifier: MIT

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "straylight/evring/http2.h"

// ============================================================================
// Fuzz strategies
// ============================================================================

namespace {

/// Strategy 1: Feed raw bytes to uninitialized session (should fail gracefully)
void fuzz_uninitialized_session(const std::uint8_t* data, std::size_t size) {
  evring::http2_session session;
  // Don't call init_client() - session is invalid
  // receive_data should handle this gracefully

  std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data), size);
  auto result = session.receive_data(input);

  // Should return error or handle gracefully
  (void)result;
}

/// Strategy 2: Feed raw bytes to initialized session
void fuzz_initialized_session(const std::uint8_t* data, std::size_t size) {
  evring::http2_session session;

  // Initialize with default settings
  if (!session.init_client()) {
    return; // Initialization can fail under memory pressure
  }

  // Install callbacks that exercise response accumulation
  bool headers_received = false;
  bool data_received = false;
  bool stream_closed = false;

  session.set_on_headers([&](std::int32_t stream_id, const evring::http2_headers& headers) {
    headers_received = true;
    // Access header data to ensure no corruption
    for (const auto& h : headers) {
      (void)h.name.size();
      (void)h.value.size();
    }
  });

  session.set_on_data([&](std::int32_t stream_id, std::span<const std::byte> chunk) {
    data_received = true;
    (void)chunk.size();
  });

  session.set_on_stream_close([&](std::int32_t stream_id, evring::http2_error_code error) {
    stream_closed = true;
    (void)evring::http2_error_string(error);
  });

  // Feed fuzzed data
  std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data), size);
  auto result = session.receive_data(input);

  // Validate session state
  (void)session.valid();
  (void)session.wants_read();
  (void)session.wants_write();
  (void)session.local_settings();
  (void)session.remote_settings();
}

/// Strategy 3: Feed data in chunks to test incremental parsing
void fuzz_chunked_receive(const std::uint8_t* data, std::size_t size) {
  evring::http2_session session;

  if (!session.init_client()) {
    return;
  }

  // Feed data in random-ish sized chunks based on input
  std::size_t offset = 0;
  while (offset < size) {
    // Use current byte to determine chunk size (1-256 bytes)
    std::size_t chunk_size = 1;
    if (offset < size) {
      chunk_size = static_cast<std::size_t>(data[offset] % 255) + 1;
    }
    chunk_size = std::min(chunk_size, size - offset);

    std::span<const std::byte> chunk(reinterpret_cast<const std::byte*>(data + offset), chunk_size);
    auto result = session.receive_data(chunk);

    // Check for fatal errors but continue on recoverable ones
    if (result < -1000) {
      break; // Catastrophic error
    }

    offset += chunk_size;
  }

  // Exercise pending data retrieval
  auto pending = session.get_pending_data();
  (void)pending.size();
}

/// Strategy 4: Interleave receive with request submission
void fuzz_interleaved_ops(const std::uint8_t* data, std::size_t size) {
  if (size < 4) {
    return;
  }

  evring::http2_session session;
  if (!session.init_client()) {
    return;
  }

  // Use first byte as operation selector
  std::size_t op_offset = 0;

  while (op_offset < size) {
    std::uint8_t op = data[op_offset++];

    switch (op % 4) {
      case 0: {
        // Submit a request
        evring::http2_request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "fuzz.test";
        req.path = "/";

        // Add headers based on fuzz data
        if (op_offset + 2 <= size) {
          std::size_t name_len = data[op_offset++] % 32;
          std::size_t value_len = data[op_offset++] % 64;

          if (op_offset + name_len + value_len <= size) {
            std::string name(reinterpret_cast<const char*>(data + op_offset), name_len);
            op_offset += name_len;
            std::string value(reinterpret_cast<const char*>(data + op_offset), value_len);
            op_offset += value_len;

            req.headers.push_back({name, value});
          }
        }

        auto stream_id = session.submit_request(req);
        (void)stream_id;
        break;
      }

      case 1: {
        // Feed some data
        std::size_t feed_len = std::min<std::size_t>(64, size - op_offset);
        if (feed_len > 0) {
          std::span<const std::byte> chunk(reinterpret_cast<const std::byte*>(data + op_offset),
                                           feed_len);
          auto result = session.receive_data(chunk);
          (void)result;
          op_offset += feed_len;
        }
        break;
      }

      case 2: {
        // Get pending data
        auto pending = session.get_pending_data();
        (void)pending.size();
        break;
      }

      case 3: {
        // Query stream state
        std::int32_t stream_id = static_cast<std::int32_t>(op % 128);
        (void)session.is_stream_closed(stream_id);
        (void)session.get_stream_error(stream_id);
        (void)session.get_stream_response(stream_id);
        (void)session.get_pending_headers(stream_id);
        break;
      }
    }
  }
}

/// Strategy 5: Test with specific malformed frame patterns
void fuzz_malformed_frames(const std::uint8_t* data, std::size_t size) {
  evring::http2_session session;
  if (!session.init_client()) {
    return;
  }

  // Generate malformed frames based on fuzz input
  std::vector<std::byte> frame;

  if (size >= 3) {
    // Frame header: length (3 bytes) + type (1) + flags (1) + stream_id (4)

    // Use fuzz data to construct frame length (potentially invalid)
    std::uint32_t frame_length = (static_cast<std::uint32_t>(data[0]) << 16) |
                                 (static_cast<std::uint32_t>(data[1]) << 8) |
                                 static_cast<std::uint32_t>(data[2]);

    // Clamp to avoid massive allocations
    frame_length = std::min<std::uint32_t>(frame_length, 16384);

    // Frame type (0-9 are valid, anything else is unknown)
    std::uint8_t frame_type = size > 3 ? data[3] : 0;

    // Flags
    std::uint8_t flags = size > 4 ? data[4] : 0;

    // Stream ID (31 bits)
    std::uint32_t stream_id = 0;
    if (size > 8) {
      stream_id = (static_cast<std::uint32_t>(data[5] & 0x7F) << 24) |
                  (static_cast<std::uint32_t>(data[6]) << 16) |
                  (static_cast<std::uint32_t>(data[7]) << 8) | static_cast<std::uint32_t>(data[8]);
    }

    // Construct frame header
    frame.resize(9 + frame_length);
    frame[0] = static_cast<std::byte>((frame_length >> 16) & 0xFF);
    frame[1] = static_cast<std::byte>((frame_length >> 8) & 0xFF);
    frame[2] = static_cast<std::byte>(frame_length & 0xFF);
    frame[3] = static_cast<std::byte>(frame_type);
    frame[4] = static_cast<std::byte>(flags);
    frame[5] = static_cast<std::byte>((stream_id >> 24) & 0x7F);
    frame[6] = static_cast<std::byte>((stream_id >> 16) & 0xFF);
    frame[7] = static_cast<std::byte>((stream_id >> 8) & 0xFF);
    frame[8] = static_cast<std::byte>(stream_id & 0xFF);

    // Fill payload with fuzz data
    std::size_t copy_len = std::min<std::size_t>(frame_length, size > 9 ? size - 9 : 0);
    if (copy_len > 0) {
      std::memcpy(&frame[9], data + 9, copy_len);
    }

    // Feed the malformed frame
    auto result = session.receive_data(std::span<const std::byte>(frame));
    (void)result;
  }

  // Also feed raw fuzz data
  std::span<const std::byte> raw(reinterpret_cast<const std::byte*>(data), size);
  auto result = session.receive_data(raw);
  (void)result;
}

} // anonymous namespace

// ============================================================================
// Fuzz entry point
// ============================================================================

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return 0;
  }

  // Use first byte to select strategy, rest is payload
  std::uint8_t strategy = data[0] % 6;
  const std::uint8_t* payload = data + 1;
  std::size_t payload_size = size - 1;

  switch (strategy) {
    case 0:
      fuzz_uninitialized_session(payload, payload_size);
      break;
    case 1:
      fuzz_initialized_session(payload, payload_size);
      break;
    case 2:
      fuzz_chunked_receive(payload, payload_size);
      break;
    case 3:
      fuzz_interleaved_ops(payload, payload_size);
      break;
    case 4:
      fuzz_malformed_frames(payload, payload_size);
      break;
    case 5:
      // Combined: all strategies in sequence
      fuzz_uninitialized_session(payload, payload_size);
      fuzz_initialized_session(payload, payload_size);
      fuzz_chunked_receive(payload, payload_size);
      fuzz_interleaved_ops(payload, payload_size);
      fuzz_malformed_frames(payload, payload_size);
      break;
  }

  return 0;
}

// ============================================================================
// AFL main() wrapper
// ============================================================================

#ifdef AFL_MAIN
#  include <cstdio>
#  include <cstdlib>

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
    return 1;
  }

  FILE* f = fopen(argv[1], "rb");
  if (!f) {
    perror("fopen");
    return 1;
  }

  fseek(f, 0, SEEK_END);
  long file_size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (file_size <= 0) {
    fclose(f);
    return 0;
  }

  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(file_size));
  if (fread(buffer.data(), 1, buffer.size(), f) != buffer.size()) {
    fclose(f);
    return 1;
  }
  fclose(f);

  return LLVMFuzzerTestOneInput(buffer.data(), buffer.size());
}
#endif
