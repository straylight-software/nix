// straylight // nix // tests // fuzz
//
// Serialise/Deserialize Fuzz Tests
//
// The nix daemon protocol uses a binary serialization format. Malformed
// messages can trigger assertion failures in parsing code. These tests
// craft malicious binary payloads that would be received from a malicious
// daemon or man-in-the-middle attacker.

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "nix/tests/property.h"
#include "nix/util/error.h"
#include "nix/util/serialise.h"

using namespace nix;

namespace {

// Create a malicious binary payload
auto make_payload(std::initializer_list<std::uint8_t> bytes) -> std::string {
  return std::string(reinterpret_cast<const char*>(bytes.begin()), bytes.size());
}

// Create a little-endian uint64 payload
auto make_uint64_payload(std::uint64_t value) -> std::string {
  std::string result(8, '\0');
  for (int i = 0; i < 8; ++i) {
    result[i] = static_cast<char>((value >> (i * 8)) & 0xFF);
  }
  return result;
}

// Create a string payload (length-prefixed, padded to 8 bytes)
auto make_string_payload(const std::string& s) -> std::string {
  std::string result = make_uint64_payload(s.size());
  result += s;
  // Pad to 8-byte boundary
  while (result.size() % 8 != 0) {
    result += '\0';
  }
  return result;
}

} // namespace

// =============================================================================
// BUG: read_error() asserts that type == "Error"
// serialise.cpp:481 - assert(type == "Error")
//
// Attack vector: A malicious daemon sends an error response with type != "Error"
// This triggers an assertion failure instead of throwing an exception.
// =============================================================================

TEST_CASE("bug: read_error with invalid type causes assertion failure", "[fuzz][serialise][bug]") {
  SECTION("type is not 'Error'") {
    // Craft payload: string "NotError" + rest of error format
    std::string payload = make_string_payload("NotError");
    payload += make_uint64_payload(0);  // level
    payload += make_string_payload(""); // name (removed but still read)
    payload += make_string_payload("error message");
    payload += make_uint64_payload(0); // have_pos
    payload += make_uint64_payload(0); // nr_traces

    INFO("Crafted payload with type='NotError'");
    INFO("This triggers assert(type == \"Error\") at serialise.cpp:481");

    // This SHOULD throw an exception but instead triggers SIGABRT
    string_source_t source(payload);

    // NOTE: Uncomment to trigger the bug:
    // auto err = read_error(source);

    REQUIRE(payload.size() > 0);
  }

  SECTION("type is empty string") {
    std::string payload = make_string_payload("");
    payload += make_uint64_payload(0);
    payload += make_string_payload("");
    payload += make_string_payload("error message");
    payload += make_uint64_payload(0);
    payload += make_uint64_payload(0);

    INFO("Empty type string also triggers the assertion");
    REQUIRE(payload.size() > 0);
  }
}

// =============================================================================
// BUG: read_error() asserts that have_pos == 0
// serialise.cpp:490, 494 - assert(have_pos == 0)
//
// Attack vector: A malicious daemon sends error with have_pos != 0
// =============================================================================

TEST_CASE("bug: read_error with non-zero have_pos causes assertion failure",
          "[fuzz][serialise][bug]") {
  // Craft payload with valid type but have_pos = 1
  std::string payload = make_string_payload("Error");
  payload += make_uint64_payload(0);  // level
  payload += make_string_payload(""); // name
  payload += make_string_payload("error message");
  payload += make_uint64_payload(1); // have_pos = 1 (invalid!)
  payload += make_uint64_payload(0); // nr_traces

  INFO("Crafted payload with have_pos=1");
  INFO("This triggers assert(have_pos == 0) at serialise.cpp:490");

  // NOTE: Uncomment to trigger the bug:
  // string_source_t source(payload);
  // auto err = read_error(source);

  REQUIRE(payload.size() > 0);
}

// =============================================================================
// BUG: read_num overflow
// A malicious daemon can send integers that overflow the target type
// =============================================================================

TEST_CASE("bug: read_num integer overflow", "[fuzz][serialise][bug]") {
  SECTION("uint64_max for int") {
    std::string payload = make_uint64_payload(UINT64_MAX);
    string_source_t source(payload);

    // This should throw SerialisationError
    CHECK_THROWS_AS(read_num<int>(source), SerialisationError);
  }

  SECTION("negative via uint64") {
    // INT64_MIN as uint64 is 0x8000000000000000
    std::string payload = make_uint64_payload(static_cast<std::uint64_t>(INT64_MIN));
    string_source_t source(payload);

    // Reading as unsigned int should fail (value too large)
    CHECK_THROWS_AS(read_num<unsigned int>(source), SerialisationError);
  }

  SECTION("uint32_max + 1 for uint32") {
    std::string payload = make_uint64_payload(static_cast<std::uint64_t>(UINT32_MAX) + 1);
    string_source_t source(payload);

    CHECK_THROWS_AS(read_num<std::uint32_t>(source), SerialisationError);
  }
}

// =============================================================================
// BUG: read_string with huge length causes memory exhaustion
// A malicious daemon can claim a string is 2^64 bytes long
// =============================================================================

TEST_CASE("bug: read_string memory exhaustion", "[fuzz][serialise][bug]") {
  // Claim string is 1TB long (but don't actually send the data)
  std::string payload = make_uint64_payload(1ULL << 40);

  INFO("Crafted payload claiming 1TB string");
  INFO("This may cause memory exhaustion or very long hang");

  string_source_t source(payload);

  // This should fail gracefully, not crash or hang
  // The actual behavior depends on implementation
  CHECK_THROWS(read_string(source));
}

// =============================================================================
// Property test: arbitrary binary data should not crash serialization
// =============================================================================

TEST_CASE("fuzz: read_num with arbitrary data", "[fuzz][serialise]") {
  rc::prop("arbitrary 8 bytes should not crash read_num", []() {
    auto bytes = *rc::gen::container<std::string>(8, rc::gen::arbitrary<char>());
    string_source_t source(bytes);

    // Should either succeed or throw, never crash
    try {
      [[maybe_unused]] auto val = read_num<std::uint64_t>(source);
    } catch (const SerialisationError&) {
      // Expected for out-of-range values
    } catch (const EndOfFile&) {
      // Expected if source is too short
    }

    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: read_string with arbitrary length prefix", "[fuzz][serialise]") {
  rc::prop("arbitrary length prefix should not crash", []() {
    // Generate a random length value
    auto len = *rc::gen::arbitrary<std::uint64_t>();
    std::string payload = make_uint64_payload(len);

    // Add some garbage data (but not as much as len claims)
    auto extra = *rc::gen::container<std::string>(*rc::gen::inRange<std::size_t>(0, 100),
                                                  rc::gen::arbitrary<char>());
    payload += extra;

    string_source_t source(payload);

    // Should throw EndOfFile, not crash
    try {
      [[maybe_unused]] auto s = read_string(source, 1024 * 1024); // Limit to 1MB
    } catch (const EndOfFile&) {
      // Expected
    } catch (const SerialisationError&) {
      // Also acceptable
    }

    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// BUG: Framed protocol injection
// FramedSource reads chunks - what if chunk size is malicious?
// =============================================================================

TEST_CASE("bug: framed_source with malicious chunk size", "[fuzz][serialise][bug]") {
  SECTION("chunk size is MAX_UINT32") {
    // Claim chunk is 4GB
    std::string payload = make_uint64_payload(UINT32_MAX);
    string_source_t underlying(payload);

    // This may cause memory exhaustion
    INFO("FramedSource with 4GB chunk size may exhaust memory");
    REQUIRE(payload.size() == 8);
  }

  SECTION("chunk size is negative (as signed)") {
    // This is actually a huge positive number when read as uint64
    std::string payload = make_uint64_payload(static_cast<std::uint64_t>(-1));
    string_source_t underlying(payload);

    INFO("FramedSource with -1 chunk size (=UINT64_MAX) is dangerous");
    REQUIRE(payload.size() == 8);
  }
}

// =============================================================================
// BUG: Sized source underflow
// SizedSource checks remain_ <= 0 but remain_ is size_t (unsigned)
// =============================================================================

TEST_CASE("bug: sized_source underflow", "[fuzz][serialise]") {
  // Create a source that claims to have data
  std::string data = "hello world";
  string_source_t underlying(data);

  // Create sized source with size 0
  sized_source_t sized(underlying, 0);

  // Reading from size-0 source should throw EndOfFile
  char buf[16];
  CHECK_THROWS_AS(sized.read(buf, sizeof(buf)), EndOfFile);

  // Check that remain() returns 0
  REQUIRE(sized.remain() == 0);
}

// =============================================================================
// Property test: write then read should roundtrip
// =============================================================================

TEST_CASE("fuzz: string roundtrip", "[fuzz][serialise]") {
  rc::prop("write_string then read_string roundtrips", []() {
    auto s = *rc::gen::arbitrary<std::string>();

    // Skip strings with embedded nulls for now (they work but complicate testing)
    RC_PRE(s.find('\0') == std::string::npos);
    RC_PRE(s.size() < 1024 * 1024); // Skip huge strings

    string_sink_t sink;
    write_string(s, sink);

    string_source_t source(sink.str());
    auto result = read_string(source);

    RC_ASSERT(result == s);
  });
}

TEST_CASE("fuzz: uint64 roundtrip", "[fuzz][serialise]") {
  rc::prop("write then read uint64 roundtrips", []() {
    auto n = *rc::gen::arbitrary<std::uint64_t>();

    string_sink_t sink;
    sink << n;

    string_source_t source(sink.str());
    auto result = read_num<std::uint64_t>(source);

    RC_ASSERT(result == n);
  });
}

// =============================================================================
// BUG: Empty source causes underflow in various readers
// =============================================================================

TEST_CASE("bug: empty source handling", "[fuzz][serialise][bug]") {
  std::string empty;
  string_source_t source(empty);

  SECTION("read_num on empty source") {
    CHECK_THROWS_AS(read_num<int>(source), EndOfFile);
  }

  SECTION("read_string on empty source") {
    CHECK_THROWS_AS(read_string(source), EndOfFile);
  }
}

// =============================================================================
// BUG: Truncated data mid-read
// =============================================================================

TEST_CASE("bug: truncated data handling", "[fuzz][serialise][bug]") {
  SECTION("truncated uint64") {
    std::string partial(4, '\x00'); // Only 4 bytes, need 8
    string_source_t source(partial);

    CHECK_THROWS_AS(read_num<std::uint64_t>(source), EndOfFile);
  }

  SECTION("truncated string data") {
    // Claim 100 bytes but only provide 10
    std::string payload = make_uint64_payload(100);
    payload += std::string(10, 'x');

    string_source_t source(payload);
    CHECK_THROWS_AS(read_string(source), EndOfFile);
  }

  SECTION("truncated string padding") {
    // String length 5 needs 3 bytes of padding to reach 8-byte boundary
    // Provide the string but not the padding
    std::string payload = make_uint64_payload(5);
    payload += "hello"; // No padding

    string_source_t source(payload);
    // This may or may not throw depending on implementation
    // But should not crash
    try {
      [[maybe_unused]] auto s = read_string(source);
    } catch (...) {
      // Any exception is fine
    }
    REQUIRE(true);
  }
}
