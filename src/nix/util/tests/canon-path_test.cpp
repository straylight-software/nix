// straylight // nix // util // tests
//
// Unit tests for canonical path handling

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

// RapidCheck for property-based testing
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "nix/util/canon-path.h"

using namespace nix;

// ─────────────────────────────────────────────────────────────────────────────
// Construction tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("canon path construction from root", "[canon-path][construction]") {
  canon_path_t path("/");
  REQUIRE(path.isRoot());
  REQUIRE(path.abs() == "/");
  REQUIRE(path.rel() == "");
}

TEST_CASE("canon path construction from simple path", "[canon-path][construction]") {
  canon_path_t path("/foo/bar");
  REQUIRE_FALSE(path.isRoot());
  REQUIRE(path.abs() == "/foo/bar");
  REQUIRE(path.rel() == "foo/bar");
}

TEST_CASE("canon path construction normalizes relative input", "[canon-path][construction]") {
  // Constructor treats input without leading slash as relative
  canon_path_t path("foo/bar");
  REQUIRE(path.abs() == "/foo/bar");
}

TEST_CASE("canon path construction removes trailing slashes", "[canon-path][construction]") {
  canon_path_t path("/foo/bar/");
  REQUIRE(path.abs() == "/foo/bar");

  canon_path_t path_multiple("/foo/bar///");
  REQUIRE(path_multiple.abs() == "/foo/bar");
}

TEST_CASE("canon path construction removes empty components", "[canon-path][construction]") {
  canon_path_t path("/foo//bar");
  REQUIRE(path.abs() == "/foo/bar");

  canon_path_t path_many("/foo///bar////baz");
  REQUIRE(path_many.abs() == "/foo/bar/baz");
}

TEST_CASE("canon path construction removes dot components", "[canon-path][construction]") {
  canon_path_t path("/foo/./bar");
  REQUIRE(path.abs() == "/foo/bar");

  canon_path_t path_multiple("/./foo/././bar/.");
  REQUIRE(path_multiple.abs() == "/foo/bar");
}

TEST_CASE("canon path construction resolves dot-dot components", "[canon-path][construction]") {
  canon_path_t path("/foo/bar/../baz");
  REQUIRE(path.abs() == "/foo/baz");

  canon_path_t path_multiple("/foo/bar/qux/../../baz");
  REQUIRE(path_multiple.abs() == "/foo/baz");

  canon_path_t path_to_root("/foo/bar/../..");
  REQUIRE(path_to_root.abs() == "/");
  REQUIRE(path_to_root.isRoot());
}

TEST_CASE("canon path construction with root context", "[canon-path][construction]") {
  canon_path_t root("/home/user");

  // Absolute path ignores root context
  canon_path_t absolute_path("/etc/passwd", root);
  REQUIRE(absolute_path.abs() == "/etc/passwd");

  // Relative path is joined with root
  canon_path_t relative_path("documents/file.txt", root);
  REQUIRE(relative_path.abs() == "/home/user/documents/file.txt");

  // Empty relative path returns root
  canon_path_t empty_relative("", root);
  REQUIRE(empty_relative.abs() == "/home/user");
}

TEST_CASE("canon path construction from vector of elements", "[canon-path][construction]") {
  std::vector<std::string> empty_elements;
  canon_path_t empty_path(empty_elements);
  REQUIRE(empty_path.isRoot());
  REQUIRE(empty_path.abs() == "/");

  std::vector<std::string> elements = {"foo", "bar", "baz"};
  canon_path_t path(elements);
  REQUIRE(path.abs() == "/foo/bar/baz");
}

TEST_CASE("canon path construction from char pointer", "[canon-path][construction]") {
  const char* raw = "/foo/bar";
  canon_path_t path(raw);
  REQUIRE(path.abs() == "/foo/bar");
}

// ─────────────────────────────────────────────────────────────────────────────
// Accessor tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("canon path abs and rel accessors", "[canon-path][accessor]") {
  canon_path_t path("/foo/bar/baz");
  REQUIRE(path.abs() == "/foo/bar/baz");
  REQUIRE(path.rel() == "foo/bar/baz");
  REQUIRE(std::string(path.c_str()) == "/foo/bar/baz");
  REQUIRE(std::string(path.rel_c_str()) == "foo/bar/baz");
}

TEST_CASE("canon path abs_or_empty for root", "[canon-path][accessor]") {
  canon_path_t root("/");
  REQUIRE(root.absOrEmpty() == "");

  canon_path_t non_root("/foo");
  REQUIRE(non_root.absOrEmpty() == "/foo");
}

TEST_CASE("canon path string view conversion", "[canon-path][accessor]") {
  canon_path_t path("/foo/bar");
  auto view = static_cast<std::string_view>(path);
  REQUIRE(view == "/foo/bar");
}

TEST_CASE("canon path basename", "[canon-path][accessor]") {
  REQUIRE_FALSE(canon_path_t("/").baseName().has_value());

  REQUIRE(canon_path_t("/foo").baseName().value() == "foo");
  REQUIRE(canon_path_t("/foo/bar").baseName().value() == "bar");
  REQUIRE(canon_path_t("/foo/bar/baz.txt").baseName().value() == "baz.txt");
}

TEST_CASE("canon path dirname", "[canon-path][accessor]") {
  REQUIRE_FALSE(canon_path_t("/").dirOf().has_value());

  // dirOf returns the parent directory path as string_view
  REQUIRE(canon_path_t("/foo").dirOf().value() == "");
  REQUIRE(canon_path_t("/foo/bar").dirOf().value() == "/foo");
  REQUIRE(canon_path_t("/foo/bar/baz").dirOf().value() == "/foo/bar");
}

// ─────────────────────────────────────────────────────────────────────────────
// Parent and hierarchy tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("canon path parent", "[canon-path][hierarchy]") {
  REQUIRE_FALSE(canon_path_t("/").parent().has_value());

  auto parent = canon_path_t("/foo").parent();
  REQUIRE(parent.has_value());
  REQUIRE(parent->isRoot());

  parent = canon_path_t("/foo/bar").parent();
  REQUIRE(parent.has_value());
  REQUIRE(parent->abs() == "/foo");

  parent = canon_path_t("/foo/bar/baz").parent();
  REQUIRE(parent.has_value());
  REQUIRE(parent->abs() == "/foo/bar");
}

TEST_CASE("canon path pop", "[canon-path][hierarchy]") {
  canon_path_t path("/foo/bar/baz");

  path.pop();
  REQUIRE(path.abs() == "/foo/bar");

  path.pop();
  REQUIRE(path.abs() == "/foo");

  path.pop();
  REQUIRE(path.isRoot());
}

TEST_CASE("canon path is_within", "[canon-path][hierarchy]") {
  canon_path_t root("/");
  canon_path_t foo("/foo");
  canon_path_t foo_bar("/foo/bar");
  canon_path_t foo_baz("/foo/baz");

  // Every path is within root
  REQUIRE(root.isWithin(root));
  REQUIRE(foo.isWithin(root));
  REQUIRE(foo_bar.isWithin(root));

  // Child is within parent
  REQUIRE(foo_bar.isWithin(foo));
  REQUIRE_FALSE(foo.isWithin(foo_bar));

  // Path is within itself
  REQUIRE(foo.isWithin(foo));
  REQUIRE(foo_bar.isWithin(foo_bar));

  // Sibling paths are not within each other
  REQUIRE_FALSE(foo_bar.isWithin(foo_baz));
  REQUIRE_FALSE(foo_baz.isWithin(foo_bar));
}

TEST_CASE("canon path is_within handles prefix correctly", "[canon-path][hierarchy]") {
  // /foobar should NOT be within /foo (just because it starts with "/foo")
  canon_path_t foo("/foo");
  canon_path_t foobar("/foobar");

  REQUIRE_FALSE(foobar.isWithin(foo));
}

// ─────────────────────────────────────────────────────────────────────────────
// Modification tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("canon path push component", "[canon-path][modification]") {
  canon_path_t path("/foo");
  path.push("bar");
  REQUIRE(path.abs() == "/foo/bar");

  path.push("baz");
  REQUIRE(path.abs() == "/foo/bar/baz");
}

TEST_CASE("canon path push to root", "[canon-path][modification]") {
  canon_path_t path("/");
  path.push("foo");
  REQUIRE(path.abs() == "/foo");
}

TEST_CASE("canon path extend with another path", "[canon-path][modification]") {
  canon_path_t base("/foo");
  canon_path_t extension("/bar/baz");

  base.extend(extension);
  REQUIRE(base.abs() == "/foo/bar/baz");
}

TEST_CASE("canon path extend with root is no-op", "[canon-path][modification]") {
  canon_path_t path("/foo/bar");
  path.extend(canon_path_t::root);
  REQUIRE(path.abs() == "/foo/bar");
}

TEST_CASE("canon path extend from root", "[canon-path][modification]") {
  canon_path_t path("/");
  path.extend(canon_path_t("/foo/bar"));
  REQUIRE(path.abs() == "/foo/bar");
}

TEST_CASE("canon path operator slash with path", "[canon-path][modification]") {
  canon_path_t base("/foo");
  canon_path_t extension("/bar/baz");
  canon_path_t result = base / extension;

  REQUIRE(result.abs() == "/foo/bar/baz");
  // Original should be unchanged
  REQUIRE(base.abs() == "/foo");
}

TEST_CASE("canon path operator slash with component", "[canon-path][modification]") {
  canon_path_t base("/foo");
  canon_path_t result = base / "bar";

  REQUIRE(result.abs() == "/foo/bar");
  // Original should be unchanged
  REQUIRE(base.abs() == "/foo");
}

TEST_CASE("canon path remove_prefix", "[canon-path][modification]") {
  canon_path_t path("/foo/bar/baz");
  canon_path_t prefix("/foo");

  canon_path_t result = path.removePrefix(prefix);
  REQUIRE(result.abs() == "/bar/baz");
}

TEST_CASE("canon path remove_prefix with root prefix", "[canon-path][modification]") {
  canon_path_t path("/foo/bar");
  canon_path_t result = path.removePrefix(canon_path_t::root);
  REQUIRE(result.abs() == "/foo/bar");
}

TEST_CASE("canon path remove_prefix same path", "[canon-path][modification]") {
  canon_path_t path("/foo/bar");
  canon_path_t result = path.removePrefix(path);
  REQUIRE(result.isRoot());
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparison tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("canon path equality", "[canon-path][comparison]") {
  REQUIRE(canon_path_t("/foo/bar") == canon_path_t("/foo/bar"));
  REQUIRE(canon_path_t("/") == canon_path_t("/"));
  REQUIRE_FALSE(canon_path_t("/foo") == canon_path_t("/bar"));
}

TEST_CASE("canon path inequality", "[canon-path][comparison]") {
  REQUIRE(canon_path_t("/foo") != canon_path_t("/bar"));
  REQUIRE_FALSE(canon_path_t("/foo") != canon_path_t("/foo"));
}

TEST_CASE("canon path ordering puts parent before children", "[canon-path][comparison]") {
  canon_path_t foo("/foo");
  canon_path_t foo_bar("/foo/bar");
  canon_path_t foo_baz("/foo/baz");
  canon_path_t foobar("/foobar");

  // Parent comes before child
  REQUIRE(foo < foo_bar);
  REQUIRE(foo < foo_baz);

  // Siblings are ordered lexicographically
  REQUIRE(foo_bar < foo_baz);

  // Path separator sorts before other characters
  // So /foo < /foo/bar < /foobar (because '/' < 'b')
  REQUIRE(foo < foo_bar);
  REQUIRE(foo_bar < foobar);
}

TEST_CASE("canon path ordering special case with exclamation", "[canon-path][comparison]") {
  // The header specifically mentions: foo < foo/bar < foo!
  // This is because '/' is treated as 0 in the comparison
  canon_path_t foo("/foo");
  canon_path_t foo_bar("/foo/bar");
  canon_path_t foo_exclaim("/foo!");

  REQUIRE(foo < foo_bar);
  REQUIRE(foo_bar < foo_exclaim);
}

// ─────────────────────────────────────────────────────────────────────────────
// Iterator tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("canon path iterator over root", "[canon-path][iterator]") {
  canon_path_t root("/");
  std::vector<std::string> components;
  for (auto component : root) {
    components.emplace_back(component);
  }
  // Root has no components to iterate
  REQUIRE(components.empty());
}

TEST_CASE("canon path iterator over simple path", "[canon-path][iterator]") {
  canon_path_t path("/foo/bar/baz");
  std::vector<std::string> components;
  for (auto component : path) {
    components.emplace_back(component);
  }

  REQUIRE(components.size() == 3);
  REQUIRE(components[0] == "foo");
  REQUIRE(components[1] == "bar");
  REQUIRE(components[2] == "baz");
}

TEST_CASE("canon path iterator single component", "[canon-path][iterator]") {
  canon_path_t path("/foo");
  std::vector<std::string> components;
  for (auto component : path) {
    components.emplace_back(component);
  }

  REQUIRE(components.size() == 1);
  REQUIRE(components[0] == "foo");
}

TEST_CASE("canon path iterator with range algorithms", "[canon-path][iterator]") {
  canon_path_t path("/a/b/c/d");

  auto it = path.begin();
  REQUIRE(*it == "a");
  ++it;
  REQUIRE(*it == "b");

  // Test post-increment
  auto old_it = it++;
  REQUIRE(*old_it == "b");
  REQUIRE(*it == "c");
}

// ─────────────────────────────────────────────────────────────────────────────
// make_relative tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("canon path make_relative same path", "[canon-path][relative]") {
  canon_path_t path("/foo/bar");
  REQUIRE(path.makeRelative(path) == ".");
}

TEST_CASE("canon path make_relative child", "[canon-path][relative]") {
  canon_path_t parent("/foo");
  canon_path_t child("/foo/bar/baz");
  REQUIRE(parent.makeRelative(child) == "bar/baz");
}

TEST_CASE("canon path make_relative parent", "[canon-path][relative]") {
  canon_path_t child("/foo/bar/baz");
  canon_path_t parent("/foo");
  REQUIRE(child.makeRelative(parent) == "../..");
}

TEST_CASE("canon path make_relative sibling", "[canon-path][relative]") {
  canon_path_t path1("/foo/bar");
  canon_path_t path2("/foo/baz");
  REQUIRE(path1.makeRelative(path2) == "../baz");
}

TEST_CASE("canon path make_relative from root", "[canon-path][relative]") {
  canon_path_t root("/");
  canon_path_t path("/foo/bar");
  REQUIRE(root.makeRelative(path) == "foo/bar");
}

TEST_CASE("canon path make_relative to root", "[canon-path][relative]") {
  canon_path_t path("/foo/bar");
  canon_path_t root("/");
  REQUIRE(path.makeRelative(root) == "../..");
}

// ─────────────────────────────────────────────────────────────────────────────
// is_allowed tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("canon path is_allowed with empty set", "[canon-path][allowed]") {
  canon_path_t path("/foo/bar");
  std::set<canon_path_t> allowed;
  REQUIRE_FALSE(path.isAllowed(allowed));
}

TEST_CASE("canon path is_allowed exact match", "[canon-path][allowed]") {
  canon_path_t path("/foo/bar");
  std::set<canon_path_t> allowed = {canon_path_t("/foo/bar")};
  REQUIRE(path.isAllowed(allowed));
}

TEST_CASE("canon path is_allowed parent of allowed", "[canon-path][allowed]") {
  // A parent path is allowed if any of its children are in the allowed set
  canon_path_t parent("/foo");
  std::set<canon_path_t> allowed = {canon_path_t("/foo/bar")};
  REQUIRE(parent.isAllowed(allowed));
}

TEST_CASE("canon path is_allowed child of allowed", "[canon-path][allowed]") {
  // A child path is allowed if any of its parents are in the allowed set
  canon_path_t child("/foo/bar/baz");
  std::set<canon_path_t> allowed = {canon_path_t("/foo")};
  REQUIRE(child.isAllowed(allowed));
}

TEST_CASE("canon path is_allowed unrelated path", "[canon-path][allowed]") {
  canon_path_t path("/foo/bar");
  std::set<canon_path_t> allowed = {canon_path_t("/baz")};
  REQUIRE_FALSE(path.isAllowed(allowed));
}

// ─────────────────────────────────────────────────────────────────────────────
// Output stream tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("canon path stream output", "[canon-path][output]") {
  canon_path_t path("/foo/bar");
  std::ostringstream stream;
  stream << path;
  REQUIRE(stream.str() == "/foo/bar");
}

// ─────────────────────────────────────────────────────────────────────────────
// Hashing tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("canon path hash consistency", "[canon-path][hash]") {
  canon_path_t path1("/foo/bar");
  canon_path_t path2("/foo/bar");
  canon_path_t path3("/foo/baz");

  REQUIRE(std::hash<canon_path_t>{}(path1) == std::hash<canon_path_t>{}(path2));
  // Different paths should (almost certainly) have different hashes
  REQUIRE(std::hash<canon_path_t>{}(path1) != std::hash<canon_path_t>{}(path3));
}

TEST_CASE("canon path boost hash", "[canon-path][hash]") {
  canon_path_t path1("/foo/bar");
  canon_path_t path2("/foo/bar");

  REQUIRE(hash_value(path1) == hash_value(path2));
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge case tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("canon path with special characters", "[canon-path][edge-case]") {
  canon_path_t path("/foo/bar with spaces/baz");
  REQUIRE(path.abs() == "/foo/bar with spaces/baz");

  canon_path_t path_unicode("/foo/bar-\xC3\xA9/baz");
  REQUIRE(path_unicode.baseName().value() == "baz");
}

TEST_CASE("canon path deeply nested", "[canon-path][edge-case]") {
  std::string deep_path = "/a";
  for (int i = 0; i < 100; i++) {
    deep_path += "/b";
  }
  canon_path_t path(deep_path);

  // Count components
  int count = 0;
  for ([[maybe_unused]] auto component : path) {
    count++;
  }
  REQUIRE(count == 101); // 'a' + 100 'b's

  // Parent should work
  auto parent = path.parent();
  REQUIRE(parent.has_value());
}

TEST_CASE("canon path dot-dot cannot escape root", "[canon-path][edge-case]") {
  // Attempting to go above root should stay at root
  canon_path_t path("/../../../foo");
  REQUIRE(path.abs() == "/foo");

  canon_path_t path2("/foo/../../..");
  REQUIRE(path2.isRoot());
}

TEST_CASE("canon path static root constant", "[canon-path][edge-case]") {
  REQUIRE(canon_path_t::root.isRoot());
  REQUIRE(canon_path_t::root.abs() == "/");
  REQUIRE(canon_path_t::root == canon_path_t("/"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests with RapidCheck
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Generator for valid path component names (no slashes, dots, or null bytes)
rc::Gen<std::string> generate_valid_component() {
  return rc::gen::suchThat(rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::oneOf(
                               rc::gen::inRange('a', 'z'), rc::gen::inRange('0', '9'),
                               rc::gen::just('-'), rc::gen::just('_')))),
                           [](const std::string& s) { return s != "." && s != ".."; });
}

// Generator for a vector of path components
rc::Gen<std::vector<std::string>> generate_path_components() {
  return rc::gen::container<std::vector<std::string>>(generate_valid_component());
}

} // namespace

TEST_CASE("canon path property tests", "[canon-path][property]") {
  rc::prop("path always starts with slash", []() {
    auto components = *generate_path_components();
    canon_path_t path(components);
    RC_ASSERT(path.abs()[0] == '/');
  });

  rc::prop("path never ends with slash except root", []() {
    auto components = *generate_path_components();
    canon_path_t path(components);
    if (!path.isRoot()) {
      RC_ASSERT(path.abs().back() != '/');
    }
  });

  rc::prop("parent of non-root path exists", []() {
    auto components = *rc::gen::nonEmpty(generate_path_components());
    canon_path_t path(components);
    auto parent = path.parent();
    RC_ASSERT(parent.has_value());
  });

  rc::prop("path is within its parent", []() {
    auto components = *rc::gen::nonEmpty(generate_path_components());
    canon_path_t path(components);
    auto parent = path.parent();
    RC_ASSERT(path.isWithin(*parent));
  });

  rc::prop("extending with a path then removing prefix yields original extension", []() {
    auto base_components = *generate_path_components();
    auto ext_components = *generate_path_components();

    canon_path_t base(base_components);
    canon_path_t extension(ext_components);

    canon_path_t combined = base / extension;
    canon_path_t restored = combined.removePrefix(base);
    RC_ASSERT(restored == extension);
  });

  rc::prop("push and pop are inverse operations", []() {
    auto components = *generate_path_components();
    auto extra = *generate_valid_component();

    canon_path_t path(components);
    canon_path_t original = path;
    path.push(extra);
    path.pop();
    RC_ASSERT(path == original);
  });

  rc::prop("iterator count matches component count", []() {
    auto components = *generate_path_components();
    canon_path_t path(components);

    size_t count = 0;
    for ([[maybe_unused]] auto c : path) {
      count++;
    }
    RC_ASSERT(count == components.size());
  });

  rc::prop("make_relative roundtrip from base to target and back", []() {
    auto base_components = *generate_path_components();
    auto target_components = *generate_path_components();

    canon_path_t base(base_components);
    canon_path_t target(target_components);

    std::string relative = base.makeRelative(target);
    canon_path_t reconstructed(relative, base);
    RC_ASSERT(reconstructed == target);
  });

  rc::prop("hash equality implies path equality", []() {
    auto components1 = *generate_path_components();
    auto components2 = *generate_path_components();

    canon_path_t path1(components1);
    canon_path_t path2(components2);

    if (path1 == path2) {
      RC_ASSERT(std::hash<canon_path_t>{}(path1) == std::hash<canon_path_t>{}(path2));
    }
  });

  rc::prop("ordering is consistent with equality", []() {
    auto components1 = *generate_path_components();
    auto components2 = *generate_path_components();

    canon_path_t path1(components1);
    canon_path_t path2(components2);

    auto cmp = path1 <=> path2;
    if (cmp == 0) {
      RC_ASSERT(path1 == path2);
    } else {
      RC_ASSERT(path1 != path2);
    }
  });
}
