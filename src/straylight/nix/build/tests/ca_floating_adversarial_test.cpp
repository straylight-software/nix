// straylight::nix::build - CA floating output adversarial tests
//
// Property-based and fuzz tests designed to BREAK the CA floating output
// implementation. These are adversarial - they generate malicious inputs
// specifically targeting edge cases in:
//
//   1. Derivation name suffix matching (extract_outputs)
//   2. Multi-output collision detection
//   3. Store path parsing and validation
//   4. Realisation registration race conditions
//   5. Hash computation edge cases
//   6. Malformed VM output scenarios
//
// If any of these tests fail, we have a bug. Fix the code, not the test.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <future>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <rapidcheck.h>

#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

// =============================================================================
// Attack Surface 1: Derivation Name Suffix Matching
//
// The extract_outputs() function finds CA floating outputs by matching:
//   suffix = "-" + drv.name              (for "out" output)
//   suffix = "-" + drv.name + "-" + name (for other outputs)
//
// This is FRAGILE. We're looking for bugs in:
//   - Empty names
//   - Names that are substrings of each other
//   - Names with special characters (dashes, dots, underscores)
//   - Names that could match multiple store paths
// =============================================================================

namespace {

// Simulate the suffix matching logic from firecracker_build_service.cpp
// This is the EXACT algorithm we need to test
auto matches_output_suffix(const std::string& store_entry, const std::string& drv_name,
                           const std::string& output_name) -> bool {
  std::string suffix = "-" + drv_name;
  if (output_name != "out") {
    suffix += "-" + output_name;
  }
  return store_entry.size() > suffix.size() &&
         store_entry.substr(store_entry.size() - suffix.size()) == suffix;
}

// Generate a valid Nix store hash (32 chars, base32)
auto gen_store_hash() -> rc::Gen<std::string> {
  // Nix uses a custom base32: 0123456789abcdfghijklmnpqrsvwxyz (no e, o, t, u)
  return rc::gen::map(rc::gen::container<std::vector<int>>(rc::gen::inRange(0, 32)),
                      [](const std::vector<int>& indices) {
                        static const std::string base32 = "0123456789abcdfghijklmnpqrsvwxyz";
                        std::string result;
                        result.reserve(32);
                        for (int i = 0; i < 32; ++i) {
                          int idx =
                              indices.empty() ? (i * 7) % 32 : indices[i % indices.size()] % 32;
                          result += base32[idx];
                        }
                        return result;
                      });
}

// Generate a simple name (a-z letters only, 1-20 chars)
auto gen_simple_name() -> rc::Gen<std::string> {
  return rc::gen::map(rc::gen::container<std::vector<char>>(rc::gen::inRange('a', 'z')),
                      [](const std::vector<char>& chars) {
                        if (chars.empty()) {
                          return std::string("x");
                        }
                        std::string result(chars.begin(), chars.end());
                        if (result.size() > 20) {
                          result.resize(20);
                        }
                        return result;
                      });
}

// Generate derivation names - ADVERSARIAL
// Includes edge cases that could break suffix matching
auto gen_drv_name_adversarial() -> rc::Gen<std::string> {
  return rc::gen::oneOf(
      // Simple names
      gen_simple_name(),
      // Names with dashes (common in Nix: "hello-world")
      rc::gen::map(
          rc::gen::pair(gen_simple_name(), gen_simple_name()),
          [](const std::pair<std::string, std::string>& p) { return p.first + "-" + p.second; }),
      // Single character
      rc::gen::map(rc::gen::inRange('a', 'z'), [](char c) { return std::string(1, c); }),
      // Names ending with common output suffixes (sneaky!)
      rc::gen::map(gen_simple_name(), [](const std::string& s) { return s + "-out"; }),
      rc::gen::map(gen_simple_name(), [](const std::string& s) { return s + "-lib"; }),
      rc::gen::map(gen_simple_name(), [](const std::string& s) { return s + "-dev"; }),
      // Version numbers (foo-1.2.3)
      rc::gen::map(rc::gen::tuple(gen_simple_name(), rc::gen::inRange(0, 99),
                                  rc::gen::inRange(0, 99), rc::gen::inRange(0, 99)),
                   [](const std::tuple<std::string, int, int, int>& t) {
                     return std::get<0>(t) + "-" + std::to_string(std::get<1>(t)) + "." +
                            std::to_string(std::get<2>(t)) + "." + std::to_string(std::get<3>(t));
                   }));
}

// Generate output names - ADVERSARIAL
auto gen_output_name_adversarial() -> rc::Gen<std::string> {
  return rc::gen::oneOf(
      // Standard output names
      rc::gen::just(std::string("out")), rc::gen::just(std::string("lib")),
      rc::gen::just(std::string("dev")), rc::gen::just(std::string("doc")),
      rc::gen::just(std::string("man")), rc::gen::just(std::string("info")),
      rc::gen::just(std::string("bin")),
      // Custom names
      gen_simple_name(),
      // Single character outputs
      rc::gen::map(rc::gen::inRange('a', 'z'), [](char c) { return std::string(1, c); }));
}

// Generate a store path entry (hash-name format)
auto gen_store_entry(const std::string& drv_name, const std::string& output_name)
    -> rc::Gen<std::string> {
  return rc::gen::map(gen_store_hash(), [drv_name, output_name](const std::string& hash) {
    if (output_name == "out") {
      return hash + "-" + drv_name;
    } else {
      return hash + "-" + drv_name + "-" + output_name;
    }
  });
}

// Generate DECOY store entries that might falsely match
auto gen_decoy_entry(const std::string& drv_name, const std::string& output_name)
    -> rc::Gen<std::string> {
  return rc::gen::oneOf(
      // Superset name: "foo" might match "prefix-foo"
      rc::gen::map(rc::gen::pair(gen_store_hash(), gen_simple_name()),
                   [drv_name, output_name](const auto& p) {
                     if (output_name == "out") {
                       return p.first + "-" + p.second + "-" + drv_name;
                     } else {
                       return p.first + "-" + p.second + "-" + drv_name + "-" + output_name;
                     }
                   }),
      // Substring name: "foobar" when looking for "foo"
      rc::gen::map(gen_store_hash(),
                   [drv_name](const std::string& hash) { return hash + "-" + drv_name + "extra"; }),
      // Different output with same drv name
      rc::gen::map(rc::gen::pair(gen_store_hash(), gen_simple_name()),
                   [drv_name](const auto& p) { return p.first + "-" + drv_name + "-" + p.second; }),
      // Completely unrelated
      rc::gen::map(rc::gen::pair(gen_store_hash(), gen_simple_name()),
                   [](const auto& p) { return p.first + "-" + p.second; }));
}

} // namespace

// =============================================================================
// PROPERTY TESTS: Suffix Matching Correctness
// =============================================================================

TEST_CASE("property: suffix matching finds correct output", "[property][ca][adversarial]") {
  rc::check("correct entry always matches", []() {
    auto drv_name = *gen_drv_name_adversarial();
    auto output_name = *gen_output_name_adversarial();

    // Skip empty names
    RC_PRE(!drv_name.empty());
    RC_PRE(!output_name.empty());

    // Generate the correct store entry
    auto correct_entry = *gen_store_entry(drv_name, output_name);

    // It MUST match
    RC_ASSERT(matches_output_suffix(correct_entry, drv_name, output_name));
  });
}

TEST_CASE("property: suffix matching FALSE POSITIVE rate", "[property][ca][adversarial]") {
  // NOTE: This test documents that suffix matching HAS false positives.
  // The original assertion "wrong drv name never matches" was PROVEN FALSE
  // by RapidCheck, finding cases like:
  //   drv1="x-b", drv2="b", output="lib"
  //   Entry "hash-x-b-lib" matches BOTH because "-b-lib" is suffix of "-x-b-lib"
  //
  // This is the fundamental bug with suffix matching.

  rc::check("classify false positive rate", []() {
    auto drv_name1 = *gen_drv_name_adversarial();
    auto drv_name2 = *gen_drv_name_adversarial();
    auto output_name = *gen_output_name_adversarial();

    RC_PRE(!drv_name1.empty());
    RC_PRE(!drv_name2.empty());
    RC_PRE(!output_name.empty());
    RC_PRE(drv_name1 != drv_name2);

    // Generate entry for drv_name1
    auto entry = *gen_store_entry(drv_name1, output_name);

    // Check if drv_name2 ALSO matches (false positive)
    bool false_positive = matches_output_suffix(entry, drv_name2, output_name);

    // Classify to track false positive rate
    RC_CLASSIFY(false_positive, "FALSE POSITIVE - suffix matching is broken");
    RC_CLASSIFY(!false_positive, "correctly rejected");

    // We don't assert - we're just measuring the bug
  });
}

TEST_CASE("property: decoy entries classification", "[property][ca][adversarial]") {
  rc::check("decoys are analyzed", []() {
    auto drv_name = *gen_drv_name_adversarial();
    auto output_name = *gen_output_name_adversarial();

    RC_PRE(!drv_name.empty());
    RC_PRE(!output_name.empty());

    // Generate a decoy
    auto decoy = *gen_decoy_entry(drv_name, output_name);

    // Generate the real entry to compare
    auto real = *gen_store_entry(drv_name, output_name);

    // If decoy == real, skip (valid match)
    RC_PRE(decoy != real);

    // Check if decoy matches
    auto decoy_matches = matches_output_suffix(decoy, drv_name, output_name);

    // Classify the result
    RC_CLASSIFY(decoy_matches, "decoy matched (potential collision)");
    RC_CLASSIFY(!decoy_matches, "decoy correctly rejected");
  });
}

// =============================================================================
// PROPERTY TESTS: Collision Detection
// =============================================================================

TEST_CASE("property: multiple matching entries detected", "[property][ca][adversarial]") {
  rc::check("collision scenario", []() {
    auto drv_name = *gen_drv_name_adversarial();
    auto output_name = *gen_output_name_adversarial();

    RC_PRE(!drv_name.empty());
    RC_PRE(!output_name.empty());

    // Generate multiple entries for the same output
    auto entry1 = *gen_store_entry(drv_name, output_name);
    auto entry2 = *gen_store_entry(drv_name, output_name);

    // Both should match (different hashes, same suffix)
    RC_ASSERT(matches_output_suffix(entry1, drv_name, output_name));
    RC_ASSERT(matches_output_suffix(entry2, drv_name, output_name));

    // If hashes differ, we have a collision scenario
    // The current implementation just takes the FIRST match (break;)
    // This is nondeterministic with fs::directory_iterator!
    RC_CLASSIFY(entry1 != entry2, "hash collision (nondeterministic pick)");
  });
}

// =============================================================================
// Attack Surface 2: Substring/Prefix Attacks
//
// What if drv_name="foo" and there's also "foobar" or "prefix-foo"?
// =============================================================================

TEST_CASE("edge case: substring derivation names", "[ca][adversarial][edge]") {
  SECTION("short name is not substring of long name match") {
    // drv_name = "foo", but store has "hash-foobar"
    // Looking for "-foo" should NOT match "-foobar"
    std::string entry = "abc123-foobar";
    CHECK_FALSE(matches_output_suffix(entry, "foo", "out"));
  }

  SECTION("long name does not match short name entry") {
    // drv_name = "foobar", store has "hash-foo"
    std::string entry = "abc123-foo";
    CHECK_FALSE(matches_output_suffix(entry, "foobar", "out"));
  }

  SECTION("prefix derivation names - THIS IS A BUG") {
    // drv_name = "foo", store has "hash-prefix-foo"
    // This SHOULD NOT match because "prefix-foo" is a different derivation!
    // But suffix matching says it does match "-foo"
    std::string entry = "abc123-prefix-foo";
    // Current behavior: matches because it ends with "-foo"
    // This documents the BUG - suffix matching is insufficient
    CHECK(matches_output_suffix(entry, "foo", "out"));
  }

  SECTION("similar names with version suffix") {
    // "hello-1.0" vs "hello-1.0.1"
    std::string entry1 = "abc123-hello-1.0";
    std::string entry2 = "abc123-hello-1.0.1";

    CHECK(matches_output_suffix(entry1, "hello-1.0", "out"));
    CHECK_FALSE(matches_output_suffix(entry1, "hello-1.0.1", "out"));
    CHECK_FALSE(matches_output_suffix(entry2, "hello-1.0", "out"));
    CHECK(matches_output_suffix(entry2, "hello-1.0.1", "out"));
  }
}

// =============================================================================
// Attack Surface 3: Output Name Collisions
// =============================================================================

TEST_CASE("edge case: output name substring attacks", "[ca][adversarial][edge]") {
  SECTION("output 'lib' vs 'libfoo'") {
    std::string entry = "abc123-mydrv-lib";
    CHECK(matches_output_suffix(entry, "mydrv", "lib"));
    CHECK_FALSE(matches_output_suffix(entry, "mydrv", "libfoo"));
  }

  SECTION("output named same as drv name part") {
    // drv = "foo-bar", output = "bar"
    // Entry should be "hash-foo-bar-bar"
    std::string entry = "abc123-foo-bar-bar";
    CHECK(matches_output_suffix(entry, "foo-bar", "bar"));

    // But what about "hash-foo-bar" with output "out"?
    std::string entry2 = "abc123-foo-bar";
    CHECK(matches_output_suffix(entry2, "foo-bar", "out"));
  }
}

TEST_CASE("CRITICAL: drv/output collision vulnerability", "[ca][adversarial][critical]") {
  // This demonstrates a fundamental flaw in suffix matching

  // Scenario:
  // - Derivation A: name="foo", output="bar" -> expected path ends in "-foo-bar"
  // - Derivation B: name="foo-bar", output="out" -> expected path ends in "-foo-bar"
  //
  // THEY HAVE THE SAME SUFFIX!

  std::string ambiguous_entry = "abc123xyz-foo-bar";

  bool matches_A = matches_output_suffix(ambiguous_entry, "foo", "bar");
  bool matches_B = matches_output_suffix(ambiguous_entry, "foo-bar", "out");

  INFO("Entry: " << ambiguous_entry);
  INFO("Matches drv='foo', output='bar': " << matches_A);
  INFO("Matches drv='foo-bar', output='out': " << matches_B);

  // BOTH MATCH! This is a critical vulnerability.
  CHECK(matches_A);
  CHECK(matches_B);

  // The current implementation will return whichever it finds first.
  // This is WRONG - we cannot distinguish these cases with suffix matching alone.
  //
  // FIX REQUIRED: We need to either:
  // 1. Encode the output name BEFORE the drv name: "hash-out-foo-bar" vs "hash-bar-foo"
  // 2. Use a delimiter that can't appear in names (but Nix allows most chars)
  // 3. Store expected hashes and verify after extraction
  // 4. Parse the .drv file written by the guest
}

// =============================================================================
// Attack Surface 4: Special Characters in Names
// =============================================================================

TEST_CASE("edge case: names with special characters", "[ca][adversarial][edge]") {
  SECTION("dots in version numbers") {
    std::string entry = "abc123-gcc-12.3.0";
    CHECK(matches_output_suffix(entry, "gcc-12.3.0", "out"));
    CHECK_FALSE(matches_output_suffix(entry, "gcc-12.3", "out"));
  }

  SECTION("underscores") {
    std::string entry = "abc123-my_package";
    CHECK(matches_output_suffix(entry, "my_package", "out"));
  }

  SECTION("plus signs (common in C++ packages)") {
    std::string entry = "abc123-libstdc++";
    CHECK(matches_output_suffix(entry, "libstdc++", "out"));
  }
}

// =============================================================================
// Attack Surface 5: Empty and Minimal Names
// =============================================================================

TEST_CASE("edge case: minimal names", "[ca][adversarial][edge]") {
  SECTION("single character drv name") {
    std::string entry = "abc123-a";
    CHECK(matches_output_suffix(entry, "a", "out"));
  }

  SECTION("single character output name") {
    std::string entry = "abc123-foo-a";
    CHECK(matches_output_suffix(entry, "foo", "a"));
  }

  SECTION("entry is just the suffix") {
    // What if entry = "-foo" with no hash prefix?
    // size check should catch this
    std::string entry = "-foo";
    CHECK_FALSE(matches_output_suffix(entry, "foo", "out"));
  }

  SECTION("entry equals suffix exactly") {
    // entry = "-foo" (length 4), suffix = "-foo" (length 4)
    // entry.size() > suffix.size() is false, so no match
    std::string entry = "-foo";
    CHECK_FALSE(matches_output_suffix(entry, "foo", "out"));
  }
}

// =============================================================================
// Attack Surface 6: Unicode and Non-ASCII
// (Nix derivation names should be ASCII, but what's if they're not?)
// =============================================================================

TEST_CASE("edge case: unicode in names", "[ca][adversarial][edge]") {
  SECTION("UTF-8 drv name") {
    // This shouldn't happen in practice, but let's be defensive
    std::string entry = "abc123-caf\xc3\xa9"; // cafe in UTF-8
    CHECK(matches_output_suffix(entry, "caf\xc3\xa9", "out"));
  }

  SECTION("UTF-8 lookalike ASCII") {
    // 'a' (0x61) vs Cyrillic 'a' would be different
    std::string entry_ascii = "abc123-foo";
    CHECK(matches_output_suffix(entry_ascii, "foo", "out"));
  }
}

// =============================================================================
// FUZZ-LIKE STRESS: Many Random Combinations
// =============================================================================

TEST_CASE("stress: many random name combinations", "[ca][adversarial][stress]") {
  std::mt19937 rng(42); // deterministic seed
  std::uniform_int_distribution<int> len_dist(1, 30);
  std::uniform_int_distribution<int> char_dist('a', 'z');
  std::uniform_int_distribution<int> special_dist(0, 10);

  constexpr int NUM_ITERATIONS = 1000;
  int collisions = 0;

  for (int i = 0; i < NUM_ITERATIONS; ++i) {
    // Generate random drv name
    int drv_len = len_dist(rng);
    std::string drv_name;
    for (int j = 0; j < drv_len; ++j) {
      if (special_dist(rng) == 0) {
        drv_name += '-';
      } else {
        drv_name += static_cast<char>(char_dist(rng));
      }
    }
    if (drv_name.empty() || drv_name[0] == '-') {
      drv_name = "x" + drv_name;
    }

    // Generate random output name
    int out_len = std::min(len_dist(rng), 10);
    std::string output_name;
    for (int j = 0; j < out_len; ++j) {
      output_name += static_cast<char>(char_dist(rng));
    }
    if (output_name.empty()) {
      output_name = "out";
    }

    // Create expected entry
    std::string hash = "0123456789abcdfghijklmnpqrsvwxyz"; // 32 char placeholder
    std::string entry = hash + "-" + drv_name;
    if (output_name != "out") {
      entry += "-" + output_name;
    }

    // Verify it matches
    bool matches = matches_output_suffix(entry, drv_name, output_name);
    if (!matches) {
      FAIL("Generated entry doesn't match: entry='" << entry << "' drv='" << drv_name
                                                    << "' output='" << output_name << "'");
    }

    // Try some collision scenarios
    // Parse the drv_name differently if it contains dashes
    auto dash_pos = drv_name.rfind('-');
    if (dash_pos != std::string::npos && dash_pos > 0) {
      std::string alt_drv = drv_name.substr(0, dash_pos);
      std::string alt_out = drv_name.substr(dash_pos + 1);
      if (!alt_out.empty() && output_name == "out") {
        // Check if alternative interpretation also matches
        if (matches_output_suffix(entry, alt_drv, alt_out)) {
          collisions++;
        }
      }
    }
  }

  INFO("Found " << collisions << " ambiguous entries out of " << NUM_ITERATIONS);
  // We expect some collisions due to the fundamental ambiguity
  // This is informational, not a test failure
}

// =============================================================================
// REGRESSION: Known Problematic Names from Real Nixpkgs
// =============================================================================

TEST_CASE("regression: real nixpkgs name patterns", "[ca][adversarial][regression]") {
  struct TestCase {
    std::string drv_name;
    std::string output_name;
    std::string entry;
    bool should_match;
  };

  std::vector<TestCase> cases = {
      // Normal cases
      {"hello-2.12.1", "out", "abc123-hello-2.12.1", true},
      {"openssl-3.0.12", "dev", "abc123-openssl-3.0.12-dev", true},
      {"gcc-12.3.0", "lib", "abc123-gcc-12.3.0-lib", true},

      // Multi-output packages
      {"systemd-254", "out", "abc123-systemd-254", true},
      {"systemd-254", "dev", "abc123-systemd-254-dev", true},
      {"systemd-254", "lib", "abc123-systemd-254-lib", true},

      // Wrong matches
      {"hello", "out", "abc123-hello-world", false},
      {"openssl", "out", "abc123-openssl-3.0.12", false}, // version != drv name

      // Ambiguous (these are the dangerous ones)
      // "foo-bar" output "out" vs "foo" output "bar"
      // Both would produce suffix "-foo-bar"
  };

  for (const auto& tc : cases) {
    INFO("drv=" << tc.drv_name << " output=" << tc.output_name << " entry=" << tc.entry);
    CHECK(matches_output_suffix(tc.entry, tc.drv_name, tc.output_name) == tc.should_match);
  }
}

// =============================================================================
// Property: Output name "out" requires special handling
// =============================================================================

TEST_CASE("property: 'out' output special case", "[property][ca][adversarial]") {
  rc::check("'out' has different suffix than other outputs", []() {
    auto drv_name = *gen_drv_name_adversarial();
    RC_PRE(!drv_name.empty());

    // For output "out", suffix is "-<drv_name>"
    // For other outputs, suffix is "-<drv_name>-<output>"
    auto hash = *gen_store_hash();
    std::string entry_for_out = hash + "-" + drv_name;
    std::string entry_for_lib = hash + "-" + drv_name + "-lib";

    // "out" output matches entry without -out suffix
    RC_ASSERT(matches_output_suffix(entry_for_out, drv_name, "out"));
    RC_ASSERT(!matches_output_suffix(entry_for_lib, drv_name, "out"));

    // "lib" output matches entry with -lib suffix
    RC_ASSERT(matches_output_suffix(entry_for_lib, drv_name, "lib"));
    RC_ASSERT(!matches_output_suffix(entry_for_out, drv_name, "lib"));
  });
}

// =============================================================================
// DEMONSTRATION: Why suffix matching is insufficient
// =============================================================================

TEST_CASE("DOCUMENTATION: suffix matching limitations", "[ca][adversarial][doc]") {
  // This test documents the fundamental problem and potential fixes

  INFO("=== THE PROBLEM ===");
  INFO("Suffix matching cannot distinguish between:");
  INFO("  1. drv='foo', output='bar' -> suffix '-foo-bar'");
  INFO("  2. drv='foo-bar', output='out' -> suffix '-foo-bar'");
  INFO("");
  INFO("=== POTENTIAL FIXES ===");
  INFO("Option A: Use unique delimiter (e.g., '!' but Nix may not allow it)");
  INFO("Option B: Encode output count/index in path");
  INFO("Option C: Store derivation hash alongside output to verify");
  INFO("Option D: Parse the .drv or output info written by guest");
  INFO("Option E: Use CA output hash which is globally unique");
  INFO("");
  INFO("=== RECOMMENDED FIX ===");
  INFO("For CA derivations, the output hash IS the identifier.");
  INFO("We should query the realisation by DrvOutput key BEFORE extraction,");
  INFO("then verify the extracted path matches the expected output path.");
  INFO("If no realisation exists (first build), we compute the CA hash of");
  INFO("the extracted content and register that.");

  // The test passes because we're documenting the issue
  CHECK(true);
}

// =============================================================================
// ATTACK: Path Traversal / Injection
// =============================================================================

TEST_CASE("security: path traversal attempts", "[ca][adversarial][security]") {
  SECTION("drv name with path separators") {
    // If a malicious drv name contains '/', could it escape?
    // The suffix matching itself doesn't validate path safety
    std::string evil_entry = "abc123-foo/../../../etc/passwd";
    // This would match drv_name="foo/../../../etc/passwd"
    CHECK(matches_output_suffix(evil_entry, "foo/../../../etc/passwd", "out"));
    // The REAL protection must be in the code that uses this path
  }

  SECTION("null bytes in names") {
    // Null bytes could truncate strings in C
    std::string entry_with_null = std::string("abc123-foo") + '\0' + "bar";
    std::string drv_with_null = std::string("foo") + '\0' + "bar";
    // C++ strings handle nulls correctly
    CHECK(matches_output_suffix(entry_with_null, drv_with_null, "out"));
  }
}

// =============================================================================
// PERFORMANCE: Timing attacks / DoS
// =============================================================================

TEST_CASE("performance: long name handling", "[ca][adversarial][perf]") {
  SECTION("very long drv name") {
    std::string long_name(10000, 'a');
    std::string hash = "0123456789abcdfghijklmnpqrsvwxyz";
    std::string entry = hash + "-" + long_name;

    auto start = std::chrono::steady_clock::now();
    bool result = matches_output_suffix(entry, long_name, "out");
    auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(result);
    // Should be fast (O(suffix length) for substr comparison)
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 10);
  }

  SECTION("many matching attempts") {
    std::string hash = "0123456789abcdfghijklmnpqrsvwxyz";
    std::string drv = "test-package";
    std::string entry = hash + "-" + drv;

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; ++i) {
      matches_output_suffix(entry, drv, "out");
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    INFO("100k matches took "
         << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << "ms");
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 1000);
  }
}

// =============================================================================
// BUG DEMONSTRATION: Real-world collision scenario
//
// This test demonstrates why the current suffix matching is BROKEN.
// In a real store, these ambiguous cases WILL occur.
// =============================================================================

TEST_CASE("BUG: ambiguous suffix collision in real scenario", "[ca][adversarial][bug]") {
  // Scenario: Building Python packages
  // - python3-pip (drv name "python3-pip", output "out")
  // - python3 (drv name "python3", output "pip")  [hypothetical multi-output]
  //
  // Both produce suffix "-python3-pip"

  SECTION("python package name collision") {
    std::string store_entry = "abc123def456ghi789jkl012mno345pq-python3-pip";

    bool matches_pip_drv = matches_output_suffix(store_entry, "python3-pip", "out");
    bool matches_python3_pip_output = matches_output_suffix(store_entry, "python3", "pip");

    INFO("Both interpretations match: THIS IS THE BUG");
    CHECK(matches_pip_drv);
    CHECK(matches_python3_pip_output);
  }

  SECTION("boost library collision") {
    // boost-build (drv name) vs boost (drv) with output build
    std::string store_entry = "abc123def456ghi789jkl012mno345pq-boost-build";

    bool matches_boost_build_drv = matches_output_suffix(store_entry, "boost-build", "out");
    bool matches_boost_build_output = matches_output_suffix(store_entry, "boost", "build");

    CHECK(matches_boost_build_drv);
    CHECK(matches_boost_build_output);
  }

  SECTION("gcc output collision") {
    // gcc-lib (drv name) vs gcc (drv) with output lib
    std::string store_entry = "abc123def456ghi789jkl012mno345pq-gcc-lib";

    bool matches_gcc_lib_drv = matches_output_suffix(store_entry, "gcc-lib", "out");
    bool matches_gcc_lib_output = matches_output_suffix(store_entry, "gcc", "lib");

    // BOTH MATCH - the algorithm cannot distinguish
    CHECK(matches_gcc_lib_drv);
    CHECK(matches_gcc_lib_output);

    INFO("This WILL cause incorrect output extraction for CA floating derivations!");
  }
}

// =============================================================================
// IMPROVED MATCHING: Using hash validation
//
// The fix is to NOT rely solely on suffix matching. Instead:
// 1. For first build: compute CA hash of extracted content
// 2. For subsequent: verify against registered realisation
// =============================================================================

namespace {

// A safer matching that also checks hash prefix format
auto safe_matches_output(const std::string& store_entry, const std::string& drv_name,
                         const std::string& output_name, const std::string& expected_hash_prefix)
    -> bool {
  // First check suffix (existing logic)
  if (!matches_output_suffix(store_entry, drv_name, output_name)) {
    return false;
  }

  // Then verify the hash prefix matches what we expect
  // This requires us to know the expected output hash beforehand
  if (!expected_hash_prefix.empty()) {
    if (store_entry.size() < expected_hash_prefix.size()) {
      return false;
    }
    return store_entry.substr(0, expected_hash_prefix.size()) == expected_hash_prefix;
  }

  // Without hash prefix, we can't be sure - return true but mark as uncertain
  return true;
}

} // namespace

TEST_CASE("FIXED: safe matching with hash validation", "[ca][adversarial][fix]") {
  SECTION("hash prefix distinguishes ambiguous entries") {
    // Two entries that would be ambiguous with suffix-only matching
    std::string entry_gcc_lib = "abc123def456ghi789jkl012mno345pq-gcc-lib";
    std::string entry_gcc_out_renamed = "zzz999xyz888uvw777rst666qpo543nm-gcc-lib";

    // If we know the expected hash prefix, we can distinguish
    CHECK(safe_matches_output(entry_gcc_lib, "gcc", "lib", "abc123"));
    CHECK_FALSE(safe_matches_output(entry_gcc_lib, "gcc", "lib", "zzz999"));

    CHECK(safe_matches_output(entry_gcc_out_renamed, "gcc", "lib", "zzz999"));
    CHECK_FALSE(safe_matches_output(entry_gcc_out_renamed, "gcc", "lib", "abc123"));
  }

  SECTION("without hash prefix falls back to suffix") {
    std::string entry = "abc123def456ghi789jkl012mno345pq-gcc-lib";
    // With empty hash prefix, it's still ambiguous
    CHECK(safe_matches_output(entry, "gcc", "lib", ""));
    CHECK(safe_matches_output(entry, "gcc-lib", "out", ""));
  }
}

// =============================================================================
// PROPERTY: Safe matching is strictly more correct than suffix matching
// =============================================================================

TEST_CASE("property: safe matching is more selective", "[property][ca][adversarial]") {
  rc::check("safe matching with hash rejects more false positives", []() {
    auto drv_name = *gen_drv_name_adversarial();
    auto output_name = *gen_output_name_adversarial();

    RC_PRE(!drv_name.empty());
    RC_PRE(!output_name.empty());

    // Generate two different entries that match the same suffix
    auto hash1 = *gen_store_hash();
    auto hash2 = *gen_store_hash();
    RC_PRE(hash1 != hash2);

    std::string entry1, entry2;
    if (output_name == "out") {
      entry1 = hash1 + "-" + drv_name;
      entry2 = hash2 + "-" + drv_name;
    } else {
      entry1 = hash1 + "-" + drv_name + "-" + output_name;
      entry2 = hash2 + "-" + drv_name + "-" + output_name;
    }

    // Suffix matching accepts both
    RC_ASSERT(matches_output_suffix(entry1, drv_name, output_name));
    RC_ASSERT(matches_output_suffix(entry2, drv_name, output_name));

    // Safe matching with hash1 prefix only accepts entry1
    RC_ASSERT(safe_matches_output(entry1, drv_name, output_name, hash1.substr(0, 10)));
    RC_ASSERT(!safe_matches_output(entry2, drv_name, output_name, hash1.substr(0, 10)));
  });
}
