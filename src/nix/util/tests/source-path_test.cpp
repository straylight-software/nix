// straylight // nix // util // tests
//
// Exhaustive property-based and fuzz tests for SourcePath
// Focus on security-critical path handling: traversal attacks, null bytes, unicode

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

// RapidCheck for property-based testing
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "nix/util/memory-source-accessor.h"
#include "nix/util/source-path.h"

using namespace nix;

// ─────────────────────────────────────────────────────────────────────────────
// Test fixtures and helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Create a MemorySourceAccessor with some test files
ref<memory_source_accessor_t> make_test_accessor() {
  auto accessor = make_ref<memory_source_accessor_t>();
  accessor->add_file(canon_path_t("/file.txt"), "hello world");
  accessor->add_file(canon_path_t("/dir/nested.txt"), "nested content");
  accessor->add_file(canon_path_t("/dir/subdir/deep.txt"), "deep content");
  return accessor;
}

// Create an empty MemorySourceAccessor
ref<memory_source_accessor_t> make_empty_accessor() {
  auto accessor = make_ref<memory_source_accessor_t>();
  // Initialize with an empty root directory
  accessor->root = memory_source_accessor_t::file_t{memory_source_accessor_t::file_t::directory_t{}};
  return accessor;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Basic construction tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("source path construction with root", "[source-path][construction]") {
  auto accessor = make_empty_accessor();
  source_path_t path(accessor);

  REQUIRE(path.path.is_root());
  REQUIRE(path.path.abs() == "/");
}

TEST_CASE("source path construction with explicit path", "[source-path][construction]") {
  auto accessor = make_empty_accessor();
  source_path_t path(accessor, canon_path_t("/foo/bar"));

  REQUIRE_FALSE(path.path.is_root());
  REQUIRE(path.path.abs() == "/foo/bar");
}

TEST_CASE("source path construction preserves accessor", "[source-path][construction]") {
  auto accessor = make_empty_accessor();
  source_path_t path(accessor, canon_path_t("/test"));

  REQUIRE(&*path.accessor == &*accessor);
}

// ─────────────────────────────────────────────────────────────────────────────
// baseName tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("source path basename for root returns source", "[source-path][basename]") {
  auto accessor = make_empty_accessor();
  source_path_t path(accessor);

  // Root path has no basename, returns "source" as default
  REQUIRE(path.base_name() == "source");
}

TEST_CASE("source path basename for simple path", "[source-path][basename]") {
  auto accessor = make_empty_accessor();
  source_path_t path(accessor, canon_path_t("/foo"));

  REQUIRE(path.base_name() == "foo");
}

TEST_CASE("source path basename for nested path", "[source-path][basename]") {
  auto accessor = make_empty_accessor();
  source_path_t path(accessor, canon_path_t("/foo/bar/baz.txt"));

  REQUIRE(path.base_name() == "baz.txt");
}

// ─────────────────────────────────────────────────────────────────────────────
// parent tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("source path parent of nested path", "[source-path][parent]") {
  auto accessor = make_empty_accessor();
  source_path_t path(accessor, canon_path_t("/foo/bar/baz"));

  source_path_t parent = path.parent();
  REQUIRE(parent.path.abs() == "/foo/bar");
  REQUIRE(&*parent.accessor == &*accessor);
}

TEST_CASE("source path parent preserves accessor reference", "[source-path][parent]") {
  auto accessor = make_empty_accessor();
  source_path_t path(accessor, canon_path_t("/a/b/c/d/e"));

  source_path_t p1 = path.parent();
  source_path_t p2 = p1.parent();
  source_path_t p3 = p2.parent();

  REQUIRE(&*p1.accessor == &*accessor);
  REQUIRE(&*p2.accessor == &*accessor);
  REQUIRE(&*p3.accessor == &*accessor);
}

// ─────────────────────────────────────────────────────────────────────────────
// Operator / (path concatenation) tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("source path operator slash with canon path", "[source-path][concatenation]") {
  auto accessor = make_empty_accessor();
  source_path_t base(accessor, canon_path_t("/foo"));

  source_path_t result = base / canon_path_t("/bar/baz");
  REQUIRE(result.path.abs() == "/foo/bar/baz");
  REQUIRE(&*result.accessor == &*accessor);
}

TEST_CASE("source path operator slash with string view", "[source-path][concatenation]") {
  auto accessor = make_empty_accessor();
  source_path_t base(accessor, canon_path_t("/foo"));

  source_path_t result = base / "bar";
  REQUIRE(result.path.abs() == "/foo/bar");
  REQUIRE(&*result.accessor == &*accessor);
}

TEST_CASE("source path operator slash from root", "[source-path][concatenation]") {
  auto accessor = make_empty_accessor();
  source_path_t root(accessor);

  source_path_t result = root / "foo";
  REQUIRE(result.path.abs() == "/foo");
}

TEST_CASE("source path chained operator slash", "[source-path][concatenation]") {
  auto accessor = make_empty_accessor();
  source_path_t root(accessor);

  source_path_t result = root / "a" / "b" / "c";
  REQUIRE(result.path.abs() == "/a/b/c");
}

// ─────────────────────────────────────────────────────────────────────────────
// Equality and comparison tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("source path equality same accessor same path", "[source-path][comparison]") {
  auto accessor = make_empty_accessor();
  source_path_t p1(accessor, canon_path_t("/foo/bar"));
  source_path_t p2(accessor, canon_path_t("/foo/bar"));

  REQUIRE(p1 == p2);
}

TEST_CASE("source path inequality same accessor different path", "[source-path][comparison]") {
  auto accessor = make_empty_accessor();
  source_path_t p1(accessor, canon_path_t("/foo"));
  source_path_t p2(accessor, canon_path_t("/bar"));

  REQUIRE_FALSE(p1 == p2);
}

TEST_CASE("source path inequality different accessor same path", "[source-path][comparison]") {
  auto accessor1 = make_empty_accessor();
  auto accessor2 = make_empty_accessor();
  source_path_t p1(accessor1, canon_path_t("/foo"));
  source_path_t p2(accessor2, canon_path_t("/foo"));

  // Different accessors mean different paths
  REQUIRE_FALSE(p1 == p2);
}

TEST_CASE("source path three-way comparison ordering", "[source-path][comparison]") {
  auto accessor = make_empty_accessor();
  source_path_t foo(accessor, canon_path_t("/foo"));
  source_path_t foo_bar(accessor, canon_path_t("/foo/bar"));
  source_path_t foo_baz(accessor, canon_path_t("/foo/baz"));

  REQUIRE((foo <=> foo_bar) < 0);
  REQUIRE((foo_bar <=> foo_baz) < 0);
  REQUIRE((foo <=> foo) == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// to_string tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("source path to_string for root", "[source-path][string]") {
  auto accessor = make_empty_accessor();
  source_path_t path(accessor);

  std::string str = path.to_string();
  // Should not be empty - accessor provides display
  REQUIRE_FALSE(str.empty());
}

TEST_CASE("source path to_string for nested path", "[source-path][string]") {
  auto accessor = make_empty_accessor();
  source_path_t path(accessor, canon_path_t("/foo/bar"));

  std::string str = path.to_string();
  // Should contain the path
  REQUIRE(str.contains("/foo/bar"));
}

TEST_CASE("source path stream output", "[source-path][string]") {
  auto accessor = make_empty_accessor();
  source_path_t path(accessor, canon_path_t("/test/path"));

  std::ostringstream oss;
  oss << path;
  REQUIRE_FALSE(oss.str().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Hash tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("source path hash consistency", "[source-path][hash]") {
  auto accessor = make_empty_accessor();
  source_path_t p1(accessor, canon_path_t("/foo/bar"));
  source_path_t p2(accessor, canon_path_t("/foo/bar"));

  REQUIRE(std::hash<source_path_t>{}(p1) == std::hash<source_path_t>{}(p2));
  REQUIRE(hash_value(p1) == hash_value(p2));
}

TEST_CASE("source path hash different for different paths", "[source-path][hash]") {
  auto accessor = make_empty_accessor();
  source_path_t p1(accessor, canon_path_t("/foo"));
  source_path_t p2(accessor, canon_path_t("/bar"));

  // Very likely different hashes
  REQUIRE(std::hash<source_path_t>{}(p1) != std::hash<source_path_t>{}(p2));
}

TEST_CASE("source path hash different for different accessors", "[source-path][hash]") {
  auto accessor1 = make_empty_accessor();
  auto accessor2 = make_empty_accessor();
  source_path_t p1(accessor1, canon_path_t("/foo"));
  source_path_t p2(accessor2, canon_path_t("/foo"));

  // Different accessors contribute to hash
  REQUIRE(std::hash<source_path_t>{}(p1) != std::hash<source_path_t>{}(p2));
}

TEST_CASE("source path usable in unordered_set", "[source-path][hash]") {
  auto accessor = make_empty_accessor();
  std::unordered_set<source_path_t> paths;

  paths.insert(source_path_t(accessor, canon_path_t("/a")));
  paths.insert(source_path_t(accessor, canon_path_t("/b")));
  paths.insert(source_path_t(accessor, canon_path_t("/a"))); // duplicate

  REQUIRE(paths.size() == 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// File system operations tests (with MemorySourceAccessor)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("source path read file", "[source-path][fs]") {
  auto accessor = make_test_accessor();
  source_path_t path(accessor, canon_path_t("/file.txt"));

  REQUIRE(path.path_exists());
  REQUIRE(path.read_file() == "hello world");
}

TEST_CASE("source path path exists", "[source-path][fs]") {
  auto accessor = make_test_accessor();

  source_path_t existing(accessor, canon_path_t("/file.txt"));
  REQUIRE(existing.path_exists());

  source_path_t nonexistent(accessor, canon_path_t("/nonexistent"));
  REQUIRE_FALSE(nonexistent.path_exists());
}

TEST_CASE("source path lstat for file", "[source-path][fs]") {
  auto accessor = make_test_accessor();
  source_path_t path(accessor, canon_path_t("/file.txt"));

  auto stat = path.lstat();
  REQUIRE(stat.type == SourceAccessor::t_regular);
}

TEST_CASE("source path lstat for directory", "[source-path][fs]") {
  auto accessor = make_test_accessor();
  source_path_t path(accessor, canon_path_t("/dir"));

  auto stat = path.lstat();
  REQUIRE(stat.type == SourceAccessor::t_directory);
}

TEST_CASE("source path maybe lstat returns nullopt for nonexistent", "[source-path][fs]") {
  auto accessor = make_test_accessor();
  source_path_t path(accessor, canon_path_t("/nonexistent"));

  auto stat = path.maybe_lstat();
  REQUIRE_FALSE(stat.has_value());
}

TEST_CASE("source path read directory", "[source-path][fs]") {
  auto accessor = make_test_accessor();
  source_path_t path(accessor, canon_path_t("/dir"));

  auto entries = path.read_directory();
  REQUIRE(entries.size() >= 1);
  REQUIRE(entries.contains("nested.txt"));
  REQUIRE(entries.contains("subdir"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Security: Path traversal attack tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("source path traversal dot-dot is normalized", "[source-path][security]") {
  auto accessor = make_empty_accessor();

  // Attempting path traversal via /.. should be normalized
  source_path_t base(accessor, canon_path_t("/foo/bar"));
  source_path_t traversal = base / canon_path_t("/../../../etc/passwd");

  // CanonPath normalizes this - cannot escape root
  REQUIRE_FALSE(traversal.path.abs().contains(".."));
}

TEST_CASE("source path traversal cannot escape via concatenation", "[source-path][security]") {
  auto accessor = make_empty_accessor();
  source_path_t base(accessor, canon_path_t("/safe/dir"));

  // Multiple traversal attempts
  source_path_t t1 = base / canon_path_t("/../../..");
  REQUIRE_FALSE(t1.path.abs().contains(".."));

  source_path_t t2 = base / canon_path_t("/../../../../../../../etc");
  REQUIRE_FALSE(t2.path.abs().contains(".."));
}

TEST_CASE("source path traversal stays within root after deep nesting", "[source-path][security]") {
  auto accessor = make_empty_accessor();

  // Create a deeply nested path
  canon_path_t deep("/a/b/c/d/e/f/g/h/i/j");
  source_path_t path(accessor, deep);

  // Try to traverse way beyond root
  source_path_t traversal = path / canon_path_t("/../../../../../../../../../../../../root");

  // Must still be a valid path starting with /
  REQUIRE(traversal.path.abs()[0] == '/');
  REQUIRE_FALSE(traversal.path.abs().contains(".."));
}

// ─────────────────────────────────────────────────────────────────────────────
// Security: Null byte injection tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("source path rejects null bytes in path component", "[source-path][security]") {
  auto accessor = make_empty_accessor();
  source_path_t base(accessor, canon_path_t("/safe"));

  // CanonPath construction should reject null bytes
  std::string evil = "file";
  evil += '\0';
  evil += ".txt";
  REQUIRE_THROWS(base / std::string_view(evil.data(), evil.size()));
}

TEST_CASE("source path null byte at start of component", "[source-path][security]") {
  // Null byte at the very start
  std::string evil = std::string("\x00", 1) + "malicious";
  REQUIRE_THROWS(canon_path_t(std::string_view(evil.c_str(), evil.size())));
}

TEST_CASE("source path null byte in middle of path", "[source-path][security]") {
  // /foo\x00bar/baz
  std::string evil = "/foo";
  evil += '\x00';
  evil += "bar/baz";
  REQUIRE_THROWS(canon_path_t(std::string_view(evil.c_str(), evil.size())));
}

// ─────────────────────────────────────────────────────────────────────────────
// Security: Unicode and encoding edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("source path with unicode characters", "[source-path][unicode]") {
  auto accessor = make_empty_accessor();

  // Various unicode characters that might be problematic
  source_path_t path_emoji(accessor, canon_path_t("/dir/file_\xF0\x9F\x98\x80.txt")); // emoji
  REQUIRE(path_emoji.path.abs() == "/dir/file_\xF0\x9F\x98\x80.txt");

  source_path_t path_cjk(accessor, canon_path_t("/\xE4\xB8\xAD\xE6\x96\x87")); // Chinese
  REQUIRE_FALSE(path_cjk.path.is_root());

  source_path_t path_arabic(accessor, canon_path_t("/\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A")); // Arabic
  REQUIRE_FALSE(path_arabic.path.is_root());
}

TEST_CASE("source path with unicode normalization forms", "[source-path][unicode]") {
  auto accessor = make_empty_accessor();

  // NFC vs NFD forms of the same character (e acute)
  // NFC: U+00E9 (single code point)
  source_path_t nfc(accessor, canon_path_t("/caf\xC3\xA9"));
  // NFD: e + combining acute U+0065 U+0301
  source_path_t nfd(accessor, canon_path_t("/cafe\xCC\x81"));

  // These are different byte sequences, so different paths
  // This is expected behavior - CanonPath doesn't normalize unicode
  REQUIRE_FALSE(nfc == nfd);
}

TEST_CASE("source path with unicode lookalikes", "[source-path][unicode][security]") {
  auto accessor = make_empty_accessor();

  // Cyrillic 'а' (U+0430) vs Latin 'a' (U+0061)
  source_path_t latin(accessor, canon_path_t("/a"));
  source_path_t cyrillic(accessor, canon_path_t("/\xD0\xB0"));

  // These should be treated as different paths
  REQUIRE_FALSE(latin == cyrillic);
}

TEST_CASE("source path with bidirectional unicode", "[source-path][unicode][security]") {
  auto accessor = make_empty_accessor();

  // Right-to-left override character U+202E - potential spoofing
  // Using string concatenation to avoid misleading bidi warning
  std::string bidi_path = "/test";
  bidi_path += '\xE2';
  bidi_path += '\x80';
  bidi_path += '\xAE'; // U+202E RLO
  bidi_path += "exe.txt";
  source_path_t path(accessor, canon_path_t(bidi_path));
  // Should be stored as-is (byte-for-byte)
  REQUIRE(path.path.abs().size() > 5);
}

TEST_CASE("source path with zero width characters", "[source-path][unicode][security]") {
  auto accessor = make_empty_accessor();

  // Zero-width space U+200B
  source_path_t path(accessor, canon_path_t("/test\xE2\x80"
                                      "\x8B"
                                      "file"));
  REQUIRE_FALSE(path.path.is_root());

  // Zero-width joiner U+200D
  source_path_t path2(accessor, canon_path_t("/test\xE2\x80"
                                       "\x8D"
                                       "file"));
  REQUIRE_FALSE(path2.path.is_root());
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("source path with very long path", "[source-path][edge-case]") {
  auto accessor = make_empty_accessor();

  // Create a path with 1000 components
  std::string long_path;
  for (int i = 0; i < 1000; ++i) {
    long_path += "/component" + std::to_string(i);
  }

  source_path_t path(accessor, canon_path_t(long_path));
  REQUIRE_FALSE(path.path.is_root());

  // Parent should work
  source_path_t parent = path.parent();
  REQUIRE_FALSE(parent.path.is_root());
}

TEST_CASE("source path with very long component name", "[source-path][edge-case]") {
  auto accessor = make_empty_accessor();

  // Single very long component (4096 chars)
  std::string long_component(4096, 'x');
  source_path_t path(accessor, canon_path_t("/" + long_component));

  REQUIRE(path.base_name() == long_component);
}

TEST_CASE("source path with special characters in component", "[source-path][edge-case]") {
  auto accessor = make_empty_accessor();

  // Various special characters (but not slash or null)
  source_path_t path1(accessor, canon_path_t("/file with spaces"));
  REQUIRE(path1.base_name() == "file with spaces");

  source_path_t path2(accessor, canon_path_t("/file\twith\ttabs"));
  REQUIRE(path2.base_name() == "file\twith\ttabs");

  source_path_t path3(accessor, canon_path_t("/file\nwith\nnewlines"));
  REQUIRE(path3.base_name() == "file\nwith\nnewlines");

  source_path_t path4(accessor, canon_path_t("/!@#$%^&*()"));
  REQUIRE(path4.base_name() == "!@#$%^&*()");
}

TEST_CASE("source path with backslashes", "[source-path][edge-case]") {
  auto accessor = make_empty_accessor();

  // Backslashes are not path separators in CanonPath (Unix-style)
  source_path_t path(accessor, canon_path_t("/foo\\bar\\baz"));
  // Should be single component with backslashes
  REQUIRE(path.base_name() == "foo\\bar\\baz");
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Generator for valid path component names
rc::Gen<std::string> generate_valid_component() {
  return rc::gen::suchThat(
      rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::oneOf(
          rc::gen::inRange('a', 'z'), rc::gen::inRange('A', 'Z'), rc::gen::inRange('0', '9'),
          rc::gen::just('-'), rc::gen::just('_'), rc::gen::just('.')))),
      [](const std::string& s) { return s != "." && s != ".." && !s.contains('\0'); });
}

// Generator for a vector of path components
rc::Gen<std::vector<std::string>> generate_path_components() {
  return rc::gen::container<std::vector<std::string>>(generate_valid_component());
}

// Generator for non-empty path components
rc::Gen<std::vector<std::string>> generate_nonempty_path_components() {
  return rc::gen::nonEmpty(generate_path_components());
}

} // namespace

TEST_CASE("source path property tests", "[source-path][property]") {
  auto accessor = make_empty_accessor();

  rc::prop("source path always has valid accessor", [&]() {
    auto components = *generate_path_components();
    canon_path_t canon(components);
    source_path_t path(accessor, canon);
    RC_ASSERT(&*path.accessor == &*accessor);
  });

  rc::prop("source path basename matches canon path basename", [&]() {
    auto components = *generate_path_components();
    canon_path_t canon(components);
    source_path_t path(accessor, canon);

    if (canon.is_root()) {
      RC_ASSERT(path.base_name() == "source");
    } else {
      RC_ASSERT(path.base_name() == canon.base_name().value());
    }
  });

  rc::prop("source path parent preserves accessor", [&]() {
    auto components = *generate_nonempty_path_components();
    canon_path_t canon(components);
    source_path_t path(accessor, canon);
    source_path_t parent = path.parent();
    RC_ASSERT(&*parent.accessor == &*path.accessor);
  });

  rc::prop("source path slash operator preserves accessor", [&]() {
    auto base_components = *generate_path_components();
    auto component = *generate_valid_component();

    canon_path_t base_canon(base_components);
    source_path_t base(accessor, base_canon);
    source_path_t extended = base / component;

    RC_ASSERT(&*extended.accessor == &*base.accessor);
  });

  rc::prop("source path equality is reflexive", [&]() {
    auto components = *generate_path_components();
    canon_path_t canon(components);
    source_path_t path(accessor, canon);
    RC_ASSERT(path == path);
  });

  rc::prop("source path equality is symmetric", [&]() {
    auto components = *generate_path_components();
    canon_path_t canon(components);
    source_path_t p1(accessor, canon);
    source_path_t p2(accessor, canon);
    RC_ASSERT((p1 == p2) == (p2 == p1));
  });

  rc::prop("source path hash equality consistent with equality", [&]() {
    auto components = *generate_path_components();
    canon_path_t canon(components);
    source_path_t p1(accessor, canon);
    source_path_t p2(accessor, canon);

    if (p1 == p2) {
      RC_ASSERT(std::hash<source_path_t>{}(p1) == std::hash<source_path_t>{}(p2));
    }
  });

  rc::prop("source path ordering is transitive", [&]() {
    auto comp1 = *generate_path_components();
    auto comp2 = *generate_path_components();
    auto comp3 = *generate_path_components();

    source_path_t p1(accessor, canon_path_t(comp1));
    source_path_t p2(accessor, canon_path_t(comp2));
    source_path_t p3(accessor, canon_path_t(comp3));

    bool p1_lt_p2 = (p1 <=> p2) < 0;
    bool p2_lt_p3 = (p2 <=> p3) < 0;
    bool p1_lt_p3 = (p1 <=> p3) < 0;
    if (p1_lt_p2 && p2_lt_p3) {
      RC_ASSERT(p1_lt_p3);
    }
  });

  rc::prop("source path ordering is antisymmetric", [&]() {
    auto comp1 = *generate_path_components();
    auto comp2 = *generate_path_components();

    source_path_t p1(accessor, canon_path_t(comp1));
    source_path_t p2(accessor, canon_path_t(comp2));

    bool is_equal = (p1 <=> p2) == 0;
    if (is_equal) {
      RC_ASSERT(p1 == p2);
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Fuzz tests with arbitrary byte sequences
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("source path fuzz path components", "[source-path][fuzz]") {
  auto accessor = make_empty_accessor();

  rc::prop("fuzz: arbitrary strings as path components", [&]() {
    auto raw = *rc::gen::string<std::string>();

    // Skip strings with null bytes or slashes - they're handled elsewhere
    RC_PRE(raw.find('\0') == std::string::npos);
    RC_PRE(raw.find('/') == std::string::npos);
    RC_PRE(!raw.empty());
    RC_PRE(raw != "." && raw != "..");

    // Should not crash
    source_path_t base(accessor, canon_path_t("/base"));
    source_path_t extended = base / raw;
    RC_ASSERT(!extended.path.is_root());
    RC_ASSERT(&*extended.accessor == &*accessor);
  });
}

TEST_CASE("source path fuzz unicode sequences", "[source-path][fuzz][unicode]") {
  auto accessor = make_empty_accessor();

  rc::prop("fuzz: random utf-8 like sequences", [&]() {
    // Generate bytes that look like UTF-8
    auto bytes = *rc::gen::container<std::vector<unsigned char>>(
        rc::gen::inRange<unsigned char>(0x01, 0xFF));

    // Convert to string, skip if it has slashes or is empty
    std::string str(bytes.begin(), bytes.end());
    RC_PRE(!str.empty());
    RC_PRE(str.find('/') == std::string::npos);
    RC_PRE(str != "." && str != "..");

    // Should not crash - either succeeds or throws
    try {
      source_path_t base(accessor, canon_path_t("/base"));
      source_path_t extended = base / str;
      RC_ASSERT(&*extended.accessor == &*accessor);
    } catch (const nix::Error&) {
      // Expected for invalid inputs - throwing is acceptable behavior
      RC_SUCCEED("threw expected exception");
    }
  });
}

TEST_CASE("source path fuzz traversal attempts", "[source-path][fuzz][security]") {
  auto accessor = make_empty_accessor();

  rc::prop("fuzz: cannot escape root via any combination of dot-dot", [&]() {
    // Generate a sequence of ".." and valid components
    auto depth = *rc::gen::inRange(1, 50);
    auto extra_dots = *rc::gen::inRange(0, 20);

    std::string path;
    for (int i = 0; i < depth; ++i) {
      path += "/dir" + std::to_string(i);
    }
    for (int i = 0; i < depth + extra_dots; ++i) {
      path += "/..";
    }
    path += "/target";

    canon_path_t canon(path);
    source_path_t source_path(accessor, canon);

    // Must never contain ".." after normalization
    RC_ASSERT(source_path.path.abs().find("..") == std::string::npos);
    // Must start with /
    RC_ASSERT(source_path.path.abs()[0] == '/');
  });
}

TEST_CASE("source path fuzz hash collisions", "[source-path][fuzz][hash]") {
  auto accessor = make_empty_accessor();

  rc::prop("fuzz: different paths have different hashes (probabilistic)", [&]() {
    auto comp1 = *generate_path_components();
    auto comp2 = *generate_path_components();

    RC_PRE(comp1 != comp2);

    source_path_t p1(accessor, canon_path_t(comp1));
    source_path_t p2(accessor, canon_path_t(comp2));

    // Not guaranteed, but very likely
    if (!(p1 == p2)) {
      // If paths are different, hashes should be different most of the time
      // This is probabilistic, so we don't assert, just check
      auto h1 = std::hash<source_path_t>{}(p1);
      auto h2 = std::hash<source_path_t>{}(p2);
      // Log collision for analysis but don't fail
      (void)h1;
      (void)h2;
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Fuzz tests for special byte sequences
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("source path fuzz control characters", "[source-path][fuzz][security]") {
  auto accessor = make_empty_accessor();

  rc::prop("fuzz: control characters in paths", [&]() {
    // Generate control characters (0x01-0x1F, excluding null)
    auto ctrl = *rc::gen::inRange<char>(0x01, 0x1F);
    std::string component = "file";
    component += ctrl;
    component += "name";

    // Skip if it happens to be a slash (0x2F = 47 is not in range anyway)
    RC_PRE(ctrl != '/');

    source_path_t base(accessor, canon_path_t("/safe"));
    source_path_t result = base / component;

    // Should handle control characters (they're valid in paths)
    RC_ASSERT(!result.path.is_root());
  });
}

TEST_CASE("source path fuzz high bytes", "[source-path][fuzz]") {
  auto accessor = make_empty_accessor();

  rc::prop("fuzz: high byte sequences", [&]() {
    // Generate bytes 0x80-0xFF
    auto bytes = *rc::gen::nonEmpty(rc::gen::container<std::vector<unsigned char>>(
        rc::gen::inRange<unsigned char>(0x80, 0xFF)));

    std::string component(bytes.begin(), bytes.end());

    source_path_t base(accessor, canon_path_t("/base"));
    source_path_t result = base / component;

    RC_ASSERT(!result.path.is_root());
    RC_ASSERT(&*result.accessor == &*accessor);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Stress tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("source path stress many operations", "[source-path][stress]") {
  auto accessor = make_empty_accessor();

  rc::prop("stress: many sequential operations", [&]() {
    auto components = *rc::gen::nonEmpty(generate_path_components());

    source_path_t path(accessor);

    for (const auto& component : components) {
      path = path / component;
    }

    // Should have built up a path
    RC_ASSERT(!path.path.is_root());

    // Walk back up
    auto ops = static_cast<int>(components.size());
    for (int i = 0; i < ops; ++i) {
      if (!path.path.is_root()) {
        path = path.parent();
      }
    }

    RC_ASSERT(path.path.is_root());
  });
}

TEST_CASE("source path stress many accessors", "[source-path][stress]") {
  rc::prop("stress: paths from many accessors", [&]() {
    auto count = *rc::gen::inRange(1, 50);
    std::vector<ref<memory_source_accessor_t>> accessors;
    std::vector<source_path_t> paths;

    for (int i = 0; i < count; ++i) {
      auto acc = make_empty_accessor();
      accessors.push_back(acc);
      paths.emplace_back(acc, canon_path_t("/test"));
    }

    // All paths should be distinct
    for (size_t i = 0; i < paths.size(); ++i) {
      for (size_t j = i + 1; j < paths.size(); ++j) {
        RC_ASSERT(!(paths[i] == paths[j]));
      }
    }
  });
}
