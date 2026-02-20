// straylight // nix // util // tests
//
// Unit and property-based tests for serialise.h/cpp

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

// RapidCheck for property-based testing
#include <array>
#include <cstring>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "nix/util/serialise.h"

using namespace nix;

// =============================================================================
// StringSink / StringSource basic tests
// =============================================================================

TEST_CASE("string_sink accepts data", "[serialise][string_sink]") {
  string_sink_t sink;
  sink("hello");
  REQUIRE(sink.str() == "hello");

  sink(" world");
  REQUIRE(sink.str() == "hello world");
}

TEST_CASE("string_sink with reserved size", "[serialise][string_sink]") {
  string_sink_t sink(1024);
  sink("test");
  REQUIRE(sink.str() == "test");
  REQUIRE(sink.str().capacity() >= 1024);
}

TEST_CASE("string_sink accepts empty data", "[serialise][string_sink]") {
  string_sink_t sink;
  sink("");
  REQUIRE(sink.str().empty());
}

TEST_CASE("string_source reads data", "[serialise][string_source]") {
  std::string data = "hello world";
  string_source_t source(data);

  std::array<char, 5> buf{};
  size_t n = source.read(buf.data(), buf.size());
  REQUIRE(n == 5);
  REQUIRE(std::string_view(buf.data(), 5) == "hello");
}

TEST_CASE("string_source throws on end of file", "[serialise][string_source]") {
  std::string data = "hi";
  string_source_t source(data);

  std::array<char, 10> buf{};
  source.read(buf.data(), 2);
  REQUIRE_THROWS_AS(source.read(buf.data(), 1), EndOfFile);
}

TEST_CASE("string_source restart", "[serialise][string_source]") {
  std::string data = "hello";
  string_source_t source(data);

  std::array<char, 5> buf{};
  source.read(buf.data(), 5);
  REQUIRE(std::string_view(buf.data(), 5) == "hello");

  source.restart();

  source.read(buf.data(), 5);
  REQUIRE(std::string_view(buf.data(), 5) == "hello");
}

TEST_CASE("string_source skip", "[serialise][string_source]") {
  std::string data = "hello world";
  string_source_t source(data);

  source.skip(6);

  std::array<char, 5> buf{};
  size_t n = source.read(buf.data(), buf.size());
  REQUIRE(n == 5);
  REQUIRE(std::string_view(buf.data(), 5) == "world");
}

TEST_CASE("string_source skip past end throws", "[serialise][string_source]") {
  std::string data = "hi";
  string_source_t source(data);

  REQUIRE_THROWS_AS(source.skip(10), EndOfFile);
}

TEST_CASE("string_source drain", "[serialise][string_source]") {
  std::string data = "hello world";
  string_source_t source(data);

  std::string result = source.drain();
  REQUIRE(result == "hello world");
}

// =============================================================================
// NullSink tests
// =============================================================================

TEST_CASE("null_sink discards data", "[serialise][null_sink]") {
  null_sink_t sink;
  sink("hello");
  sink("world");
  REQUIRE(sink.good());
}

// =============================================================================
// LengthSink tests
// =============================================================================

TEST_CASE("length_sink counts bytes", "[serialise][length_sink]") {
  length_sink_t sink;
  REQUIRE(sink.length() == 0);

  sink("hello");
  REQUIRE(sink.length() == 5);

  sink(" world");
  REQUIRE(sink.length() == 11);
}

TEST_CASE("length_sink empty data", "[serialise][length_sink]") {
  length_sink_t sink;
  sink("");
  REQUIRE(sink.length() == 0);
}

// =============================================================================
// LengthSource tests
// =============================================================================

TEST_CASE("length_source counts bytes read", "[serialise][length_source]") {
  std::string data = "hello world";
  string_source_t inner_source(data);
  length_source_t source(inner_source);

  REQUIRE(source.total() == 0);

  std::array<char, 5> buf{};
  source.read(buf.data(), 5);
  REQUIRE(source.total() == 5);

  source.read(buf.data(), 3);
  REQUIRE(source.total() == 8);
}

// =============================================================================
// TeeSink tests
// =============================================================================

TEST_CASE("tee_sink writes to both sinks", "[serialise][tee_sink]") {
  string_sink_t sink1;
  string_sink_t sink2;
  tee_sink_t tee(sink1, sink2);

  tee("hello");
  REQUIRE(sink1.str() == "hello");
  REQUIRE(sink2.str() == "hello");

  tee(" world");
  REQUIRE(sink1.str() == "hello world");
  REQUIRE(sink2.str() == "hello world");
}

// =============================================================================
// TeeSource tests
// =============================================================================

TEST_CASE("tee_source saves data to sink", "[serialise][tee_source]") {
  std::string data = "hello world";
  string_source_t inner_source(data);
  string_sink_t sink;
  tee_source_t tee(inner_source, sink);

  std::array<char, 5> buf{};
  tee.read(buf.data(), 5);
  REQUIRE(std::string_view(buf.data(), 5) == "hello");
  REQUIRE(sink.str() == "hello");

  tee.read(buf.data(), 1);
  REQUIRE(sink.str() == "hello ");
}

// =============================================================================
// SizedSource tests
// =============================================================================

TEST_CASE("sized_source limits reading", "[serialise][sized_source]") {
  std::string data = "hello world";
  string_source_t inner_source(data);
  sized_source_t sized(inner_source, 5);

  std::array<char, 10> buf{};
  size_t n = sized.read(buf.data(), buf.size());
  REQUIRE(n == 5);
  REQUIRE(std::string_view(buf.data(), 5) == "hello");
}

TEST_CASE("sized_source throws when exhausted", "[serialise][sized_source]") {
  std::string data = "hello";
  string_source_t inner_source(data);
  sized_source_t sized(inner_source, 5);

  std::array<char, 10> buf{};
  sized.read(buf.data(), 5);
  REQUIRE_THROWS_AS(sized.read(buf.data(), 1), EndOfFile);
}

TEST_CASE("sized_source drain_all", "[serialise][sized_source]") {
  std::string data = "hello world";
  string_source_t inner_source(data);
  sized_source_t sized(inner_source, 5);

  size_t drained = sized.drain_all();
  REQUIRE(drained == 5);
}

// =============================================================================
// ChainSource tests
// =============================================================================

TEST_CASE("chain_source reads from both sources", "[serialise][chain_source]") {
  std::string data1 = "hello";
  std::string data2 = " world";
  string_source_t source1(data1);
  string_source_t source2(data2);
  chain_source_t chain(source1, source2);

  std::string result = chain.drain();
  REQUIRE(result == "hello world");
}

TEST_CASE("chain_source switches on eof", "[serialise][chain_source]") {
  std::string data1 = "ab";
  std::string data2 = "cd";
  string_source_t source1(data1);
  string_source_t source2(data2);
  chain_source_t chain(source1, source2);

  std::array<char, 1> buf{};
  chain.read(buf.data(), 1);
  REQUIRE(buf[0] == 'a');
  chain.read(buf.data(), 1);
  REQUIRE(buf[0] == 'b');
  chain.read(buf.data(), 1);
  REQUIRE(buf[0] == 'c');
  chain.read(buf.data(), 1);
  REQUIRE(buf[0] == 'd');
}

// =============================================================================
// LambdaSink tests
// =============================================================================

TEST_CASE("lambda_sink calls function", "[serialise][lambda_sink]") {
  std::string captured;
  lambda_sink_t sink([&](std::string_view data) { captured.append(data); });

  sink("hello");
  REQUIRE(captured == "hello");

  sink(" world");
  REQUIRE(captured == "hello world");
}

TEST_CASE("lambda_sink cleanup called on destruction", "[serialise][lambda_sink]") {
  bool cleanup_called = false;
  {
    lambda_sink_t sink([](std::string_view) {}, [&]() { cleanup_called = true; });
    sink("data");
  }
  REQUIRE(cleanup_called);
}

// =============================================================================
// LambdaSource tests
// =============================================================================

TEST_CASE("lambda_source calls function", "[serialise][lambda_source]") {
  std::string data = "hello world";
  size_t pos = 0;
  lambda_source_t source([&](char* buf, size_t len) -> size_t {
    size_t n = std::min(len, data.size() - pos);
    if (n == 0) {
      throw EndOfFile("end");
    }
    // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
    std::memcpy(buf, data.data() + pos, n);
    pos += n;
    return n;
  });

  std::string result = source.drain();
  REQUIRE(result == "hello world");
}

// =============================================================================
// Integer serialisation tests
// =============================================================================

TEST_CASE("write uint64_t to sink", "[serialise][integer]") {
  string_sink_t sink;
  sink << static_cast<uint64_t>(0x0102030405060708ULL);

  REQUIRE(sink.str().size() == 8);
  // Little-endian encoding
  REQUIRE(static_cast<unsigned char>(sink.str()[0]) == 0x08);
  REQUIRE(static_cast<unsigned char>(sink.str()[1]) == 0x07);
  REQUIRE(static_cast<unsigned char>(sink.str()[2]) == 0x06);
  REQUIRE(static_cast<unsigned char>(sink.str()[3]) == 0x05);
  REQUIRE(static_cast<unsigned char>(sink.str()[4]) == 0x04);
  REQUIRE(static_cast<unsigned char>(sink.str()[5]) == 0x03);
  REQUIRE(static_cast<unsigned char>(sink.str()[6]) == 0x02);
  REQUIRE(static_cast<unsigned char>(sink.str()[7]) == 0x01);
}

TEST_CASE("write uint64_t zero", "[serialise][integer]") {
  string_sink_t sink;
  sink << static_cast<uint64_t>(0);

  REQUIRE(sink.str().size() == 8);
  for (int i = 0; i < 8; i++) {
    REQUIRE(sink.str()[i] == '\0');
  }
}

TEST_CASE("write uint64_t max", "[serialise][integer]") {
  string_sink_t sink;
  sink << std::numeric_limits<uint64_t>::max();

  REQUIRE(sink.str().size() == 8);
  for (int i = 0; i < 8; i++) {
    REQUIRE(static_cast<unsigned char>(sink.str()[i]) == 0xff);
  }
}

TEST_CASE("read uint64_t from source", "[serialise][integer]") {
  std::string data;
  data.resize(8);
  data[0] = 0x08;
  data[1] = 0x07;
  data[2] = 0x06;
  data[3] = 0x05;
  data[4] = 0x04;
  data[5] = 0x03;
  data[6] = 0x02;
  data[7] = 0x01;
  string_source_t source(data);

  uint64_t n = read_long_long(source);
  REQUIRE(n == 0x0102030405060708ULL);
}

TEST_CASE("read unsigned int from source", "[serialise][integer]") {
  std::string data;
  data.resize(8);
  data[0] = 0x78;
  data[1] = 0x56;
  data[2] = 0x34;
  data[3] = 0x12;
  data[4] = 0x00;
  data[5] = 0x00;
  data[6] = 0x00;
  data[7] = 0x00;
  string_source_t source(data);

  unsigned int n = read_int(source);
  REQUIRE(n == 0x12345678);
}

TEST_CASE("readNum overflow throws", "[serialise][integer]") {
  std::string data;
  data.resize(8);
  // value_t larger than uint32_t max
  data[0] = 0x00;
  data[1] = 0x00;
  data[2] = 0x00;
  data[3] = 0x00;
  data[4] = 0x01; // bit 32 set
  data[5] = 0x00;
  data[6] = 0x00;
  data[7] = 0x00;
  string_source_t source(data);

  REQUIRE_THROWS_AS(read_num<uint32_t>(source), SerialisationError);
}

// =============================================================================
// String serialisation tests
// =============================================================================

TEST_CASE("write string to sink", "[serialise][string]") {
  string_sink_t sink;
  sink << std::string_view("hello");

  // 8 bytes length + 5 bytes data + 3 bytes padding = 16 bytes
  REQUIRE(sink.str().size() == 16);
}

TEST_CASE("read string from source", "[serialise][string]") {
  string_sink_t sink;
  sink << std::string_view("hello");

  string_source_t source(sink.str());
  std::string result = read_string(source);
  REQUIRE(result == "hello");
}

TEST_CASE("write empty string", "[serialise][string]") {
  string_sink_t sink;
  sink << std::string_view("");

  // 8 bytes length + 0 bytes data + 0 bytes padding = 8 bytes
  REQUIRE(sink.str().size() == 8);
}

TEST_CASE("read empty string", "[serialise][string]") {
  string_sink_t sink;
  sink << std::string_view("");

  string_source_t source(sink.str());
  std::string result = read_string(source);
  REQUIRE(result.empty());
}

TEST_CASE("read string with max limit", "[serialise][string]") {
  string_sink_t sink;
  sink << std::string_view("hello");

  string_source_t source(sink.str());
  REQUIRE_THROWS_AS(read_string(source, 3), SerialisationError);
}

TEST_CASE("read string into buffer", "[serialise][string]") {
  string_sink_t sink;
  sink << std::string_view("hello");

  string_source_t source(sink.str());
  std::array<char, 10> buf{};
  size_t len = read_string(buf.data(), buf.size(), source);
  REQUIRE(len == 5);
  REQUIRE(std::string_view(buf.data(), len) == "hello");
}

TEST_CASE("read string into buffer too small throws", "[serialise][string]") {
  string_sink_t sink;
  sink << std::string_view("hello");

  string_source_t source(sink.str());
  std::array<char, 3> buf{};
  REQUIRE_THROWS_AS(read_string(buf.data(), buf.size(), source), SerialisationError);
}

// =============================================================================
// Padding tests
// =============================================================================

TEST_CASE("write padding for various lengths", "[serialise][padding]") {
  // Length % 8 == 0: no padding
  {
    string_sink_t sink;
    write_padding(0, sink);
    REQUIRE(sink.str().empty());
  }
  {
    string_sink_t sink;
    write_padding(8, sink);
    REQUIRE(sink.str().empty());
  }
  {
    string_sink_t sink;
    write_padding(16, sink);
    REQUIRE(sink.str().empty());
  }

  // Length % 8 == 1: 7 bytes padding
  {
    string_sink_t sink;
    write_padding(1, sink);
    REQUIRE(sink.str().size() == 7);
    for (char c : sink.str()) {
      REQUIRE(c == '\0');
    }
  }

  // Length % 8 == 5: 3 bytes padding
  {
    string_sink_t sink;
    write_padding(5, sink);
    REQUIRE(sink.str().size() == 3);
    for (char c : sink.str()) {
      REQUIRE(c == '\0');
    }
  }

  // Length % 8 == 7: 1 byte padding
  {
    string_sink_t sink;
    write_padding(7, sink);
    REQUIRE(sink.str().size() == 1);
    REQUIRE(sink.str()[0] == '\0');
  }
}

TEST_CASE("read padding validates zeros", "[serialise][padding]") {
  // Valid padding (all zeros)
  {
    std::string data(3, '\0');
    string_source_t source(data);
    REQUIRE_NOTHROW(read_padding(5, source));
  }

  // Invalid padding (non-zero)
  {
    std::string data;
    data.push_back('\x00');
    data.push_back('\x01');
    data.push_back('\x00');
    string_source_t source(data);
    REQUIRE_THROWS_AS(read_padding(5, source), SerialisationError);
  }
}

// =============================================================================
// Strings/StringSet serialisation tests
// =============================================================================

TEST_CASE("write strings to sink", "[serialise][strings]") {
  string_sink_t sink;
  strings_t strings = {"hello", "world"};
  sink << strings;

  string_source_t source(sink.str());
  auto result = read_strings<strings_t>(source);
  REQUIRE(result.size() == 2);
  auto it = result.begin();
  REQUIRE(*it++ == "hello");
  REQUIRE(*it++ == "world");
}

TEST_CASE("write empty strings list", "[serialise][strings]") {
  string_sink_t sink;
  strings_t empty;
  sink << empty;

  string_source_t source(sink.str());
  auto result = read_strings<strings_t>(source);
  REQUIRE(result.empty());
}

TEST_CASE("write string set to sink", "[serialise][strings]") {
  string_sink_t sink;
  string_set_t strings = {"alpha", "beta", "gamma"};
  sink << strings;

  string_source_t source(sink.str());
  // StringSet is sorted
  auto result = read_strings<string_set_t>(source);
  REQUIRE(result.size() == 3);
  REQUIRE(result.count("alpha") == 1);
  REQUIRE(result.count("beta") == 1);
  REQUIRE(result.count("gamma") == 1);
}

// =============================================================================
// source_t operator() tests
// =============================================================================

TEST_CASE("source operator reads exact bytes", "[serialise][source]") {
  std::string data = "hello world";
  string_source_t source(data);

  std::array<char, 11> buf{};
  source(buf.data(), 11);
  REQUIRE(std::string_view(buf.data(), 11) == "hello world");
}

TEST_CASE("source operator throws on insufficient data", "[serialise][source]") {
  std::string data = "hi";
  string_source_t source(data);

  std::array<char, 10> buf{};
  REQUIRE_THROWS_AS(source(buf.data(), 10), EndOfFile);
}

// =============================================================================
// source_t drain into sink tests
// =============================================================================

TEST_CASE("source drain_into sink", "[serialise][source]") {
  std::string data = "hello world";
  string_source_t source(data);
  string_sink_t sink;

  source.drain_into(sink);
  REQUIRE(sink.str() == "hello world");
}

// =============================================================================
// Property-based tests
// =============================================================================

TEST_CASE("uint64_t roundtrip property", "[serialise][property][integer]") {
  rc::prop("write then read uint64_t gives same value", []() {
    uint64_t value = *rc::gen::arbitrary<uint64_t>();

    string_sink_t sink;
    sink << value;

    string_source_t source(sink.str());
    uint64_t result = read_long_long(source);

    RC_ASSERT(result == value);
  });
}

TEST_CASE("unsigned int roundtrip property", "[serialise][property][integer]") {
  rc::prop("write then read unsigned int gives same value", []() {
    unsigned int value = *rc::gen::arbitrary<unsigned int>();

    string_sink_t sink;
    sink << static_cast<uint64_t>(value);

    string_source_t source(sink.str());
    unsigned int result = read_int(source);

    RC_ASSERT(result == value);
  });
}

TEST_CASE("string roundtrip property", "[serialise][property][string]") {
  rc::prop("write then read string gives same value", []() {
    auto str = *rc::gen::arbitrary<std::string>();

    string_sink_t sink;
    sink << str;

    string_source_t source(sink.str());
    std::string result = read_string(source);

    RC_ASSERT(result == str);
  });
}

TEST_CASE("string with nulls roundtrip", "[serialise][property][string]") {
  rc::prop("strings with embedded nulls survive roundtrip", []() {
    auto bytes = *rc::gen::container<std::vector<char>>(rc::gen::arbitrary<char>());
    std::string str(bytes.begin(), bytes.end());

    string_sink_t sink;
    sink << str;

    string_source_t source(sink.str());
    std::string result = read_string(source);

    RC_ASSERT(result == str);
  });
}

TEST_CASE("strings list roundtrip property", "[serialise][property][strings]") {
  rc::prop("write then read Strings gives same value", []() {
    auto strings = *rc::gen::container<std::vector<std::string>>(rc::gen::arbitrary<std::string>());
    strings_t input(strings.begin(), strings.end());

    string_sink_t sink;
    sink << input;

    string_source_t source(sink.str());
    auto result = read_strings<strings_t>(source);

    RC_ASSERT(result == input);
  });
}

TEST_CASE("string_source restart invariant", "[serialise][property][source]") {
  rc::prop("restart resets position to beginning", []() {
    auto str = *rc::gen::nonEmpty<std::string>();

    string_source_t source(str);

    // Read some data
    std::array<char, 1> buf{};
    source.read(buf.data(), 1);

    // Restart
    source.restart();

    // Should read from beginning
    std::string result = source.drain();
    RC_ASSERT(result == str);
  });
}

TEST_CASE("length_sink counts all bytes", "[serialise][property][length_sink]") {
  rc::prop("length_sink.length equals sum of all chunk sizes", []() {
    auto chunks = *rc::gen::container<std::vector<std::string>>(rc::gen::arbitrary<std::string>());

    length_sink_t sink;
    size_t expected = 0;
    for (const auto& chunk : chunks) {
      sink(chunk);
      expected += chunk.size();
    }

    RC_ASSERT(sink.length() == expected);
  });
}

TEST_CASE("tee_sink writes identical data to both", "[serialise][property][tee_sink]") {
  rc::prop("tee_sink writes same data to both underlying sinks", []() {
    auto chunks = *rc::gen::container<std::vector<std::string>>(rc::gen::arbitrary<std::string>());

    string_sink_t sink1;
    string_sink_t sink2;
    tee_sink_t tee(sink1, sink2);

    for (const auto& chunk : chunks) {
      tee(chunk);
    }

    RC_ASSERT(sink1.str() == sink2.str());
  });
}

TEST_CASE("chain_source concatenates sources", "[serialise][property][chain_source]") {
  rc::prop("chain_source drain equals concatenation", []() {
    auto str1 = *rc::gen::arbitrary<std::string>();
    auto str2 = *rc::gen::arbitrary<std::string>();

    string_source_t source1(str1);
    string_source_t source2(str2);
    chain_source_t chain(source1, source2);

    std::string result = chain.drain();

    RC_ASSERT(result == str1 + str2);
  });
}

TEST_CASE("sized_source limits data", "[serialise][property][sized_source]") {
  rc::prop("sized_source drains at most size bytes", []() {
    auto str = *rc::gen::nonEmpty<std::string>();
    size_t limit = *rc::gen::inRange<size_t>(1, str.size() + 1);

    string_source_t inner(str);
    sized_source_t sized(inner, limit);

    size_t drained = sized.drain_all();

    RC_ASSERT(drained == std::min(limit, str.size()));
  });
}

TEST_CASE("string padding is always 8-byte aligned", "[serialise][property][padding]") {
  rc::prop("serialised string size is multiple of 8", []() {
    auto str = *rc::gen::arbitrary<std::string>();

    string_sink_t sink;
    sink << str;

    RC_ASSERT(sink.str().size() % 8 == 0);
  });
}

TEST_CASE("multiple values roundtrip", "[serialise][property]") {
  rc::prop("multiple values can be written and read in sequence", []() {
    auto val1 = *rc::gen::arbitrary<uint64_t>();
    auto str1 = *rc::gen::arbitrary<std::string>();
    auto val2 = *rc::gen::arbitrary<uint64_t>();
    auto str2 = *rc::gen::arbitrary<std::string>();

    string_sink_t sink;
    sink << val1 << str1 << val2 << str2;

    string_source_t source(sink.str());
    uint64_t r_val1 = read_long_long(source);
    std::string r_str1 = read_string(source);
    uint64_t r_val2 = read_long_long(source);
    std::string r_str2 = read_string(source);

    RC_ASSERT(r_val1 == val1);
    RC_ASSERT(r_str1 == str1);
    RC_ASSERT(r_val2 == val2);
    RC_ASSERT(r_str2 == str2);
  });
}

// =============================================================================
// Fuzz-style edge case tests
// =============================================================================

TEST_CASE("malformed padding detection", "[serialise][fuzz][padding]") {
  // Generate all possible non-zero padding patterns for length 5 (3 bytes padding)
  for (int byte0 = 0; byte0 <= 1; byte0++) {
    for (int byte1 = 0; byte1 <= 1; byte1++) {
      for (int byte2 = 0; byte2 <= 1; byte2++) {
        std::string padding;
        padding += static_cast<char>(byte0);
        padding += static_cast<char>(byte1);
        padding += static_cast<char>(byte2);

        string_source_t source(padding);

        if (byte0 == 0 && byte1 == 0 && byte2 == 0) {
          REQUIRE_NOTHROW(read_padding(5, source));
        } else {
          REQUIRE_THROWS_AS(read_padding(5, source), SerialisationError);
        }
      }
    }
  }
}

TEST_CASE("truncated integer read fails", "[serialise][fuzz][integer]") {
  for (size_t len = 0; len < 8; len++) {
    std::string data(len, '\x42');
    string_source_t source(data);

    REQUIRE_THROWS_AS(read_long_long(source), EndOfFile);
  }
}

TEST_CASE("truncated string length fails", "[serialise][fuzz][string]") {
  // Less than 8 bytes for the length field
  for (size_t len = 0; len < 8; len++) {
    std::string data(len, '\x00');
    string_source_t source(data);

    REQUIRE_THROWS_AS(read_string(source), EndOfFile);
  }
}

TEST_CASE("truncated string data fails", "[serialise][fuzz][string]") {
  // Valid length field saying 10 bytes, but only 5 bytes of data
  std::string data;
  data.resize(8 + 5);
  // Write length 10 in little-endian
  data[0] = 10;
  data[1] = 0;
  data[2] = 0;
  data[3] = 0;
  data[4] = 0;
  data[5] = 0;
  data[6] = 0;
  data[7] = 0;
  // Only 5 bytes of data follow

  string_source_t source(data);
  REQUIRE_THROWS_AS(read_string(source), EndOfFile);
}

TEST_CASE("string length overflow handling", "[serialise][fuzz][string]") {
  // Very large length that would overflow
  std::string data;
  data.resize(8);
  // Write size_t max
  for (int i = 0; i < 8; i++) {
    data[i] = static_cast<char>(0xff);
  }

  string_source_t source(data);
  // Should either throw or try to allocate huge amount
  REQUIRE_THROWS(read_string(source));
}

TEST_CASE("extreme string lengths property", "[serialise][property][string]") {
  rc::prop("strings of various lengths roundtrip correctly", []() {
    // Generate strings of specific boundary lengths
    size_t len = *rc::gen::oneOf(
        rc::gen::just<size_t>(0), rc::gen::just<size_t>(1), rc::gen::just<size_t>(7),
        rc::gen::just<size_t>(8), rc::gen::just<size_t>(9), rc::gen::just<size_t>(15),
        rc::gen::just<size_t>(16), rc::gen::just<size_t>(17), rc::gen::inRange<size_t>(0, 1024));

    std::string str(len, 'x');

    string_sink_t sink;
    sink << str;

    string_source_t source(sink.str());
    std::string result = read_string(source);

    RC_ASSERT(result == str);
    RC_ASSERT(sink.str().size() == 8 + len + (len % 8 == 0 ? 0 : 8 - (len % 8)));
  });
}

TEST_CASE("skip beyond end detection", "[serialise][fuzz][source]") {
  std::string data = "hello";
  string_source_t source(data);

  // Skip exactly the data length works
  REQUIRE_NOTHROW(source.skip(5));

  // But then skip more fails
  REQUIRE_THROWS_AS(source.skip(1), EndOfFile);
}

TEST_CASE("source operator with zero length", "[serialise][source]") {
  std::string data = "hello";
  string_source_t source(data);

  std::array<char, 1> buf{};
  // Zero-length read should work
  source(buf.data(), 0);

  // Data should still be available
  std::string result = source.drain();
  REQUIRE(result == "hello");
}

// =============================================================================
// BufferedSink tests (via a mock)
// =============================================================================

namespace {
// NOLINTNEXTLINE(readability-identifier-naming)
struct TestBufferedSink : buffered_sink_t {
  std::string output;
  size_t write_count = 0;

  explicit TestBufferedSink(size_t buf_size = 32) : buffered_sink_t(buf_size) {}

  void write_unbuffered(std::string_view data) override {
    output.append(data);
    write_count++;
  }
};
} // namespace

TEST_CASE("buffered_sink buffers small writes", "[serialise][buffered_sink]") {
  TestBufferedSink sink(32);

  sink("hello");
  REQUIRE(sink.output.empty()); // Still in buffer
  REQUIRE(sink.write_count == 0);

  sink.flush();
  REQUIRE(sink.output == "hello");
  REQUIRE(sink.write_count == 1);
}

TEST_CASE("buffered_sink flushes when buffer full", "[serialise][buffered_sink]") {
  TestBufferedSink sink(10);

  sink("12345"); // 5 bytes, fits
  sink("67890"); // 5 more bytes, still fits (10 total)
  sink("!");     // Triggers flush + may stay in buffer

  sink.flush(); // Force remaining to be written

  // All data should now be written
  REQUIRE(sink.output.contains("1234567890"));
  REQUIRE(sink.output.contains('!'));
}

TEST_CASE("buffered_sink bypasses buffer for large writes", "[serialise][buffered_sink]") {
  TestBufferedSink sink(10);

  std::string large_data(100, 'x');
  sink(large_data);

  // Should have been written directly (bypassing buffer)
  // First flush any existing buffer, then write unbuffered
  REQUIRE(sink.output == large_data);
}

TEST_CASE("buffered_sink multiple flushes", "[serialise][buffered_sink]") {
  TestBufferedSink sink(10);

  sink("abc");
  sink.flush();
  REQUIRE(sink.output == "abc");

  sink("def");
  sink.flush();
  REQUIRE(sink.output == "abcdef");

  // Double flush should be no-op
  sink.flush();
  REQUIRE(sink.output == "abcdef");
}

// =============================================================================
// BufferedSource tests (via a mock)
// =============================================================================

namespace {
// NOLINTNEXTLINE(readability-identifier-naming)
struct TestBufferedSource : buffered_source_t {
  std::string data;
  size_t pos = 0;
  size_t read_count = 0;

  explicit TestBufferedSource(std::string_view input, size_t buf_size = 32)
      : buffered_source_t(buf_size), data(input) {}

  size_t read_unbuffered(char* buf, size_t len) override {
    if (pos >= data.size()) {
      throw EndOfFile("end of test data");
    }
    size_t n = std::min(len, data.size() - pos);
    std::memcpy(buf, data.data() + pos, n);
    pos += n;
    read_count++;
    return n;
  }
};
} // namespace

TEST_CASE("buffered_source buffers reads", "[serialise][buffered_source]") {
  TestBufferedSource source("hello world", 32);

  std::array<char, 5> buf{};
  source.read(buf.data(), 5); // Triggers one underlying read of 32 bytes (or less)
  source.read(buf.data(), 5); // Should be served from buffer

  REQUIRE(source.read_count == 1); // Only one underlying read
}

TEST_CASE("buffered_source has_data", "[serialise][buffered_source]") {
  TestBufferedSource source("hello", 32);

  REQUIRE_FALSE(source.has_data()); // Buffer empty initially

  std::array<char, 3> buf{};
  source.read(buf.data(), 3);

  REQUIRE(source.has_data()); // "lo" still in buffer
}

// =============================================================================
// FramedSource/FramedSink protocol tests
// =============================================================================

namespace {
// NOLINTNEXTLINE(readability-identifier-naming)
struct MockBufferedSink : buffered_sink_t {
  std::string output;

  void write_unbuffered(std::string_view data) override { output.append(data); }
};
} // namespace

TEST_CASE("framed_sink_source roundtrip", "[serialise][framed]") {
  // Write using FramedSink
  MockBufferedSink underlying_sink;
  {
    framed_sink_t framed(underlying_sink, []() {});
    framed("hello");
    framed(" world");
    framed.flush();
  } // Destructor writes terminator

  underlying_sink.flush();

  // Read using FramedSource
  string_source_t underlying_source(underlying_sink.output);
  framed_source_t framed(underlying_source);

  std::string result = framed.drain();
  REQUIRE(result == "hello world");
}

TEST_CASE("framed_source handles empty chunks", "[serialise][framed]") {
  // Create framed data with terminator only
  string_sink_t sink;
  sink << static_cast<uint64_t>(0); // Terminator

  string_source_t source(sink.str());
  framed_source_t framed(source);

  // drain() on empty framed source returns empty string
  std::string result = framed.drain();
  REQUIRE(result.empty());
}

// =============================================================================
// Property tests for framing protocol
// =============================================================================

TEST_CASE("framed roundtrip property", "[serialise][property][framed]") {
  rc::prop("framed sink/source roundtrip preserves data", []() {
    auto chunks = *rc::gen::container<std::vector<std::string>>(rc::gen::arbitrary<std::string>());

    // Write
    MockBufferedSink underlying_sink;
    {
      framed_sink_t framed(underlying_sink, []() {});
      for (const auto& chunk : chunks) {
        framed(chunk);
      }
      framed.flush();
    }
    underlying_sink.flush();

    // Read
    string_source_t underlying_source(underlying_sink.output);
    framed_source_t framed(underlying_source);

    std::string result = framed.drain();

    // Expected result
    std::string expected;
    for (const auto& chunk : chunks) {
      expected += chunk;
    }

    RC_ASSERT(result == expected);
  });
}

// =============================================================================
// Binary data fuzz tests
// =============================================================================

TEST_CASE("binary data roundtrip", "[serialise][property][binary]") {
  rc::prop("arbitrary binary data survives serialisation", []() {
    auto bytes = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    std::string binary_data(bytes.begin(), bytes.end());

    string_sink_t sink;
    sink << binary_data;

    string_source_t source(sink.str());
    std::string result = read_string(source);

    RC_ASSERT(result == binary_data);
  });
}

TEST_CASE("high entropy data roundtrip", "[serialise][property][binary]") {
  rc::prop("random high-entropy data survives", []() {
    // Generate data with all byte values
    size_t len = *rc::gen::inRange<size_t>(0, 512);
    std::string data;
    data.reserve(len);

    std::mt19937 rng(*rc::gen::arbitrary<uint32_t>());
    std::uniform_int_distribution<int> dist(0, 255);

    for (size_t i = 0; i < len; i++) {
      data += static_cast<char>(dist(rng));
    }

    string_sink_t sink;
    sink << data;

    string_source_t source(sink.str());
    std::string result = read_string(source);

    RC_ASSERT(result == data);
  });
}

// =============================================================================
// Edge case: very long strings
// =============================================================================

TEST_CASE("large string roundtrip", "[serialise][string]") {
  // 1MB string
  std::string large_string(static_cast<size_t>(1024) * 1024, 'A');

  string_sink_t sink;
  sink << large_string;

  string_source_t source(sink.str());
  std::string result = read_string(source);

  REQUIRE(result == large_string);
}

TEST_CASE("many small strings roundtrip", "[serialise][strings]") {
  strings_t many_strings;
  for (int i = 0; i < 1000; i++) {
    many_strings.push_back("string" + std::to_string(i));
  }

  string_sink_t sink;
  sink << many_strings;

  string_source_t source(sink.str());
  auto result = read_strings<strings_t>(source);

  REQUIRE(result == many_strings);
}

// =============================================================================
// Concurrent/interleaved read tests
// =============================================================================

TEST_CASE("interleaved reads from same source", "[serialise][source]") {
  // Serialise multiple values
  string_sink_t sink;
  sink << static_cast<uint64_t>(42);
  sink << std::string_view("hello");
  sink << static_cast<uint64_t>(100);
  sink << std::string_view("world");

  string_source_t source(sink.str());

  // Read them back in order
  REQUIRE(read_long_long(source) == 42);
  REQUIRE(read_string(source) == "hello");
  REQUIRE(read_long_long(source) == 100);
  REQUIRE(read_string(source) == "world");
}

// =============================================================================
// Stream adapter test
// =============================================================================

TEST_CASE("stream_to_source_adapter", "[serialise][stream]") {
  auto ss = std::make_shared<std::istringstream>("hello world");
  stream_to_source_adapter_t adapter(ss);

  std::array<char, 5> buf{};
  size_t n = adapter.read(buf.data(), buf.size());
  REQUIRE(n == 5);
  REQUIRE(std::string_view(buf.data(), 5) == "hello");
}

TEST_CASE("stream_to_source_adapter eof", "[serialise][stream]") {
  auto ss = std::make_shared<std::istringstream>("hi");
  stream_to_source_adapter_t adapter(ss);

  std::array<char, 10> buf{};
  adapter.read(buf.data(), 2);
  REQUIRE_THROWS_AS(adapter.read(buf.data(), 1), EndOfFile);
}

// =============================================================================
// source_t >> operator tests
// =============================================================================

TEST_CASE("source >> string operator", "[serialise][operator]") {
  string_sink_t sink;
  sink << std::string_view("test");

  string_source_t source(sink.str());
  std::string result;
  source >> result;

  REQUIRE(result == "test");
}

TEST_CASE("source >> uint64_t operator", "[serialise][operator]") {
  string_sink_t sink;
  sink << static_cast<uint64_t>(12345);

  string_source_t source(sink.str());
  uint64_t result = 0;
  source >> result;

  REQUIRE(result == 12345);
}
