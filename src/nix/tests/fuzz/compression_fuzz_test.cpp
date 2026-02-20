// straylight // nix // tests // fuzz
//
// Compression/Decompression Fuzz Tests
//
// Fuzz decompression with malformed input.
// Focus on decompression since that's the attack surface (untrusted input).

#include <string>

#include "nix/tests/property.h"
#include "nix/util/compression.h"
#include "nix/util/error.h"

using namespace nix;

// =============================================================================
// Fuzz decompress with arbitrary input
// =============================================================================

TEST_CASE("fuzz: decompress handles arbitrary input", "[fuzz][compression]") {
  rc::prop("decompress 'none' never crashes", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    try {
      [[maybe_unused]] auto result = decompress("none", input);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });

  rc::prop("decompress 'zstd' never crashes", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    try {
      [[maybe_unused]] auto result = decompress("zstd", input);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz decompress with unknown method
// =============================================================================

TEST_CASE("fuzz: decompress handles unknown methods", "[fuzz][compression]") {
  rc::prop("decompress with arbitrary method never crashes", []() {
    auto method = *rc::gen::arbitrary<std::string>();
    auto input = *rc::gen::arbitrary<std::string>();
    try {
      [[maybe_unused]] auto result = decompress(method, input);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz compress -> decompress roundtrip with arbitrary data
// =============================================================================

TEST_CASE("fuzz: compress roundtrip is lossless", "[fuzz][compression]") {
  rc::prop("none compression roundtrip", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    auto compressed = compress("none", input);
    auto decompressed = decompress("none", compressed);
    RC_ASSERT(decompressed == input);
  });
}

// =============================================================================
// Structured fuzzing: compression headers with garbage
// =============================================================================

TEST_CASE("fuzz: decompress with malformed headers", "[fuzz][compression]") {
  rc::prop("zstd with malformed header", []() {
    // ZSTD magic: 28 B5 2F FD
    auto use_magic = *rc::gen::arbitrary<bool>();
    std::string data;

    if (use_magic) {
      data += "\x28\xB5\x2F\xFD";
    }

    // Add random garbage (limited size to avoid slow decompression attempts)
    auto garbage = *rc::gen::container<std::string>(*rc::gen::inRange<size_t>(0, 256),
                                                    rc::gen::arbitrary<char>());
    data += garbage;

    try {
      [[maybe_unused]] auto result = decompress("zstd", data);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });

  rc::prop("gzip with malformed header", []() {
    // Real gzip starts with 0x1f 0x8b
    auto use_magic = *rc::gen::arbitrary<bool>();
    std::string data;

    if (use_magic) {
      data += "\x1f\x8b";
    }

    auto garbage = *rc::gen::container<std::string>(*rc::gen::inRange<size_t>(0, 256),
                                                    rc::gen::arbitrary<char>());
    data += garbage;

    try {
      [[maybe_unused]] auto result = decompress("gzip", data);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Edge case: empty and small inputs
// =============================================================================

TEST_CASE("fuzz: decompress edge cases", "[fuzz][compression]") {
  // Empty input
  REQUIRE_NOTHROW([]() {
    try {
      decompress("zstd", "");
    } catch (...) {
    }
  }());

  // Single byte
  REQUIRE_NOTHROW([]() {
    try {
      decompress("zstd", "\x00");
    } catch (...) {
    }
  }());

  // Just magic bytes
  REQUIRE_NOTHROW([]() {
    try {
      decompress("zstd", "\x28\xB5\x2F\xFD");
    } catch (...) {
    }
  }());
}
