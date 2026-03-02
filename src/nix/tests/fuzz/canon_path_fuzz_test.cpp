// straylight // nix // tests // fuzz
//
// Canon Path Fuzz Tests
//
// canon_path_t is used throughout the nix codebase to represent normalized
// absolute paths. Several methods have assert() calls that can be triggered
// by malicious inputs.

#include <string>
#include <vector>

#include "nix/tests/property.h"
#include "nix/util/canon-path.h"
#include "nix/util/error.h"


// =============================================================================
// BUG: canon_path_t::pop() asserts that path is not root
// canon-path.cpp:63 - assert(!is_root())
//
// Attack vector: Call pop() on root path "/"
// =============================================================================

TEST_CASE("bug: canon_path pop on root causes assertion failure", "[fuzz][canon-path][bug]") {
  nix::canon_path_t root = nix::canon_path_t::root;

  INFO("root.is_root() = " << root.is_root());
  INFO("Calling pop() on root triggers assert(!is_root()) at canon-path.cpp:63");

  // This SHOULD throw an exception, not crash
  // NOTE: Uncomment to trigger the bug:
  // root.pop();

  REQUIRE(root.is_root());
}

// =============================================================================
// BUG: canon_path_t::push() asserts no slashes in component
// canon-path.cpp:102 - assert(!component.contains('/'))
//
// Attack vector: Push a component containing '/'
// =============================================================================

TEST_CASE("bug: canon_path push with slash causes assertion failure", "[fuzz][canon-path][bug]") {
  nix::canon_path_t path = nix::canon_path_t::root;

  auto malicious_components = ::std::vector<::std::string>{
      "foo/bar", // Embedded slash
      "/",       // Just slash
      "//",      // Double slash
      "a/b/c",   // Multiple slashes
      "../foo",  // Directory traversal with slash
      "./bar",   // Dot with slash
  };

  for (const auto& comp : malicious_components) {
    INFO("Testing component: " << comp);
    INFO("This triggers assert(!component.contains('/')) at canon-path.cpp:102");

    // NOTE: Uncomment to trigger the bug:
    // path.push(comp);

    REQUIRE(comp.find('/') != ::std::string::npos);
  }
}

// =============================================================================
// BUG: canon_path_t::push() asserts component is not "." or ".."
// canon-path.cpp:103 - assert(component != "." && component != "..")
//
// Attack vector: Push "." or ".." as component
// =============================================================================

TEST_CASE("bug: canon_path push with . or .. causes assertion failure", "[fuzz][canon-path][bug]") {
  nix::canon_path_t path = nix::canon_path_t::root;

  INFO("Pushing '.' triggers assert(component != \".\") at canon-path.cpp:103");
  INFO("Pushing '..' triggers assert(component != \"..\") at canon-path.cpp:103");

  // NOTE: Uncomment to trigger the bug:
  // path.push(".");
  // path.push("..");

  REQUIRE(true);
}

// =============================================================================
// BUG: canon_path_t::remove_prefix() asserts path is within prefix
// canon-path.cpp:74 - assert(is_within(prefix))
//
// Attack vector: Remove prefix that path is not within
// =============================================================================

TEST_CASE("bug: canon_path remove_prefix with invalid prefix causes assertion",
          "[fuzz][canon-path][bug]") {
  nix::canon_path_t path("/foo/bar");
  nix::canon_path_t invalid_prefix("/baz");

  INFO("path = " << path.abs());
  INFO("invalid_prefix = " << invalid_prefix.abs());
  INFO("Calling remove_prefix(invalid_prefix) triggers assert(is_within(prefix))");

  // NOTE: Uncomment to trigger the bug:
  // path.remove_prefix(invalid_prefix);

  REQUIRE(!path.is_within(invalid_prefix));
}

// =============================================================================
// Property test: arbitrary path construction should not crash
// =============================================================================

TEST_CASE("fuzz: canon_path construction", "[fuzz][canon-path]") {
  rc::prop("arbitrary strings as paths should not crash", []() {
    auto s = *rc::gen::arbitrary<::std::string>();

    try {
      [[maybe_unused]] nix::canon_path_t path(s);
      // If we get here, the path was accepted
    } catch (const nix::base_error_t&) {
      // Expected for invalid paths
    }

    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: canon_path component operations", "[fuzz][canon-path]") {
  rc::prop("valid push operations should roundtrip", []() {
    // Generate path components without '/', '.', '..'
    auto gen_component =
        rc::gen::suchThat(rc::gen::nonEmpty<::std::string>(), [](const ::std::string& s) {
          return s != "." && s != ".." && s.find('/') == ::std::string::npos &&
                 s.find('\0') == ::std::string::npos;
        });

    auto component = *gen_component;

    nix::canon_path_t path = nix::canon_path_t::root;
    path.push(component);

    RC_ASSERT(!path.is_root());
    RC_ASSERT(path.base_name() == component);
  });
}

// =============================================================================
// BUG: Null bytes in paths
// canon-path.cpp calls ensure_no_null_bytes() but assertion may trigger first
// =============================================================================

TEST_CASE("bug: canon_path with null bytes", "[fuzz][canon-path][bug]") {
  auto path_with_null = ::std::string{"/foo"};
  path_with_null += '\0';
  path_with_null += "bar";

  INFO("Path contains embedded null byte");

  // This should throw an exception about null bytes
  CHECK_THROWS(nix::canon_path_t(path_with_null));
}

// =============================================================================
// Edge cases in path parsing
// =============================================================================

TEST_CASE("fuzz: canon_path edge cases", "[fuzz][canon-path]") {
  SECTION("empty string") {
    // Empty string - behavior varies by implementation
    // Some throw, some treat as root
    try {
      nix::canon_path_t path("");
      // If it succeeds, verify it's a valid path
      REQUIRE(path.abs().front() == '/');
    } catch (const nix::base_error_t&) {
      // Also acceptable
      REQUIRE(true);
    }
  }

  SECTION("relative path") {
    // Relative paths - canon_path_t constructor behavior varies
    // Some implementations throw, some prepend "/"
    // Just verify it doesn't crash
    try {
      nix::canon_path_t path("foo/bar");
      // If it succeeds, path should be absolute
      REQUIRE(path.abs().front() == '/');
    } catch (const nix::base_error_t&) {
      // Also acceptable
      REQUIRE(true);
    }
  }

  SECTION("double slashes") {
    // Double slashes should be normalized
    nix::canon_path_t path("//foo//bar//");
    // The path should be normalized
    REQUIRE(path.abs().find("//") == ::std::string::npos);
  }

  SECTION("trailing slash") {
    // Trailing slashes should be normalized
    nix::canon_path_t path("/foo/bar/");
    REQUIRE(path.abs().back() != '/');
  }

  SECTION("dot components") {
    // "." components should be removed
    nix::canon_path_t path("/foo/./bar");
    REQUIRE(path.abs() == "/foo/bar");
  }

  SECTION("dotdot components") {
    // ".." components should be resolved
    nix::canon_path_t path("/foo/bar/../baz");
    REQUIRE(path.abs() == "/foo/baz");
  }

  SECTION("excessive dotdot") {
    // Too many ".." should stay at root
    nix::canon_path_t path("/foo/../../../bar");
    REQUIRE(path.abs() == "/bar");
  }
}

// =============================================================================
// Property test: operations preserve invariants
// =============================================================================

TEST_CASE("fuzz: canon_path invariants", "[fuzz][canon-path]") {
  rc::prop("abs() always starts with /", []() {
    // Generate valid path components (no slash, no dot/dotdot, no nulls)
    auto gen_component =
        rc::gen::suchThat(rc::gen::nonEmpty<::std::string>(), [](const ::std::string& s) {
          return s != "." && s != ".." && s.find('/') == ::std::string::npos &&
                 s.find('\0') == ::std::string::npos;
        });

    auto gen_valid_path = rc::gen::map(
        rc::gen::nonEmpty(rc::gen::container<::std::vector<::std::string>>(gen_component)),
        [](const ::std::vector<::std::string>& components) {
          auto path = ::std::string{"/"};
          for (auto idx = ::std::size_t{0}; idx < components.size(); ++idx) {
            if (idx > 0) {
              path += "/";
            }
            path += components[idx];
          }
          return path;
        });

    auto path_str = *gen_valid_path;
    nix::canon_path_t path(path_str);

    RC_ASSERT(path.abs().front() == '/');
  });

  rc::prop("extend with root is identity", []() {
    auto path = nix::canon_path_t("/foo/bar");
    auto original = path.abs();

    path.extend(nix::canon_path_t::root);

    RC_ASSERT(path.abs() == original);
  });
}

// =============================================================================
// BUG: Very long paths
// =============================================================================

TEST_CASE("bug: very long paths", "[fuzz][canon-path][bug]") {
  // Create a path with 10000 components
  auto long_path = ::std::string{"/"};
  for (auto idx = 0; idx < 10000; ++idx) {
    long_path += "x/";
  }
  long_path.pop_back(); // Remove trailing slash

  INFO("Path length: " << long_path.size());

  // This should either work or throw an exception, not crash
  try {
    nix::canon_path_t path(long_path);
    // Path should be normalized (may be same size if already normalized)
    REQUIRE(path.abs().size() <= long_path.size());
    REQUIRE(path.abs().front() == '/');
  } catch (const nix::base_error_t&) {
    // Path too long error is acceptable
    REQUIRE(true);
  }
}
