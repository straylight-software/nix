// straylight::nix::primitives::serialise tests
//
// Tests for binary serialization framework: Source, Sink, and helpers

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/primitives/serialise.h"

namespace ser = straylight::nix::primitives;

// ─────────────────────────────────────────────────────────────────────────────
// Endianness utilities
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("write_little_endian writes bytes in little-endian order", "[serialise][endian]") {
  std::array<std::byte, 8> buffer{};
  ser::write_little_endian<std::uint64_t>(0x0102030405060708ULL, std::span<std::byte, 8>(buffer));

  REQUIRE(static_cast<std::uint8_t>(buffer[0]) == 0x08);
  REQUIRE(static_cast<std::uint8_t>(buffer[1]) == 0x07);
  REQUIRE(static_cast<std::uint8_t>(buffer[2]) == 0x06);
  REQUIRE(static_cast<std::uint8_t>(buffer[3]) == 0x05);
  REQUIRE(static_cast<std::uint8_t>(buffer[4]) == 0x04);
  REQUIRE(static_cast<std::uint8_t>(buffer[5]) == 0x03);
  REQUIRE(static_cast<std::uint8_t>(buffer[6]) == 0x02);
  REQUIRE(static_cast<std::uint8_t>(buffer[7]) == 0x01);
}

TEST_CASE("read_little_endian reads bytes in little-endian order", "[serialise][endian]") {
  std::array<std::byte, 8> buffer{std::byte{0x08}, std::byte{0x07}, std::byte{0x06},
                                  std::byte{0x05}, std::byte{0x04}, std::byte{0x03},
                                  std::byte{0x02}, std::byte{0x01}};

  std::uint64_t value =
      ser::read_little_endian<std::uint64_t>(std::span<const std::byte, 8>(buffer));
  REQUIRE(value == 0x0102030405060708ULL);
}

TEST_CASE("write and read little_endian round-trip", "[serialise][endian]") {
  std::array<std::byte, 8> buffer{};

  SECTION("zero") {
    ser::write_little_endian<std::uint64_t>(0, std::span<std::byte, 8>(buffer));
    REQUIRE(ser::read_little_endian<std::uint64_t>(std::span<const std::byte, 8>(buffer)) == 0);
  }

  SECTION("max value") {
    ser::write_little_endian<std::uint64_t>(std::numeric_limits<std::uint64_t>::max(),
                                            std::span<std::byte, 8>(buffer));
    REQUIRE(ser::read_little_endian<std::uint64_t>(std::span<const std::byte, 8>(buffer)) ==
            std::numeric_limits<std::uint64_t>::max());
  }

  SECTION("arbitrary value") {
    constexpr std::uint64_t test_value = 0xDEADBEEFCAFEBABEULL;
    ser::write_little_endian<std::uint64_t>(test_value, std::span<std::byte, 8>(buffer));
    REQUIRE(ser::read_little_endian<std::uint64_t>(std::span<const std::byte, 8>(buffer)) ==
            test_value);
  }
}

TEST_CASE("read_little_endian from unsigned char buffer", "[serialise][endian]") {
  unsigned char buffer[8] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
  std::uint64_t value = ser::read_little_endian<std::uint64_t>(buffer);
  REQUIRE(value == 0x0102030405060708ULL);
}

// ─────────────────────────────────────────────────────────────────────────────
// Varint encoding/decoding
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("encode_varint encodes single-byte values", "[serialise][varint]") {
  std::array<std::byte, 10> buffer{};

  SECTION("zero") {
    std::size_t length = ser::encode_varint<std::uint64_t>(0, buffer);
    REQUIRE(length == 1);
    REQUIRE(static_cast<std::uint8_t>(buffer[0]) == 0x00);
  }

  SECTION("small value") {
    std::size_t length = ser::encode_varint<std::uint64_t>(127, buffer);
    REQUIRE(length == 1);
    REQUIRE(static_cast<std::uint8_t>(buffer[0]) == 0x7F);
  }
}

TEST_CASE("encode_varint encodes multi-byte values", "[serialise][varint]") {
  std::array<std::byte, 10> buffer{};

  SECTION("128 requires two bytes") {
    std::size_t length = ser::encode_varint<std::uint64_t>(128, buffer);
    REQUIRE(length == 2);
    REQUIRE(static_cast<std::uint8_t>(buffer[0]) == 0x80);
    REQUIRE(static_cast<std::uint8_t>(buffer[1]) == 0x01);
  }

  SECTION("300 requires two bytes") {
    std::size_t length = ser::encode_varint<std::uint64_t>(300, buffer);
    REQUIRE(length == 2);
    // 300 = 0x12C = 10101100 (low 7 bits) + 00000010 (high bits)
    REQUIRE(static_cast<std::uint8_t>(buffer[0]) == 0xAC);
    REQUIRE(static_cast<std::uint8_t>(buffer[1]) == 0x02);
  }
}

TEST_CASE("decode_varint decodes single-byte values", "[serialise][varint]") {
  SECTION("zero") {
    std::array<std::byte, 1> buffer{std::byte{0x00}};
    auto result = ser::decode_varint<std::uint64_t>(buffer);
    REQUIRE(result.has_value());
    REQUIRE(result->value == 0);
    REQUIRE(result->bytes_consumed == 1);
  }

  SECTION("127") {
    std::array<std::byte, 1> buffer{std::byte{0x7F}};
    auto result = ser::decode_varint<std::uint64_t>(buffer);
    REQUIRE(result.has_value());
    REQUIRE(result->value == 127);
    REQUIRE(result->bytes_consumed == 1);
  }
}

TEST_CASE("decode_varint decodes multi-byte values", "[serialise][varint]") {
  SECTION("128") {
    std::array<std::byte, 2> buffer{std::byte{0x80}, std::byte{0x01}};
    auto result = ser::decode_varint<std::uint64_t>(buffer);
    REQUIRE(result.has_value());
    REQUIRE(result->value == 128);
    REQUIRE(result->bytes_consumed == 2);
  }

  SECTION("300") {
    std::array<std::byte, 2> buffer{std::byte{0xAC}, std::byte{0x02}};
    auto result = ser::decode_varint<std::uint64_t>(buffer);
    REQUIRE(result.has_value());
    REQUIRE(result->value == 300);
    REQUIRE(result->bytes_consumed == 2);
  }
}

TEST_CASE("decode_varint returns nullopt on incomplete data", "[serialise][varint]") {
  std::array<std::byte, 1> buffer{std::byte{0x80}}; // Continuation bit set, but no more bytes
  auto result = ser::decode_varint<std::uint64_t>(buffer);
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("varint encode/decode round-trip", "[serialise][varint]") {
  std::array<std::byte, 10> buffer{};

  SECTION("various values") {
    std::vector<std::uint64_t> test_values = {
        0, 1, 127, 128, 255, 256, 16383, 16384, 1000000, std::numeric_limits<std::uint32_t>::max()};

    for (std::uint64_t value : test_values) {
      std::size_t encoded_length = ser::encode_varint(value, buffer);
      auto decoded = ser::decode_varint<std::uint64_t>(
          std::span<const std::byte>(buffer.data(), encoded_length));
      REQUIRE(decoded.has_value());
      REQUIRE(decoded->value == value);
      REQUIRE(decoded->bytes_consumed == encoded_length);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// StringSink
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("StringSink accumulates data", "[serialise][stringsink]") {
  ser::StringSink sink;
  sink.write("hello");
  sink.write(" ");
  sink.write("world");

  REQUIRE(sink.data() == "hello world");
  REQUIRE(sink.size() == 11);
}

TEST_CASE("StringSink with reserved capacity", "[serialise][stringsink]") {
  ser::StringSink sink(1024);
  sink.write("test");
  REQUIRE(sink.data() == "test");
  REQUIRE(sink.data().capacity() >= 1024);
}

TEST_CASE("StringSink extract moves data out", "[serialise][stringsink]") {
  ser::StringSink sink;
  sink.write("hello");
  std::string extracted = sink.extract();
  REQUIRE(extracted == "hello");
  REQUIRE(sink.size() == 0);
}

TEST_CASE("StringSink clear empties data", "[serialise][stringsink]") {
  ser::StringSink sink;
  sink.write("hello");
  sink.clear();
  REQUIRE(sink.size() == 0);
  REQUIRE(sink.data().empty());
}

TEST_CASE("StringSink good() always returns true", "[serialise][stringsink]") {
  ser::StringSink sink;
  REQUIRE(sink.good());
  sink.write("test");
  REQUIRE(sink.good());
}

// ─────────────────────────────────────────────────────────────────────────────
// StringSource
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("StringSource reads data", "[serialise][stringsource]") {
  std::string data = "hello world";
  ser::StringSource source(data);

  std::array<std::byte, 5> buffer{};
  std::size_t bytes_read = source.read(buffer);
  REQUIRE(bytes_read == 5);
  REQUIRE(std::memcmp(buffer.data(), "hello", 5) == 0);
}

TEST_CASE("StringSource read_exact reads exact bytes", "[serialise][stringsource]") {
  std::string data = "hello world";
  ser::StringSource source(data);

  std::array<std::byte, 5> buffer{};
  source.read_exact(buffer);
  REQUIRE(std::memcmp(buffer.data(), "hello", 5) == 0);
}

TEST_CASE("StringSource throws EndOfFile when exhausted", "[serialise][stringsource]") {
  std::string data = "hi";
  ser::StringSource source(data);

  std::array<std::byte, 2> buffer{};
  source.read(buffer);

  REQUIRE_THROWS_AS(source.read(buffer), ser::EndOfFile);
}

TEST_CASE("StringSource skip advances position", "[serialise][stringsource]") {
  std::string data = "hello world";
  ser::StringSource source(data);

  source.skip(6);
  REQUIRE(source.position() == 6);
  REQUIRE(source.remaining() == 5);

  std::array<std::byte, 5> buffer{};
  source.read_exact(buffer);
  REQUIRE(std::memcmp(buffer.data(), "world", 5) == 0);
}

TEST_CASE("StringSource restart resets position", "[serialise][stringsource]") {
  std::string data = "hello";
  ser::StringSource source(data);

  std::array<std::byte, 5> buffer{};
  source.read(buffer);
  REQUIRE(source.remaining() == 0);

  source.restart();
  REQUIRE(source.position() == 0);
  REQUIRE(source.remaining() == 5);
}

TEST_CASE("StringSource drain reads all remaining data", "[serialise][stringsource]") {
  std::string data = "hello world";
  ser::StringSource source(data);

  source.skip(6);
  std::string remaining = source.drain();
  REQUIRE(remaining == "world");
}

// ─────────────────────────────────────────────────────────────────────────────
// BufferedSink
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("BufferedSink buffers small writes", "[serialise][bufferedsink]") {
  ser::StringSink underlying;
  ser::BufferedSink sink(underlying, 1024);

  sink.write("hello");
  // Data should still be in buffer
  REQUIRE(underlying.size() == 0);

  sink.flush();
  REQUIRE(underlying.data() == "hello");
}

TEST_CASE("BufferedSink flushes when buffer is full", "[serialise][bufferedsink]") {
  ser::StringSink underlying;
  ser::BufferedSink sink(underlying, 10);

  sink.write("hello");
  REQUIRE(underlying.size() == 0); // Still buffered

  sink.write("world!"); // 11 bytes total, exceeds buffer
  // Original buffer should be flushed
  REQUIRE(underlying.data() == "hello");

  sink.flush();
  REQUIRE(underlying.data() == "helloworld!");
}

TEST_CASE("BufferedSink bypasses buffer for large writes", "[serialise][bufferedsink]") {
  ser::StringSink underlying;
  ser::BufferedSink sink(underlying, 10);

  std::string large_data(100, 'x');
  sink.write(large_data);

  REQUIRE(underlying.data() == large_data);
}

// ─────────────────────────────────────────────────────────────────────────────
// BufferedSource
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("BufferedSource buffers reads", "[serialise][bufferedsource]") {
  std::string data = "hello world this is a test";
  ser::StringSource underlying(data);
  ser::BufferedSource source(underlying, 1024);

  std::array<std::byte, 5> buffer{};
  std::size_t bytes_read = source.read(buffer);
  REQUIRE(bytes_read == 5);
  REQUIRE(std::memcmp(buffer.data(), "hello", 5) == 0);

  bytes_read = source.read(buffer);
  REQUIRE(bytes_read == 5);
  REQUIRE(std::memcmp(buffer.data(), " worl", 5) == 0);
}

TEST_CASE("BufferedSource has_data reflects buffer state", "[serialise][bufferedsource]") {
  std::string data = "hello";
  ser::StringSource underlying(data);
  ser::BufferedSource source(underlying, 1024);

  REQUIRE_FALSE(source.has_data()); // Initially empty

  std::array<std::byte, 2> buffer{};
  source.read(buffer);
  REQUIRE(source.has_data()); // Buffer has more data
}

// ─────────────────────────────────────────────────────────────────────────────
// NullSink
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("NullSink discards data", "[serialise][nullsink]") {
  ser::NullSink sink;
  sink.write("hello");
  sink.write("world");
  REQUIRE(sink.good());
}

// ─────────────────────────────────────────────────────────────────────────────
// LengthSink
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("LengthSink counts bytes", "[serialise][lengthsink]") {
  ser::LengthSink sink;
  sink.write("hello");
  REQUIRE(sink.length() == 5);

  sink.write("world!");
  REQUIRE(sink.length() == 11);
}

TEST_CASE("LengthSink reset clears count", "[serialise][lengthsink]") {
  ser::LengthSink sink;
  sink.write("hello");
  sink.reset();
  REQUIRE(sink.length() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// TeeSink
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TeeSink writes to both sinks", "[serialise][teesink]") {
  ser::StringSink sink1;
  ser::StringSink sink2;
  ser::TeeSink tee(sink1, sink2);

  tee.write("hello");
  REQUIRE(sink1.data() == "hello");
  REQUIRE(sink2.data() == "hello");
}

TEST_CASE("TeeSink good reflects both sinks", "[serialise][teesink]") {
  ser::StringSink sink1;
  ser::StringSink sink2;
  ser::TeeSink tee(sink1, sink2);

  REQUIRE(tee.good());
}

// ─────────────────────────────────────────────────────────────────────────────
// TeeSource
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TeeSource copies data to sink", "[serialise][teesource]") {
  std::string data = "hello world";
  ser::StringSource source(data);
  ser::StringSink sink;
  ser::TeeSource tee(source, sink);

  std::array<std::byte, 5> buffer{};
  tee.read(buffer);
  REQUIRE(sink.data() == "hello");
}

// ─────────────────────────────────────────────────────────────────────────────
// LengthSource
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("LengthSource counts bytes read", "[serialise][lengthsource]") {
  std::string data = "hello world";
  ser::StringSource source(data);
  ser::LengthSource counted(source);

  std::array<std::byte, 5> buffer{};
  counted.read(buffer);
  REQUIRE(counted.total() == 5);

  counted.read(buffer);
  REQUIRE(counted.total() == 10);
}

TEST_CASE("LengthSource reset clears count", "[serialise][lengthsource]") {
  std::string data = "hello";
  ser::StringSource source(data);
  ser::LengthSource counted(source);

  std::array<std::byte, 5> buffer{};
  counted.read(buffer);
  counted.reset();
  REQUIRE(counted.total() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// SizedSource
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SizedSource limits bytes read", "[serialise][sizedsource]") {
  std::string data = "hello world";
  ser::StringSource source(data);
  ser::SizedSource sized(source, 5);

  std::array<std::byte, 10> buffer{};
  std::size_t bytes_read = sized.read(buffer);
  REQUIRE(bytes_read == 5);
  REQUIRE(sized.remaining() == 0);

  REQUIRE_THROWS_AS(sized.read(buffer), ser::EndOfFile);
}

TEST_CASE("SizedSource drain_all reads remaining limit", "[serialise][sizedsource]") {
  std::string data = "hello world";
  ser::StringSource source(data);
  ser::SizedSource sized(source, 11);

  std::array<std::byte, 5> buffer{};
  sized.read(buffer);
  REQUIRE(sized.remaining() == 6);

  std::size_t drained = sized.drain_all();
  REQUIRE(drained == 6);
  REQUIRE(sized.remaining() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// write_int / read_int
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("write_int and read_int round-trip uint64", "[serialise][int]") {
  ser::StringSink sink;
  ser::write_int(sink, std::uint64_t{0x0102030405060708ULL});

  ser::StringSource source(sink.data());
  std::uint64_t value = ser::read_int<std::uint64_t>(source);
  REQUIRE(value == 0x0102030405060708ULL);
}

TEST_CASE("write_int and read_int round-trip various types", "[serialise][int]") {
  SECTION("uint32") {
    ser::StringSink sink;
    ser::write_int(sink, std::uint32_t{12345});
    ser::StringSource source(sink.data());
    REQUIRE(ser::read_int<std::uint32_t>(source) == 12345);
  }

  SECTION("uint16") {
    ser::StringSink sink;
    ser::write_int(sink, std::uint16_t{65535});
    ser::StringSource source(sink.data());
    REQUIRE(ser::read_int<std::uint16_t>(source) == 65535);
  }

  SECTION("uint8") {
    ser::StringSink sink;
    ser::write_int(sink, std::uint8_t{255});
    ser::StringSource source(sink.data());
    REQUIRE(ser::read_int<std::uint8_t>(source) == 255);
  }
}

TEST_CASE("read_int throws on overflow", "[serialise][int]") {
  ser::StringSink sink;
  ser::write_int(sink, std::uint64_t{1000000});

  ser::StringSource source(sink.data());
  REQUIRE_THROWS_AS(ser::read_int<std::uint8_t>(source), ser::SerialisationError);
}

TEST_CASE("read_uint64 and read_uint32 helpers", "[serialise][int]") {
  ser::StringSink sink;
  ser::write_int(sink, std::uint64_t{42});
  ser::write_int(sink, std::uint64_t{100});

  ser::StringSource source(sink.data());
  REQUIRE(ser::read_uint64(source) == 42);
  REQUIRE(ser::read_uint32(source) == 100);
}

// ─────────────────────────────────────────────────────────────────────────────
// write_varint / read_varint
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("write_varint and read_varint round-trip", "[serialise][varint]") {
  std::vector<std::uint64_t> test_values = {0,
                                            1,
                                            127,
                                            128,
                                            255,
                                            256,
                                            16383,
                                            16384,
                                            1000000,
                                            1ULL << 32,
                                            std::numeric_limits<std::uint64_t>::max()};

  for (std::uint64_t value : test_values) {
    ser::StringSink sink;
    ser::write_varint(sink, value);

    ser::StringSource source(sink.data());
    std::uint64_t read_value = ser::read_varint<std::uint64_t>(source);
    REQUIRE(read_value == value);
  }
}

TEST_CASE("read_varint throws on overflow for smaller types", "[serialise][varint]") {
  ser::StringSink sink;
  ser::write_varint(sink, std::uint64_t{1000000});

  ser::StringSource source(sink.data());
  REQUIRE_THROWS_AS(ser::read_varint<std::uint8_t>(source), ser::SerialisationError);
}

// ─────────────────────────────────────────────────────────────────────────────
// write_string / read_string
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("write_string and read_string round-trip", "[serialise][string]") {
  ser::StringSink sink;
  ser::write_string(sink, "hello world");

  ser::StringSource source(sink.data());
  std::string value = ser::read_string(source);
  REQUIRE(value == "hello world");
}

TEST_CASE("write_string handles empty string", "[serialise][string]") {
  ser::StringSink sink;
  ser::write_string(sink, "");

  ser::StringSource source(sink.data());
  std::string value = ser::read_string(source);
  REQUIRE(value.empty());
}

TEST_CASE("write_string pads to 8-byte boundary", "[serialise][string]") {
  // String "hi" (2 bytes) + 8 byte length prefix + 6 bytes padding = 16 bytes
  ser::StringSink sink;
  ser::write_string(sink, "hi");
  REQUIRE(sink.size() == 16);

  // String "hello" (5 bytes) + 8 byte length prefix + 3 bytes padding = 16 bytes
  ser::StringSink sink2;
  ser::write_string(sink2, "hello");
  REQUIRE(sink2.size() == 16);

  // String "12345678" (8 bytes) + 8 byte length prefix + 0 bytes padding = 16 bytes
  ser::StringSink sink3;
  ser::write_string(sink3, "12345678");
  REQUIRE(sink3.size() == 16);
}

TEST_CASE("read_string respects max_length", "[serialise][string]") {
  ser::StringSink sink;
  ser::write_string(sink, "hello world");

  ser::StringSource source(sink.data());
  REQUIRE_THROWS_AS(ser::read_string(source, 5), ser::SerialisationError);
}

TEST_CASE("read_string into buffer", "[serialise][string]") {
  ser::StringSink sink;
  ser::write_string(sink, "hello");

  ser::StringSource source(sink.data());
  char buffer[10] = {};
  std::size_t length = ser::read_string(source, buffer, 10);
  REQUIRE(length == 5);
  REQUIRE(std::strcmp(buffer, "hello") == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stream operators
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("operator<< writes integers to sink", "[serialise][operators]") {
  ser::StringSink sink;
  sink << std::uint64_t{42};

  ser::StringSource source(sink.data());
  REQUIRE(ser::read_uint64(source) == 42);
}

TEST_CASE("operator<< writes strings to sink", "[serialise][operators]") {
  ser::StringSink sink;
  sink << std::string_view{"hello"};

  ser::StringSource source(sink.data());
  REQUIRE(ser::read_string(source) == "hello");
}

TEST_CASE("operator>> reads integers from source", "[serialise][operators]") {
  ser::StringSink sink;
  ser::write_int(sink, std::uint64_t{42});

  ser::StringSource source(sink.data());
  std::uint64_t value = 0;
  source >> value;
  REQUIRE(value == 42);
}

TEST_CASE("operator>> reads strings from source", "[serialise][operators]") {
  ser::StringSink sink;
  ser::write_string(sink, "hello");

  ser::StringSource source(sink.data());
  std::string value;
  source >> value;
  REQUIRE(value == "hello");
}

TEST_CASE("chained operator<< writes multiple values", "[serialise][operators]") {
  ser::StringSink sink;
  sink << std::uint64_t{1} << std::uint64_t{2} << std::uint64_t{3};

  ser::StringSource source(sink.data());
  REQUIRE(ser::read_uint64(source) == 1);
  REQUIRE(ser::read_uint64(source) == 2);
  REQUIRE(ser::read_uint64(source) == 3);
}

// ─────────────────────────────────────────────────────────────────────────────
// drain_into
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("drain_into copies all data to sink", "[serialise][drain]") {
  std::string data = "hello world";
  ser::StringSource source(data);
  ser::StringSink sink;

  source.drain_into(sink);
  REQUIRE(sink.data() == "hello world");
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("StringSource with string_view from const char*", "[serialise][edge]") {
  const char* data = "hello";
  ser::StringSource source(std::string_view(data));

  std::array<std::byte, 5> buffer{};
  source.read_exact(buffer);
  REQUIRE(std::memcmp(buffer.data(), "hello", 5) == 0);
}

TEST_CASE("Multiple sequential reads from StringSource", "[serialise][edge]") {
  std::string data = "abcdefghij";
  ser::StringSource source(data);

  std::array<std::byte, 3> buffer{};

  source.read_exact(buffer);
  REQUIRE(std::memcmp(buffer.data(), "abc", 3) == 0);

  source.read_exact(buffer);
  REQUIRE(std::memcmp(buffer.data(), "def", 3) == 0);

  source.read_exact(buffer);
  REQUIRE(std::memcmp(buffer.data(), "ghi", 3) == 0);
}

TEST_CASE("Large string serialization", "[serialise][edge]") {
  std::string large_string(10000, 'x');

  ser::StringSink sink;
  ser::write_string(sink, large_string);

  ser::StringSource source(sink.data());
  std::string read_value = ser::read_string(source);
  REQUIRE(read_value == large_string);
}

TEST_CASE("Binary data preservation", "[serialise][edge]") {
  // Create binary data with null bytes
  std::string binary_data;
  for (int idx = 0; idx < 256; ++idx) {
    binary_data.push_back(static_cast<char>(idx));
  }

  ser::StringSink sink;
  ser::write_string(sink, binary_data);

  ser::StringSource source(sink.data());
  std::string read_value = ser::read_string(source);
  REQUIRE(read_value == binary_data);
}

TEST_CASE("Interleaved integer and string serialization", "[serialise][edge]") {
  ser::StringSink sink;
  sink << std::uint64_t{42};
  ser::write_string(sink, "hello");
  sink << std::uint64_t{100};
  ser::write_string(sink, "world");

  ser::StringSource source(sink.data());
  REQUIRE(ser::read_uint64(source) == 42);
  REQUIRE(ser::read_string(source) == "hello");
  REQUIRE(ser::read_uint64(source) == 100);
  REQUIRE(ser::read_string(source) == "world");
}

// ─────────────────────────────────────────────────────────────────────────────
// zpp_bits integration tests
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Test struct for zpp_bits serialization
struct SimpleMessage {
  std::uint32_t id;
  std::string name;
  std::vector<std::uint64_t> values;

  bool operator==(const SimpleMessage&) const = default;
};

// Test struct with nested types
struct NestedMessage {
  SimpleMessage inner;
  std::optional<std::int32_t> optional_value;

  bool operator==(const NestedMessage&) const = default;
};

// Fixed-size struct
struct FixedSizePoint {
  std::int32_t x;
  std::int32_t y;
  std::int32_t z;

  bool operator==(const FixedSizePoint&) const = default;
};

} // namespace

TEST_CASE("serialize/deserialize simple struct", "[serialise][zpp_bits]") {
  SimpleMessage original{42, "hello world", {1, 2, 3, 4, 5}};

  auto bytes = ser::serialize(original);
  REQUIRE(!bytes.empty());

  auto decoded = ser::deserialize<SimpleMessage>(bytes);
  REQUIRE(decoded == original);
}

TEST_CASE("serialize/deserialize nested struct", "[serialise][zpp_bits]") {
  NestedMessage original{{100, "nested", {10, 20, 30}}, std::optional<std::int32_t>{42}};

  auto bytes = ser::serialize(original);
  auto decoded = ser::deserialize<NestedMessage>(bytes);
  REQUIRE(decoded == original);
}

TEST_CASE("serialize/deserialize with empty optional", "[serialise][zpp_bits]") {
  NestedMessage original{{1, "test", {}}, std::nullopt};

  auto bytes = ser::serialize(original);
  auto decoded = ser::deserialize<NestedMessage>(bytes);
  REQUIRE(decoded == original);
  REQUIRE(!decoded.optional_value.has_value());
}

TEST_CASE("serialize/deserialize empty string and vector", "[serialise][zpp_bits]") {
  SimpleMessage original{0, "", {}};

  auto bytes = ser::serialize(original);
  auto decoded = ser::deserialize<SimpleMessage>(bytes);
  REQUIRE(decoded == original);
}

TEST_CASE("serialize_to_string returns valid string", "[serialise][zpp_bits]") {
  SimpleMessage original{123, "test", {7, 8, 9}};

  std::string serialized = ser::serialize_to_string(original);
  REQUIRE(!serialized.empty());

  auto decoded = ser::deserialize<SimpleMessage>(serialized);
  REQUIRE(decoded == original);
}

TEST_CASE("deserialize from string_view", "[serialise][zpp_bits]") {
  FixedSizePoint original{10, 20, 30};

  auto bytes = ser::serialize(original);
  std::string_view view(reinterpret_cast<const char*>(bytes.data()), bytes.size());

  auto decoded = ser::deserialize<FixedSizePoint>(view);
  REQUIRE(decoded == original);
}

TEST_CASE("write_object/read_object through Sink/Source", "[serialise][zpp_bits]") {
  SimpleMessage original{999, "streaming test", {100, 200, 300}};

  ser::StringSink sink;
  ser::write_object(sink, original);

  ser::StringSource source(sink.data());
  auto decoded = ser::read_object<SimpleMessage>(source);
  REQUIRE(decoded == original);
}

TEST_CASE("multiple objects through same Sink/Source", "[serialise][zpp_bits]") {
  SimpleMessage message1{1, "first", {1}};
  SimpleMessage message2{2, "second", {2, 2}};
  FixedSizePoint point{5, 10, 15};

  ser::StringSink sink;
  ser::write_object(sink, message1);
  ser::write_object(sink, message2);
  ser::write_object(sink, point);

  ser::StringSource source(sink.data());
  auto decoded1 = ser::read_object<SimpleMessage>(source);
  auto decoded2 = ser::read_object<SimpleMessage>(source);
  auto decoded_point = ser::read_object<FixedSizePoint>(source);

  REQUIRE(decoded1 == message1);
  REQUIRE(decoded2 == message2);
  REQUIRE(decoded_point == point);
}

TEST_CASE("interleave zpp_bits objects with raw integers", "[serialise][zpp_bits]") {
  SimpleMessage message{42, "interleaved", {1, 2, 3}};

  ser::StringSink sink;
  ser::write_int(sink, std::uint64_t{12345});
  ser::write_object(sink, message);
  ser::write_int(sink, std::uint64_t{67890});

  ser::StringSource source(sink.data());
  REQUIRE(ser::read_uint64(source) == 12345);
  auto decoded = ser::read_object<SimpleMessage>(source);
  REQUIRE(decoded == message);
  REQUIRE(ser::read_uint64(source) == 67890);
}

TEST_CASE("make_out/make_in with default options", "[serialise][zpp_bits]") {
  std::vector<std::byte> buffer;
  FixedSizePoint original{100, 200, 300};

  auto out = ser::make_out(buffer);
  auto result = out(original);
  REQUIRE(!zpp::bits::failure(result));

  FixedSizePoint decoded;
  auto in = ser::make_in(std::span<const std::byte>(buffer));
  result = in(decoded);
  REQUIRE(!zpp::bits::failure(result));
  REQUIRE(decoded == original);
}

TEST_CASE("Serializable concept", "[serialise][zpp_bits][concepts]") {
  static_assert(ser::Serializable<SimpleMessage>);
  static_assert(ser::Serializable<NestedMessage>);
  static_assert(ser::Serializable<FixedSizePoint>);
  static_assert(ser::Serializable<std::uint64_t>);
  static_assert(ser::Serializable<std::string>);
  static_assert(ser::Serializable<std::vector<int>>);
}

TEST_CASE("FixedSizeSerializable concept", "[serialise][zpp_bits][concepts]") {
  // FixedSizePoint has fixed size (3 * 4 = 12 bytes)
  static_assert(ser::FixedSizeSerializable<FixedSizePoint>);

  // SimpleMessage has variable size due to string and vector
  static_assert(!ser::FixedSizeSerializable<SimpleMessage>);
}

TEST_CASE("deserialize_fixed_from_source for fixed-size types", "[serialise][zpp_bits]") {
  FixedSizePoint original{-10, 0, 10};

  auto bytes = ser::serialize(original);
  std::string data(reinterpret_cast<const char*>(bytes.data()), bytes.size());

  ser::StringSource source(data);
  auto decoded = ser::deserialize_fixed_from_source<FixedSizePoint>(source);
  REQUIRE(decoded == original);
}

TEST_CASE("serialize large struct", "[serialise][zpp_bits]") {
  SimpleMessage original;
  original.id = 999999;
  original.name = std::string(10000, 'x');
  original.values.resize(1000);
  for (std::size_t idx = 0; idx < 1000; ++idx) {
    original.values[idx] = idx * 100;
  }

  auto bytes = ser::serialize(original);
  auto decoded = ser::deserialize<SimpleMessage>(bytes);
  REQUIRE(decoded == original);
}

TEST_CASE("deserialization failure throws", "[serialise][zpp_bits]") {
  std::vector<std::byte> garbage = {std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};

  REQUIRE_THROWS_AS(ser::deserialize<SimpleMessage>(garbage), ser::SerialisationError);
}

TEST_CASE("read_object with max_size limit", "[serialise][zpp_bits]") {
  SimpleMessage original{1, std::string(1000, 'a'), {}};

  ser::StringSink sink;
  ser::write_object(sink, original);

  ser::StringSource source(sink.data());
  // max_size smaller than the serialized data should throw
  REQUIRE_THROWS_AS((ser::read_object<SimpleMessage>(source, 100)), ser::SerialisationError);
}
