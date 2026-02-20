// straylight // nix // tests // fuzz
//
// NAR Archive Parsing Fuzz Tests
//
// Fuzz the NAR parser with malformed and random input.

#include <string>

#include "nix/tests/property.h"
#include "nix/util/archive.h"
#include "nix/util/error.h"
#include "nix/util/serialise.h"

// =============================================================================
// Null sink for fuzzing (discards all output)
// =============================================================================

namespace {

struct null_fso_sink_t : nix::file_system_object_sink_t {
  void create_directory(const nix::canon_path_t&) override {}
  void create_regular_file(const nix::canon_path_t&,
                           ::std::function<void(nix::create_regular_file_sink_t&)>) override {}
  void create_symlink(const nix::canon_path_t&, const ::std::string&) override {}
};

} // namespace

// =============================================================================
// Fuzz parse_dump with random NAR-like data
// =============================================================================

TEST_CASE("fuzz: parse_dump handles arbitrary input", "[fuzz][nar]") {
  rc::prop("parse_dump never crashes on arbitrary input", []() {
    auto input = *rc::gen::arbitrary<::std::string>();

    null_fso_sink_t sink;
    nix::string_source_t source(input);

    try {
      nix::parse_dump(sink, source);
    } catch (const nix::base_error_t&) {
      // Expected for malformed NAR
    } catch (const ::std::exception&) {
      // Other exceptions acceptable
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Structured NAR fuzzing: valid-ish NAR headers with random content
// =============================================================================

TEST_CASE("fuzz: parse_dump with NAR-like structure", "[fuzz][nar]") {
  rc::prop("structured NAR fuzzing", []() {
    // NAR format starts with "nix-archive-1" magic
    auto nar = ::std::string{};

    // Decide whether to include proper magic or not
    auto use_magic = *rc::gen::arbitrary<bool>();
    if (use_magic) {
      // NAR uses length-prefixed strings, padded to 8 bytes
      // "nix-archive-1" is 13 chars
      auto magic_len = ::std::uint64_t{13};
      nar.append(reinterpret_cast<const char*>(&magic_len), 8);
      nar.append("nix-archive-1");
      nar.append(3, '\0'); // Padding to 16 bytes
    }

    // Add random garbage after
    auto garbage = *rc::gen::arbitrary<::std::string>();
    nar += garbage;

    null_fso_sink_t sink;
    nix::string_source_t source(nar);

    try {
      nix::parse_dump(sink, source);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz with NAR structure elements
// =============================================================================

TEST_CASE("fuzz: parse_dump with NAR elements", "[fuzz][nar]") {
  rc::prop("NAR element fuzzing", []() {
    auto nar = ::std::string{};

    // Helper to write length-prefixed string
    auto write_str = [&nar](const ::std::string& s) {
      auto len = static_cast<::std::uint64_t>(s.size());
      nar.append(reinterpret_cast<const char*>(&len), 8);
      nar.append(s);
      // Pad to 8-byte boundary
      auto pad = (8 - (len % 8)) % 8;
      nar.append(pad, '\0');
    };

    // Write magic
    write_str("nix-archive-1");

    // Random NAR tokens
    auto tokens = *rc::gen::container<::std::vector<::std::string>>(rc::gen::element<::std::string>(
        "(", ")", "type", "regular", "directory", "symlink", "contents", "executable", "entry",
        "name", "node", "target", "", "x", "invalid"));

    for (const auto& tok : tokens) {
      write_str(tok);
    }

    // Add some random content
    auto content = *rc::gen::arbitrary<::std::string>();
    if (!content.empty()) {
      write_str(content);
    }

    null_fso_sink_t sink;
    nix::string_source_t source(nar);

    try {
      nix::parse_dump(sink, source);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz with truncated NAR
// =============================================================================

TEST_CASE("fuzz: parse_dump handles truncated NAR", "[fuzz][nar]") {
  rc::prop("truncated NAR doesn't crash", []() {
    // Build a minimal valid NAR header
    auto nar = ::std::string{};

    auto write_str = [&nar](const ::std::string& s) {
      auto len = static_cast<::std::uint64_t>(s.size());
      nar.append(reinterpret_cast<const char*>(&len), 8);
      nar.append(s);
      auto pad = (8 - (len % 8)) % 8;
      nar.append(pad, '\0');
    };

    write_str("nix-archive-1");
    write_str("(");
    write_str("type");
    write_str("regular");
    write_str("contents");

    // Add a large content length but truncated data
    auto fake_len = *rc::gen::inRange<::std::uint64_t>(0, 1000000);
    nar.append(reinterpret_cast<const char*>(&fake_len), 8);

    // Only add some of the content
    auto actual_content = *rc::gen::container<::std::string>(*rc::gen::inRange<::size_t>(0, 100),
                                                             rc::gen::arbitrary<char>());
    nar.append(actual_content);

    null_fso_sink_t sink;
    nix::string_source_t source(nar);

    try {
      nix::parse_dump(sink, source);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz with malformed length fields
// =============================================================================

TEST_CASE("fuzz: parse_dump handles malformed lengths", "[fuzz][nar]") {
  rc::prop("malformed lengths don't crash", []() {
    auto nar = ::std::string{};

    // Write some random 8-byte values as "lengths"
    auto num_fields = *rc::gen::inRange(1, 20);
    for (auto idx = 0; idx < num_fields; ++idx) {
      auto len = *rc::gen::arbitrary<::std::uint64_t>();
      nar.append(reinterpret_cast<const char*>(&len), 8);

      // Maybe add some actual data
      auto add_data = *rc::gen::arbitrary<bool>();
      if (add_data) {
        auto data = *rc::gen::container<::std::string>(*rc::gen::inRange<::size_t>(0, 50),
                                                       rc::gen::arbitrary<char>());
        nar.append(data);
      }
    }

    null_fso_sink_t sink;
    nix::string_source_t source(nar);

    try {
      nix::parse_dump(sink, source);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}
