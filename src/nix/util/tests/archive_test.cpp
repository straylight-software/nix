// straylight // nix // util // tests
//
// Exhaustive property-based and fuzz tests for NAR (Nix Archive) format

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

// RapidCheck for property-based testing
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "straylight/nix/testing/temp_dir.h"

#include "nix/util/archive.h"
#include "nix/util/canon-path.h"
#include "nix/util/file-system.h"
#include "nix/util/fs-sink.h"
#include "nix/util/serialise.h"

namespace fs = std::filesystem;
namespace testing = straylight::nix::testing;

// =============================================================================
// Test Utilities
// =============================================================================

/**
 * RAII wrapper for a temporary directory that gets cleaned up on destruction.
 */
struct TempDir {
  fs::path path;

  TempDir() {
    auto temp = testing::temp_directory_path();
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 999999);
    path = temp / ("nix_archive_test_" + std::to_string(dis(gen)));
    fs::create_directories(path);
  }

  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

/**
 * In-memory sink to capture NAR output.
 */
struct CaptureSink : nix::sink_t {
  std::string data;

  void operator()(std::string_view s) override { data.append(s); }
};

/**
 * file_system_object_sink_t that records all operations for verification.
 */
struct RecordingSink : nix::file_system_object_sink_t {
  struct Entry {
    enum class Type { directory_t, RegularFile, symlink };
    Type type;
    nix::canon_path_t path;
    std::string contents; // for regular files
    std::string target;   // for symlinks
    bool executable = false;
  };

  std::vector<Entry> entries;

  void create_directory(const nix::canon_path_t& path) override {
    entries.push_back({Entry::Type::directory_t, path, {}, {}, false});
  }

  void
  create_regular_file(const nix::canon_path_t& path,
                      std::function<void(nix::create_regular_file_sink_t&)> write_fn) override {
    struct ContentCaptureSink : nix::create_regular_file_sink_t {
      std::string contents;
      bool is_exec_ = false;

      void operator()(std::string_view data) override { contents.append(data); }
      void is_executable() override { is_exec_ = true; }
    };

    ContentCaptureSink content_sink;
    write_fn(content_sink);
    entries.push_back(
        {Entry::Type::RegularFile, path, content_sink.contents, {}, content_sink.is_exec_});
  }

  void create_symlink(const nix::canon_path_t& path, const std::string& target) override {
    entries.push_back({Entry::Type::symlink, path, {}, target, false});
  }
};

/**
 * Generate a valid filename (no slashes, no NUL, not empty, not . or ..)
 */
rc::Gen<std::string> valid_filename_gen() {
  return rc::gen::suchThat(rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::oneOf(
                               rc::gen::inRange('a', 'z'), rc::gen::inRange('0', '9'),
                               rc::gen::element('_', '-', '.')))),
                           [](const std::string& s) { return s != "." && s != ".."; });
}

/**
 * Generate arbitrary binary content
 */
rc::Gen<std::string> binary_content_gen() {
  return rc::gen::container<std::string>(rc::gen::inRange<char>(0, 127));
}

/**
 * Helper to parse a NAR from a string
 */
void parse_nar_string(const std::string& nar_data, nix::file_system_object_sink_t& sink) {
  nix::string_source_t source(nar_data);
  nix::parse_dump(sink, source);
}

// =============================================================================
// NAR Magic & Basic Structure Tests
// =============================================================================

TEST_CASE("nar version magic constant", "[archive][nar]") {
  REQUIRE(nix::nar_version_magic1 == "nix-archive-1");
}

TEST_CASE("dump_string produces valid nar", "[archive][dump_string]") {
  CaptureSink sink;
  nix::dump_string("hello world", sink);

  // Should start with magic
  REQUIRE(sink.data.size() > nix::nar_version_magic1.size());

  // Verify it can be parsed
  RecordingSink recording;
  parse_nar_string(sink.data, recording);

  REQUIRE(recording.entries.size() == 1);
  REQUIRE(recording.entries[0].type == RecordingSink::Entry::Type::RegularFile);
  REQUIRE(recording.entries[0].contents == "hello world");
  REQUIRE(recording.entries[0].path == nix::canon_path_t::root);
}

TEST_CASE("dump_string empty content", "[archive][dump_string]") {
  CaptureSink sink;
  nix::dump_string("", sink);

  RecordingSink recording;
  parse_nar_string(sink.data, recording);

  REQUIRE(recording.entries.size() == 1);
  REQUIRE(recording.entries[0].contents.empty());
}

TEST_CASE("dump_string binary content", "[archive][dump_string]") {
  std::string binary_content;
  for (int i = 0; i < 256; ++i) {
    binary_content.push_back(static_cast<char>(i));
  }

  CaptureSink sink;
  nix::dump_string(binary_content, sink);

  RecordingSink recording;
  parse_nar_string(sink.data, recording);

  REQUIRE(recording.entries.size() == 1);
  REQUIRE(recording.entries[0].contents == binary_content);
}

// =============================================================================
// Roundtrip Tests - dumpPath/restorePath
// =============================================================================

TEST_CASE("roundtrip regular file", "[archive][roundtrip]") {
  TempDir source_dir;
  TempDir dest_dir;

  // Create source file
  auto source_file = source_dir.path / "test.txt";
  std::ofstream(source_file) << "hello world";

  // Dump to NAR
  CaptureSink nar_sink;
  nix::dump_path(source_file.string(), nar_sink);

  // Restore from NAR
  auto dest_file = dest_dir.path / "restored.txt";
  nix::string_source_t nar_source(nar_sink.data);
  nix::restore_path(dest_file, nar_source);

  // Verify
  REQUIRE(fs::exists(dest_file));
  std::ifstream restored(dest_file);
  std::string contents((std::istreambuf_iterator<char>(restored)),
                       std::istreambuf_iterator<char>());
  REQUIRE(contents == "hello world");
}

TEST_CASE("roundtrip empty file", "[archive][roundtrip]") {
  TempDir source_dir;
  TempDir dest_dir;

  auto source_file = source_dir.path / "empty.txt";
  {
    std::ofstream ofs(source_file);
  } // Create empty file

  CaptureSink nar_sink;
  nix::dump_path(source_file.string(), nar_sink);

  auto dest_file = dest_dir.path / "restored_empty.txt";
  nix::string_source_t nar_source(nar_sink.data);
  nix::restore_path(dest_file, nar_source);

  REQUIRE(fs::exists(dest_file));
  REQUIRE(fs::file_size(dest_file) == 0);
}

TEST_CASE("roundtrip executable file", "[archive][roundtrip]") {
  TempDir source_dir;
  TempDir dest_dir;

  auto source_file = source_dir.path / "script.sh";
  std::ofstream(source_file) << "#!/bin/bash\necho hello";
  fs::permissions(source_file,
                  fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);

  CaptureSink nar_sink;
  nix::dump_path(source_file.string(), nar_sink);

  auto dest_file = dest_dir.path / "restored_script.sh";
  nix::string_source_t nar_source(nar_sink.data);
  nix::restore_path(dest_file, nar_source);

  REQUIRE(fs::exists(dest_file));
  auto perms = fs::status(dest_file).permissions();
  REQUIRE((perms & fs::perms::owner_exec) != fs::perms::none);
}

TEST_CASE("roundtrip symlink", "[archive][roundtrip]") {
  TempDir source_dir;
  TempDir dest_dir;

  auto target_file = source_dir.path / "target.txt";
  std::ofstream(target_file) << "target content";

  auto symlink_path = source_dir.path / "link";
  fs::create_symlink("target.txt", symlink_path);

  CaptureSink nar_sink;
  nix::dump_path(symlink_path.string(), nar_sink);

  auto dest_link = dest_dir.path / "restored_link";
  nix::string_source_t nar_source(nar_sink.data);
  nix::restore_path(dest_link, nar_source);

  REQUIRE(fs::is_symlink(dest_link));
  REQUIRE(fs::read_symlink(dest_link) == "target.txt");
}

TEST_CASE("roundtrip directory structure", "[archive][roundtrip]") {
  TempDir source_dir;
  TempDir dest_dir;

  // Create directory structure
  auto root = source_dir.path / "root";
  fs::create_directories(root / "subdir1" / "nested");
  fs::create_directories(root / "subdir2");

  std::ofstream(root / "file1.txt") << "content1";
  std::ofstream(root / "subdir1" / "file2.txt") << "content2";
  std::ofstream(root / "subdir1" / "nested" / "file3.txt") << "content3";
  std::ofstream(root / "subdir2" / "file4.txt") << "content4";

  CaptureSink nar_sink;
  nix::dump_path(root.string(), nar_sink);

  auto dest_root = dest_dir.path / "restored_root";
  nix::string_source_t nar_source(nar_sink.data);
  nix::restore_path(dest_root, nar_source);

  // Verify structure
  REQUIRE(fs::is_directory(dest_root));
  REQUIRE(fs::is_directory(dest_root / "subdir1" / "nested"));
  REQUIRE(fs::is_directory(dest_root / "subdir2"));
  REQUIRE(fs::is_regular_file(dest_root / "file1.txt"));
  REQUIRE(fs::is_regular_file(dest_root / "subdir1" / "file2.txt"));
  REQUIRE(fs::is_regular_file(dest_root / "subdir1" / "nested" / "file3.txt"));
  REQUIRE(fs::is_regular_file(dest_root / "subdir2" / "file4.txt"));
}

// =============================================================================
// copyNAR Tests
// =============================================================================

TEST_CASE("copy_nar preserves content", "[archive][copy_nar]") {
  TempDir temp_dir;

  auto source_file = temp_dir.path / "source.txt";
  std::ofstream(source_file) << "test content for copyNAR";

  // Dump to NAR
  CaptureSink original_nar;
  nix::dump_path(source_file.string(), original_nar);

  // Copy NAR
  nix::string_source_t nar_source(original_nar.data);
  CaptureSink copied_nar;
  nix::copy_nar(nar_source, copied_nar);

  // Should be identical
  REQUIRE(original_nar.data == copied_nar.data);
}

// =============================================================================
// Malformed Archive Tests
// =============================================================================

TEST_CASE("parse_dump rejects empty input", "[archive][malformed]") {
  RecordingSink sink;
  std::string empty_str;
  nix::string_source_t source(empty_str);

  // Empty input throws EndOfFile when trying to read magic
  REQUIRE_THROWS(nix::parse_dump(sink, source));
}

TEST_CASE("parse_dump rejects wrong magic", "[archive][malformed]") {
  RecordingSink sink;

  // Build a properly formatted string with length prefix and content
  // NAR magic format: 8-byte length (little-endian) + string + padding
  // "wrong-magic-1" = 13 chars, padded to 16
  std::string wrong_magic;
  // Length: 13 as little-endian 64-bit
  wrong_magic.push_back('\x0d'); // 13
  for (int i = 0; i < 7; ++i) {
    wrong_magic.push_back('\x00');
  }
  wrong_magic += "wrong-magic-1";
  // Padding to 8-byte boundary (13 -> 16, so 3 bytes padding)
  wrong_magic.push_back('\x00');
  wrong_magic.push_back('\x00');
  wrong_magic.push_back('\x00');

  nix::string_source_t source(wrong_magic);

  REQUIRE_THROWS_AS(nix::parse_dump(sink, source), nix::SerialisationError);
}

TEST_CASE("parse_dump rejects truncated nar", "[archive][malformed]") {
  // Create a valid NAR first
  CaptureSink valid_nar;
  nix::dump_string("hello", valid_nar);

  // Truncate it
  std::string truncated = valid_nar.data.substr(0, valid_nar.data.size() / 2);

  RecordingSink sink;
  nix::string_source_t source(truncated);

  REQUIRE_THROWS(nix::parse_dump(sink, source));
}

TEST_CASE("parse_dump rejects invalid file names", "[archive][malformed]") {
  // Manually construct a NAR with an invalid filename

  // Test: filename with slash
  auto make_invalid_nar_with_filename = [](const std::string& filename) {
    CaptureSink sink;
    // magic
    sink << nix::nar_version_magic1;
    // type directory
    sink << "(";
    sink << "type";
    sink << "directory";
    // entry with invalid filename
    sink << "entry";
    sink << "(";
    sink << "name";
    sink << filename;
    sink << "node";
    sink << "(";
    sink << "type";
    sink << "regular";
    sink << "contents";
    sink << "";
    sink << ")";
    sink << ")";
    sink << ")";
    return sink.data;
  };

  // Test filename with slash
  {
    auto nar = make_invalid_nar_with_filename("bad/name");
    RecordingSink recording;
    nix::string_source_t source(nar);
    REQUIRE_THROWS_AS(nix::parse_dump(recording, source), nix::SerialisationError);
  }

  // Test filename with NUL
  {
    std::string filename_with_nul = "bad";
    filename_with_nul.push_back('\0');
    filename_with_nul += "name";
    auto nar = make_invalid_nar_with_filename(filename_with_nul);
    RecordingSink recording;
    nix::string_source_t source(nar);
    REQUIRE_THROWS_AS(nix::parse_dump(recording, source), nix::SerialisationError);
  }

  // Test "." filename
  {
    auto nar = make_invalid_nar_with_filename(".");
    RecordingSink recording;
    nix::string_source_t source(nar);
    REQUIRE_THROWS_AS(nix::parse_dump(recording, source), nix::SerialisationError);
  }

  // Test ".." filename
  {
    auto nar = make_invalid_nar_with_filename("..");
    RecordingSink recording;
    nix::string_source_t source(nar);
    REQUIRE_THROWS_AS(nix::parse_dump(recording, source), nix::SerialisationError);
  }

  // Test empty filename
  {
    auto nar = make_invalid_nar_with_filename("");
    RecordingSink recording;
    nix::string_source_t source(nar);
    REQUIRE_THROWS_AS(nix::parse_dump(recording, source), nix::SerialisationError);
  }
}

TEST_CASE("parse_dump rejects unsorted directory entries", "[archive][malformed]") {
  // NAR requires directory entries to be sorted lexicographically
  CaptureSink sink;
  sink << nix::nar_version_magic1;
  sink << "(";
  sink << "type";
  sink << "directory";
  // First entry "z"
  sink << "entry";
  sink << "(";
  sink << "name";
  sink << "z";
  sink << "node";
  sink << "(";
  sink << "type";
  sink << "regular";
  sink << "contents";
  sink << "";
  sink << ")";
  sink << ")";
  // Second entry "a" - violates sort order
  sink << "entry";
  sink << "(";
  sink << "name";
  sink << "a";
  sink << "node";
  sink << "(";
  sink << "type";
  sink << "regular";
  sink << "contents";
  sink << "";
  sink << ")";
  sink << ")";
  sink << ")";

  RecordingSink recording;
  nix::string_source_t source(sink.data);
  REQUIRE_THROWS_AS(nix::parse_dump(recording, source), nix::SerialisationError);
}

TEST_CASE("parse_dump rejects duplicate directory entries", "[archive][malformed]") {
  CaptureSink sink;
  sink << nix::nar_version_magic1;
  sink << "(";
  sink << "type";
  sink << "directory";
  // First entry "file"
  sink << "entry";
  sink << "(";
  sink << "name";
  sink << "file";
  sink << "node";
  sink << "(";
  sink << "type";
  sink << "regular";
  sink << "contents";
  sink << "a";
  sink << ")";
  sink << ")";
  // Duplicate entry "file"
  sink << "entry";
  sink << "(";
  sink << "name";
  sink << "file";
  sink << "node";
  sink << "(";
  sink << "type";
  sink << "regular";
  sink << "contents";
  sink << "b";
  sink << ")";
  sink << ")";
  sink << ")";

  RecordingSink recording;
  nix::string_source_t source(sink.data);
  REQUIRE_THROWS_AS(nix::parse_dump(recording, source), nix::SerialisationError);
}

TEST_CASE("parse_dump rejects unknown file type", "[archive][malformed]") {
  CaptureSink sink;
  sink << nix::nar_version_magic1;
  sink << "(";
  sink << "type";
  sink << "device"; // Unknown type
  sink << ")";

  RecordingSink recording;
  nix::string_source_t source(sink.data);
  REQUIRE_THROWS_AS(nix::parse_dump(recording, source), nix::SerialisationError);
}

TEST_CASE("parse_dump rejects non-empty executable marker", "[archive][malformed]") {
  CaptureSink sink;
  sink << nix::nar_version_magic1;
  sink << "(";
  sink << "type";
  sink << "regular";
  sink << "executable";
  sink << "yes"; // Should be empty string
  sink << "contents";
  sink << "";
  sink << ")";

  RecordingSink recording;
  nix::string_source_t source(sink.data);
  REQUIRE_THROWS_AS(nix::parse_dump(recording, source), nix::SerialisationError);
}

// =============================================================================
// Symlink Security Tests
// =============================================================================

TEST_CASE("symlink target can point outside", "[archive][symlink]") {
  // NAR allows symlinks to point anywhere - it's up to the caller to validate
  CaptureSink sink;
  sink << nix::nar_version_magic1;
  sink << "(";
  sink << "type";
  sink << "symlink";
  sink << "target";
  sink << "/etc/passwd"; // Absolute path pointing outside
  sink << ")";

  RecordingSink recording;
  nix::string_source_t source(sink.data);

  // Should parse successfully (NAR doesn't validate symlink targets)
  REQUIRE_NOTHROW(nix::parse_dump(recording, source));
  REQUIRE(recording.entries.size() == 1);
  REQUIRE(recording.entries[0].type == RecordingSink::Entry::Type::symlink);
  REQUIRE(recording.entries[0].target == "/etc/passwd");
}

TEST_CASE("symlink target with parent traversal", "[archive][symlink]") {
  CaptureSink sink;
  sink << nix::nar_version_magic1;
  sink << "(";
  sink << "type";
  sink << "symlink";
  sink << "target";
  sink << "../../../../etc/passwd"; // Relative path traversal
  sink << ")";

  RecordingSink recording;
  nix::string_source_t source(sink.data);

  // NAR format allows this - security checking is caller's responsibility
  REQUIRE_NOTHROW(nix::parse_dump(recording, source));
  REQUIRE(recording.entries[0].target == "../../../../etc/passwd");
}

TEST_CASE("nested symlink in directory", "[archive][symlink]") {
  TempDir temp_dir;

  auto root = temp_dir.path / "root";
  fs::create_directory(root);
  fs::create_symlink("../outside", root / "escape_link");

  CaptureSink nar_sink;
  nix::dump_path(root.string(), nar_sink);

  RecordingSink recording;
  parse_nar_string(nar_sink.data, recording);

  // Find the symlink entry
  bool found_symlink = false;
  for (const auto& entry : recording.entries) {
    if (entry.type == RecordingSink::Entry::Type::symlink) {
      found_symlink = true;
      REQUIRE(entry.target == "../outside");
    }
  }
  REQUIRE(found_symlink);
}

// =============================================================================
// Alignment and Padding Tests
// =============================================================================

TEST_CASE("nar content is 8-byte aligned", "[archive][alignment]") {
  // Test various content sizes to verify padding
  for (size_t size : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 100, 1000}) {
    std::string content(size, 'x');

    CaptureSink sink;
    nix::dump_string(content, sink);

    // Total NAR size should be 8-byte aligned
    REQUIRE(sink.data.size() % 8 == 0);

    // Should roundtrip correctly
    RecordingSink recording;
    parse_nar_string(sink.data, recording);
    REQUIRE(recording.entries[0].contents == content);
  }
}

// =============================================================================
// Property-Based Tests
// =============================================================================

TEST_CASE("dump_string roundtrip property", "[archive][property]") {
  rc::prop("any string content survives NAR roundtrip", []() {
    auto content = *binary_content_gen();

    CaptureSink sink;
    nix::dump_string(content, sink);

    RecordingSink recording;
    parse_nar_string(sink.data, recording);

    RC_ASSERT(recording.entries.size() == 1);
    RC_ASSERT(recording.entries[0].contents == content);
  });
}

TEST_CASE("dump_string deterministic property", "[archive][property]") {
  rc::prop("same content produces identical NAR", []() {
    auto content = *binary_content_gen();

    CaptureSink sink1;
    nix::dump_string(content, sink1);

    CaptureSink sink2;
    nix::dump_string(content, sink2);

    RC_ASSERT(sink1.data == sink2.data);
  });
}

TEST_CASE("nar size is always 8-byte aligned property", "[archive][property]") {
  rc::prop("NAR output is always 8-byte aligned", []() {
    auto content = *binary_content_gen();

    CaptureSink sink;
    nix::dump_string(content, sink);

    RC_ASSERT(sink.data.size() % 8 == 0);
  });
}

TEST_CASE("copy_nar preserves content property", "[archive][property]") {
  rc::prop("copyNAR produces identical output", []() {
    auto content = *binary_content_gen();

    CaptureSink original;
    nix::dump_string(content, original);

    nix::string_source_t source(original.data);
    CaptureSink copied;
    nix::copy_nar(source, copied);

    RC_ASSERT(original.data == copied.data);
  });
}

TEST_CASE("file system roundtrip property", "[archive][property]") {
  rc::prop("single file survives dump/restore roundtrip", []() {
    auto content = *binary_content_gen();

    TempDir source_dir;
    TempDir dest_dir;

    auto source_file = source_dir.path / "test.bin";
    {
      std::ofstream out(source_file, std::ios::binary);
      out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    CaptureSink nar_sink;
    nix::dump_path(source_file.string(), nar_sink);

    auto dest_file = dest_dir.path / "restored.bin";
    nix::string_source_t nar_source(nar_sink.data);
    nix::restore_path(dest_file, nar_source);

    std::ifstream in(dest_file, std::ios::binary);
    std::string restored((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    RC_ASSERT(restored == content);
  });
}

TEST_CASE("directory entries are sorted property", "[archive][property]") {
  rc::prop("directory entries are output in sorted order", []() {
    auto filenames = *rc::gen::unique<std::vector<std::string>>(valid_filename_gen());

    if (filenames.empty()) {
      return; // Skip trivial case
    }

    TempDir temp_dir;
    auto root = temp_dir.path / "root";
    fs::create_directory(root);

    for (const auto& name : filenames) {
      std::ofstream(root / name) << "content";
    }

    CaptureSink nar_sink;
    nix::dump_path(root.string(), nar_sink);

    RecordingSink recording;
    parse_nar_string(nar_sink.data, recording);

    // Extract file entries (skip the directory entry itself)
    std::vector<std::string> recorded_names;
    for (const auto& entry : recording.entries) {
      if (entry.type == RecordingSink::Entry::Type::RegularFile) {
        auto basename = entry.path.base_name();
        if (basename) {
          recorded_names.push_back(std::string(*basename));
        }
      }
    }

    // Verify sorted order
    for (size_t i = 1; i < recorded_names.size(); ++i) {
      RC_ASSERT(recorded_names[i - 1] < recorded_names[i]);
    }
  });
}

// =============================================================================
// Fuzz Tests
// =============================================================================

TEST_CASE("fuzz parse_dump with random bytes", "[archive][fuzz]") {
  rc::prop("random bytes don't crash parser", []() {
    auto garbage = *rc::gen::container<std::string>(rc::gen::arbitrary<char>());

    RecordingSink sink;
    nix::string_source_t source(garbage);

    // Should either parse or throw a well-defined exception, never crash
    try {
      nix::parse_dump(sink, source);
    } catch (const nix::SerialisationError&) {
      // Expected for malformed input
    } catch (const nix::EndOfFile&) {
      // Expected for truncated input
    } catch (const nix::Error&) {
      // Other nix errors are acceptable
    } catch (const std::exception&) {
      // Random bytes may decode as huge size fields, causing allocation failures
    }
    // If we get here without crashing, the test passes
  });
}

TEST_CASE("fuzz parse_dump with almost-valid nar", "[archive][fuzz]") {
  rc::prop("corrupted NAR doesn't crash parser", []() {
    // Generate a valid NAR first
    auto content = *binary_content_gen();
    CaptureSink valid_nar;
    nix::dump_string(content, valid_nar);

    // Corrupt it at a random position
    auto corruption_pos = *rc::gen::inRange<size_t>(0, valid_nar.data.size());
    auto corruption_byte = *rc::gen::arbitrary<char>();

    std::string corrupted = valid_nar.data;
    corrupted[corruption_pos] = corruption_byte;

    RecordingSink sink;
    nix::string_source_t source(corrupted);

    try {
      nix::parse_dump(sink, source);
    } catch (const nix::SerialisationError&) {
      // Expected for malformed input
    } catch (const nix::EndOfFile&) {
      // Expected for truncated input
    } catch (const nix::Error&) {
      // Other nix errors acceptable
    } catch (const std::exception&) {
      // Corrupted size fields can cause allocation failures - acceptable
    }
    // No crash = success
  });
}

TEST_CASE("fuzz parse_dump with truncated nar", "[archive][fuzz]") {
  rc::prop("truncated NAR doesn't crash parser", []() {
    auto content = *binary_content_gen();
    CaptureSink valid_nar;
    nix::dump_string(content, valid_nar);

    if (valid_nar.data.empty()) {
      return;
    }

    auto truncate_at = *rc::gen::inRange<size_t>(0, valid_nar.data.size());
    std::string truncated = valid_nar.data.substr(0, truncate_at);

    RecordingSink sink;
    nix::string_source_t source(truncated);

    try {
      nix::parse_dump(sink, source);
    } catch (const nix::SerialisationError&) {
      // Expected for malformed input
    } catch (const nix::EndOfFile&) {
      // Expected for truncated input
    } catch (const nix::Error&) {
      // Other nix errors acceptable
    } catch (const std::exception&) {
      // Truncated input may cause allocation failures - acceptable
    }
  });
}

TEST_CASE("fuzz valid nar with extra trailing data", "[archive][fuzz]") {
  rc::prop("NAR with trailing garbage parses correctly", []() {
    auto content = *binary_content_gen();
    CaptureSink valid_nar;
    nix::dump_string(content, valid_nar);

    auto garbage = *rc::gen::container<std::string>(rc::gen::arbitrary<char>());
    std::string with_trailing = valid_nar.data + garbage;

    RecordingSink sink;
    nix::string_source_t source(with_trailing);

    // Should parse the valid part
    REQUIRE_NOTHROW(nix::parse_dump(sink, source));
    RC_ASSERT(sink.entries.size() == 1);
    RC_ASSERT(sink.entries[0].contents == content);
  });
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_CASE("large file content", "[archive][edge_case]") {
  // Test with content larger than typical buffer sizes
  std::string large_content(128 * 1024, 'X'); // 128KB

  CaptureSink sink;
  nix::dump_string(large_content, sink);

  RecordingSink recording;
  parse_nar_string(sink.data, recording);

  REQUIRE(recording.entries[0].contents == large_content);
}

TEST_CASE("deeply nested directory structure", "[archive][edge_case]") {
  TempDir temp_dir;

  auto current = temp_dir.path / "root";
  fs::create_directory(current);

  // Create 20 levels of nesting
  for (int i = 0; i < 20; ++i) {
    current = current / ("level" + std::to_string(i));
    fs::create_directory(current);
  }

  // Add a file at the deepest level
  std::ofstream(current / "deep_file.txt") << "deep content";

  CaptureSink nar_sink;
  nix::dump_path((temp_dir.path / "root").string(), nar_sink);

  // Verify it can be parsed
  RecordingSink recording;
  parse_nar_string(nar_sink.data, recording);

  // Should have: 1 root + 20 subdirectories + 1 file = 22 entries
  REQUIRE(recording.entries.size() == 22);
}

TEST_CASE("many files in single directory", "[archive][edge_case]") {
  TempDir temp_dir;

  auto root = temp_dir.path / "root";
  fs::create_directory(root);

  // Create 100 files
  for (int i = 0; i < 100; ++i) {
    std::ofstream(root / ("file_" + std::to_string(i) + ".txt")) << "content " << i;
  }

  CaptureSink nar_sink;
  nix::dump_path(root.string(), nar_sink);

  RecordingSink recording;
  parse_nar_string(nar_sink.data, recording);

  // 1 directory + 100 files
  REQUIRE(recording.entries.size() == 101);
}

TEST_CASE("special characters in symlink target", "[archive][edge_case]") {
  TempDir temp_dir;

  auto root = temp_dir.path / "root";
  fs::create_directory(root);

  // Symlink with special characters in target
  fs::create_symlink("target with spaces", root / "link1");
  fs::create_symlink("target\twith\ttabs", root / "link2");
  fs::create_symlink("unicode_тест", root / "link3");

  CaptureSink nar_sink;
  nix::dump_path(root.string(), nar_sink);

  RecordingSink recording;
  parse_nar_string(nar_sink.data, recording);

  // Verify symlinks preserved
  int symlink_count = 0;
  for (const auto& entry : recording.entries) {
    if (entry.type == RecordingSink::Entry::Type::symlink) {
      symlink_count++;
    }
  }
  REQUIRE(symlink_count == 3);
}

TEST_CASE("binary content with all byte values", "[archive][edge_case]") {
  std::string all_bytes;
  for (int i = 0; i < 256; ++i) {
    all_bytes.push_back(static_cast<char>(i));
  }

  // Repeat to make it substantial
  std::string content;
  for (int i = 0; i < 100; ++i) {
    content += all_bytes;
  }

  CaptureSink sink;
  nix::dump_string(content, sink);

  RecordingSink recording;
  parse_nar_string(sink.data, recording);

  REQUIRE(recording.entries[0].contents == content);
}

TEST_CASE("empty directory", "[archive][edge_case]") {
  TempDir temp_dir;

  auto empty_dir = temp_dir.path / "empty";
  fs::create_directory(empty_dir);

  CaptureSink nar_sink;
  nix::dump_path(empty_dir.string(), nar_sink);

  RecordingSink recording;
  parse_nar_string(nar_sink.data, recording);

  REQUIRE(recording.entries.size() == 1);
  REQUIRE(recording.entries[0].type == RecordingSink::Entry::Type::directory_t);
}

// =============================================================================
// Mtime Tests
// =============================================================================

TEST_CASE("dump_path_and_get_mtime returns valid time", "[archive][mtime]") {
  TempDir temp_dir;

  auto test_file = temp_dir.path / "test.txt";
  std::ofstream(test_file) << "content";

  CaptureSink sink;
  auto mtime = nix::dump_path_and_get_mtime(test_file.string(), sink);

  // Mtime should be a reasonable Unix timestamp (after year 2000)
  REQUIRE(mtime > 946684800); // Jan 1, 2000
}

// =============================================================================
// PathFilter Tests
// =============================================================================

TEST_CASE("path_filter excludes files", "[archive][filter]") {
  TempDir temp_dir;

  auto root = temp_dir.path / "root";
  fs::create_directory(root);
  std::ofstream(root / "include.txt") << "included";
  std::ofstream(root / "exclude.txt") << "excluded";

  nix::path_filter_t filter = [](const nix::Path& path) {
    return path.find("exclude") == std::string::npos;
  };

  CaptureSink nar_sink;
  nix::dump_path(root.string(), nar_sink, filter);

  RecordingSink recording;
  parse_nar_string(nar_sink.data, recording);

  // Should only have directory + include.txt
  REQUIRE(recording.entries.size() == 2);

  bool found_include = false;
  bool found_exclude = false;
  for (const auto& entry : recording.entries) {
    if (entry.path.base_name() == "include.txt") {
      found_include = true;
    }
    if (entry.path.base_name() == "exclude.txt") {
      found_exclude = true;
    }
  }

  REQUIRE(found_include);
  REQUIRE_FALSE(found_exclude);
}
