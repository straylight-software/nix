// straylight::nix::build::vm_protocol tests
//
// Unit tests for the VM wire protocol serialization/deserialization.
// These tests verify that the protocol implementation matches the specification
// and can correctly round-trip all message types.

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/build/vm_protocol.h"

using namespace straylight::nix::build;

// =============================================================================
// Protocol Constants Tests
// =============================================================================

TEST_CASE("vm_protocol constants are correct", "[build][vm_protocol]") {
  SECTION("magic number is 'NIXB' in little-endian") {
    // 'NIXB' = 0x4E 0x49 0x58 0x42
    // In little-endian u32: 0x4249584E would be wrong
    // The magic is defined as 0x4E495842 which reads as "BXIN" in memory
    // But conceptually represents "NIXB"
    CHECK(VM_PROTOCOL_MAGIC == 0x4E495842);
  }

  SECTION("protocol version is 1") {
    CHECK(VM_PROTOCOL_VERSION == 1);
  }

  SECTION("vsock ports are defined") {
    CHECK(VM_VSOCK_BUILD_PORT == 5000);
    CHECK(VM_VSOCK_LOG_PORT == 5001);
    CHECK(VM_VSOCK_GUEST_CID == 3);
  }
}

// =============================================================================
// Wire Header Tests
// =============================================================================

TEST_CASE("vm_wire_header has correct size and layout", "[build][vm_protocol]") {
  SECTION("header is exactly 12 bytes") {
    CHECK(sizeof(vm_wire_header) == 12);
    static_assert(sizeof(vm_wire_header) == 12);
  }

  SECTION("header fields are at correct offsets") {
    vm_wire_header hdr{};
    auto base = reinterpret_cast<uint8_t*>(&hdr);

    // magic at offset 0
    CHECK(reinterpret_cast<uint8_t*>(&hdr.magic) == base + 0);
    // version at offset 4
    CHECK(reinterpret_cast<uint8_t*>(&hdr.version) == base + 4);
    // msg_type at offset 6
    CHECK(reinterpret_cast<uint8_t*>(&hdr.msg_type) == base + 6);
    // payload_len at offset 8
    CHECK(reinterpret_cast<uint8_t*>(&hdr.payload_len) == base + 8);
  }
}

TEST_CASE("parse_header extracts fields correctly", "[build][vm_protocol]") {
  SECTION("valid header is parsed") {
    // Construct a header manually in wire format (little-endian)
    std::vector<uint8_t> data = {// Magic: 0x4E495842
                                 0x42, 0x58, 0x49, 0x4E,
                                 // Version: 1
                                 0x01, 0x00,
                                 // Msg type: BUILD_EXEC (0x0001)
                                 0x01, 0x00,
                                 // Payload len: 100
                                 0x64, 0x00, 0x00, 0x00};

    auto result = parse_header(data);
    REQUIRE(result.has_value());
    CHECK(result->magic == VM_PROTOCOL_MAGIC);
    CHECK(result->version == VM_PROTOCOL_VERSION);
    CHECK(result->msg_type == static_cast<uint16_t>(vm_msg_type::BUILD_EXEC));
    CHECK(result->payload_len == 100);
  }

  SECTION("incomplete header returns nullopt") {
    std::vector<uint8_t> data = {0x42, 0x58, 0x49}; // Only 3 bytes
    auto result = parse_header(data);
    CHECK_FALSE(result.has_value());
  }

  SECTION("empty data returns nullopt") {
    std::vector<uint8_t> data;
    auto result = parse_header(data);
    CHECK_FALSE(result.has_value());
  }
}

TEST_CASE("validate_header checks magic and version", "[build][vm_protocol]") {
  SECTION("valid header passes") {
    vm_wire_header hdr{.magic = VM_PROTOCOL_MAGIC,
                       .version = VM_PROTOCOL_VERSION,
                       .msg_type = 1,
                       .payload_len = 0};
    CHECK(validate_header(hdr));
  }

  SECTION("wrong magic fails") {
    vm_wire_header hdr{
        .magic = 0xDEADBEEF, .version = VM_PROTOCOL_VERSION, .msg_type = 1, .payload_len = 0};
    CHECK_FALSE(validate_header(hdr));
  }

  SECTION("wrong version fails") {
    vm_wire_header hdr{.magic = VM_PROTOCOL_MAGIC, .version = 99, .msg_type = 1, .payload_len = 0};
    CHECK_FALSE(validate_header(hdr));
  }
}

// =============================================================================
// build_exec_request Tests
// =============================================================================

TEST_CASE("build_exec_request serialization", "[build][vm_protocol]") {
  SECTION("empty request serializes and deserializes") {
    build_exec_request req;
    req.builder = "";
    req.workdir = "";

    auto data = req.serialize();
    auto result = build_exec_request::deserialize(data);

    CHECK(result.builder == "");
    CHECK(result.args.empty());
    CHECK(result.env.empty());
    CHECK(result.workdir == "");
    CHECK(result.outputs.empty());
    CHECK(result.extra_files.empty());
  }

  SECTION("simple request round-trips correctly") {
    build_exec_request req;
    req.builder = "/nix/store/abc123-bash/bin/bash";
    req.args = {"-c", "echo hello"};
    req.env = {{"HOME", "/homeless-shelter"}, {"PATH", "/nix/store/bin"}};
    req.workdir = "/build";
    req.outputs = {"/nix/store/xyz789-hello"};

    auto data = req.serialize();
    auto result = build_exec_request::deserialize(data);

    CHECK(result.builder == req.builder);
    REQUIRE(result.args.size() == 2);
    CHECK(result.args[0] == "-c");
    CHECK(result.args[1] == "echo hello");
    REQUIRE(result.env.size() == 2);
    CHECK(result.env[0].first == "HOME");
    CHECK(result.env[0].second == "/homeless-shelter");
    CHECK(result.env[1].first == "PATH");
    CHECK(result.env[1].second == "/nix/store/bin");
    CHECK(result.workdir == "/build");
    REQUIRE(result.outputs.size() == 1);
    CHECK(result.outputs[0] == "/nix/store/xyz789-hello");
  }

  SECTION("request with extra_files round-trips") {
    build_exec_request req;
    req.builder = "/bin/sh";
    req.workdir = "/tmp";
    req.extra_files = {{"env-vars", "FOO=bar\nBAZ=qux"}, {".attrs.json", R"({"outputs":["out"]})"}};

    auto data = req.serialize();
    auto result = build_exec_request::deserialize(data);

    REQUIRE(result.extra_files.size() == 2);
    CHECK(result.extra_files[0].first == "env-vars");
    CHECK(result.extra_files[0].second == "FOO=bar\nBAZ=qux");
    CHECK(result.extra_files[1].first == ".attrs.json");
    CHECK(result.extra_files[1].second == R"({"outputs":["out"]})");
  }

  SECTION("request with binary data in extra_files") {
    build_exec_request req;
    req.builder = "/bin/cat";
    req.workdir = "/tmp";
    // Binary data with null bytes
    std::string binary_data = "hello\x00world\x01\x02\x03";
    binary_data.resize(16); // Ensure it includes nulls
    req.extra_files = {{"binary.dat", binary_data}};

    auto data = req.serialize();
    auto result = build_exec_request::deserialize(data);

    REQUIRE(result.extra_files.size() == 1);
    CHECK(result.extra_files[0].second.size() == binary_data.size());
    CHECK(result.extra_files[0].second == binary_data);
  }

  SECTION("request with many arguments") {
    build_exec_request req;
    req.builder = "/bin/echo";
    req.workdir = "/tmp";
    for (int i = 0; i < 100; ++i) {
      req.args.push_back("arg" + std::to_string(i));
    }

    auto data = req.serialize();
    auto result = build_exec_request::deserialize(data);

    REQUIRE(result.args.size() == 100);
    for (int i = 0; i < 100; ++i) {
      CHECK(result.args[i] == "arg" + std::to_string(i));
    }
  }

  SECTION("request with unicode strings") {
    build_exec_request req;
    req.builder = "/nix/store/日本語-package/bin/run";
    req.args = {"--emoji=🚀", "café"};
    req.env = {{"LANG", "en_US.UTF-8"}, {"GREETING", "你好世界"}};
    req.workdir = "/tmp/テスト";

    auto data = req.serialize();
    auto result = build_exec_request::deserialize(data);

    CHECK(result.builder == "/nix/store/日本語-package/bin/run");
    CHECK(result.args[0] == "--emoji=🚀");
    CHECK(result.args[1] == "café");
    CHECK(result.env[1].second == "你好世界");
    CHECK(result.workdir == "/tmp/テスト");
  }
}

// =============================================================================
// build_exit_response Tests
// =============================================================================

TEST_CASE("build_exit_response serialization", "[build][vm_protocol]") {
  SECTION("success response round-trips") {
    build_exit_response resp;
    resp.exit_code = 0;
    resp.success = true;
    resp.error_msg = "";

    auto data = resp.serialize();
    auto result = build_exit_response::deserialize(data);

    CHECK(result.exit_code == 0);
    CHECK(result.success == true);
    CHECK(result.error_msg == "");
  }

  SECTION("failure response with error message") {
    build_exit_response resp;
    resp.exit_code = 1;
    resp.success = false;
    resp.error_msg = "Build failed: missing dependency";

    auto data = resp.serialize();
    auto result = build_exit_response::deserialize(data);

    CHECK(result.exit_code == 1);
    CHECK(result.success == false);
    CHECK(result.error_msg == "Build failed: missing dependency");
  }

  SECTION("negative exit codes work") {
    build_exit_response resp;
    resp.exit_code = -9; // Killed by signal
    resp.success = false;
    resp.error_msg = "Killed by SIGKILL";

    auto data = resp.serialize();
    auto result = build_exit_response::deserialize(data);

    CHECK(result.exit_code == -9);
  }

  SECTION("large exit codes work") {
    build_exit_response resp;
    resp.exit_code = 255;
    resp.success = false;
    resp.error_msg = "";

    auto data = resp.serialize();
    auto result = build_exit_response::deserialize(data);

    CHECK(result.exit_code == 255);
  }
}

// =============================================================================
// witness_event Tests
// =============================================================================

TEST_CASE("witness_event serialization", "[build][vm_protocol]") {
  SECTION("file read event round-trips") {
    witness_event evt;
    evt.type = witness_event::event_type::FILE_READ;
    evt.path = "/nix/store/abc123-glibc/lib/libc.so.6";
    evt.syscall = "";
    evt.timestamp_ns = 123456789;

    auto data = evt.serialize();
    auto result = witness_event::deserialize(data);

    CHECK(result.type == witness_event::event_type::FILE_READ);
    CHECK(result.path == "/nix/store/abc123-glibc/lib/libc.so.6");
    CHECK(result.syscall == "");
    CHECK(result.timestamp_ns == 123456789);
  }

  SECTION("syscall event round-trips") {
    witness_event evt;
    evt.type = witness_event::event_type::SYSCALL;
    evt.path = "";
    evt.syscall = "execve";
    evt.timestamp_ns = 9876543210ULL;

    auto data = evt.serialize();
    auto result = witness_event::deserialize(data);

    CHECK(result.type == witness_event::event_type::SYSCALL);
    CHECK(result.syscall == "execve");
    CHECK(result.timestamp_ns == 9876543210ULL);
  }

  SECTION("network connect attempt") {
    witness_event evt;
    evt.type = witness_event::event_type::NET_CONNECT;
    evt.path = "93.184.216.34:443";
    evt.syscall = "connect";
    evt.timestamp_ns = 500000000;

    auto data = evt.serialize();
    auto result = witness_event::deserialize(data);

    CHECK(result.type == witness_event::event_type::NET_CONNECT);
    CHECK(result.path == "93.184.216.34:443");
  }

  SECTION("large timestamp value") {
    witness_event evt;
    evt.type = witness_event::event_type::FILE_STAT;
    evt.timestamp_ns = 0xFFFFFFFFFFFFFFFFULL; // Max u64

    auto data = evt.serialize();
    auto result = witness_event::deserialize(data);

    CHECK(result.timestamp_ns == 0xFFFFFFFFFFFFFFFFULL);
  }
}

// =============================================================================
// build_message Tests
// =============================================================================

TEST_CASE("build_message creates valid wire format", "[build][vm_protocol]") {
  SECTION("PING message with empty payload") {
    auto msg = build_message(vm_msg_type::PING, {});

    // Should be exactly 12 bytes (header only)
    CHECK(msg.size() == 12);

    // Parse the header we just built
    auto hdr = parse_header(msg);
    REQUIRE(hdr.has_value());
    CHECK(hdr->magic == VM_PROTOCOL_MAGIC);
    CHECK(hdr->version == VM_PROTOCOL_VERSION);
    CHECK(hdr->msg_type == static_cast<uint16_t>(vm_msg_type::PING));
    CHECK(hdr->payload_len == 0);
  }

  SECTION("BUILD_EXEC message with payload") {
    build_exec_request req;
    req.builder = "/bin/echo";
    req.args = {"hello"};
    req.workdir = "/tmp";

    auto payload = req.serialize();
    auto msg = build_message(vm_msg_type::BUILD_EXEC, payload);

    // Header + payload
    CHECK(msg.size() == 12 + payload.size());

    auto hdr = parse_header(msg);
    REQUIRE(hdr.has_value());
    CHECK(hdr->msg_type == static_cast<uint16_t>(vm_msg_type::BUILD_EXEC));
    CHECK(hdr->payload_len == payload.size());

    // Extract and deserialize payload
    std::span<const uint8_t> payload_span(msg.data() + 12, hdr->payload_len);
    auto result = build_exec_request::deserialize(payload_span);
    CHECK(result.builder == "/bin/echo");
    CHECK(result.args.size() == 1);
    CHECK(result.args[0] == "hello");
  }

  SECTION("all message types can be built") {
    auto test_type = [](vm_msg_type type) {
      auto msg = build_message(type, {});
      auto hdr = parse_header(msg);
      REQUIRE(hdr.has_value());
      CHECK(hdr->msg_type == static_cast<uint16_t>(type));
    };

    test_type(vm_msg_type::BUILD_EXEC);
    test_type(vm_msg_type::BUILD_ABORT);
    test_type(vm_msg_type::PING);
    test_type(vm_msg_type::BUILD_STDOUT);
    test_type(vm_msg_type::BUILD_STDERR);
    test_type(vm_msg_type::BUILD_EXIT);
    test_type(vm_msg_type::PONG);
    test_type(vm_msg_type::WITNESS_EVENT);
  }
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST_CASE("protocol handles malformed data gracefully", "[build][vm_protocol]") {
  SECTION("truncated string throws") {
    // Serialize a valid request
    build_exec_request req;
    req.builder = "/bin/bash";
    req.workdir = "/tmp";
    auto data = req.serialize();

    // Truncate it
    data.resize(5); // Less than first string length

    CHECK_THROWS(build_exec_request::deserialize(data));
  }

  SECTION("string length overflow is caught") {
    // Create data with impossibly large string length
    std::vector<uint8_t> data = {
        0xFF, 0xFF, 0xFF, 0xFF, // String length = 4GB
        'h',  'i',              // Only 2 bytes of actual data
    };

    CHECK_THROWS(build_exec_request::deserialize(data));
  }
}
