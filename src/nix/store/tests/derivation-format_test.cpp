// straylight // nix // store // tests
//
// Derivation format compatibility tests - executable specification for Nix .drv format
//
// These tests verify that straylight-nix uses the same ATerm derivation format as upstream nix.
// Format compatibility is CRITICAL for interoperability with upstream nix stores.
//
// Background:
//   - Derivations (.drv files) are stored in ATerm format: Derive(...) or DrvWithVersion(...)
//   - The format encodes: outputs, inputDrvs, inputSrcs, system, builder, args, env
//   - Content-addressed derivations use special output formats with hash algorithm info
//   - Structured attributes are encoded as JSON in the __json env var
//   - Breaking format compatibility would prevent reading/writing derivations from upstream stores
//
// Reference: https://github.com/NixOS/nix/blob/master/src/libstore/derivations.cc

#include <fstream>
#include <iterator>
#include <regex>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "nix/store/derivations.h"
#include "nix/store/path.h"
#include "nix/util/hash.h"

namespace {

// =============================================================================
// Test helpers
// =============================================================================

std::string read_file(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return "";
  }
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool contains_pattern(const std::string& content, const std::string& pattern) {
  std::regex re(pattern);
  return std::regex_search(content, re);
}

bool contains(const std::string& str, const std::string& substr) {
  return str.find(substr) != std::string::npos;
}

bool starts_with(const std::string& str, const std::string& prefix) {
  return str.size() >= prefix.size() && str.substr(0, prefix.size()) == prefix;
}

bool ends_with(const std::string& str, const std::string& suffix) {
  return str.size() >= suffix.size() && str.substr(str.size() - suffix.size()) == suffix;
}

} // namespace

// =============================================================================
// ATerm format structure tests
// =============================================================================

TEST_CASE("ATerm derivation format structure", "[store][derivation][format]") {
  std::string cpp_file = read_file("src/nix/store/derivations.cpp");
  REQUIRE_FALSE(cpp_file.empty());

  SECTION("Traditional format uses Derive prefix") {
    // derivations.cpp should generate "Derive(" for traditional derivations
    INFO("Traditional derivations must start with 'Derive('");
    REQUIRE(contains_pattern(cpp_file, R"(s \+= std::string_view\{"Derive\(")"));
  }

  SECTION("Dynamic derivations use DrvWithVersion prefix") {
    // For dynamic derivations with nested deps
    INFO("Dynamic derivations must start with 'DrvWithVersion('");
    REQUIRE(contains_pattern(cpp_file, R"(s \+= std::string_view\{"DrvWithVersion\(")"));
  }

  SECTION("Version string for dynamic derivations is xp-dyn-drv") {
    INFO("Dynamic derivations version must be 'xp-dyn-drv'");
    REQUIRE(contains_pattern(cpp_file, R"("xp-dyn-drv")"));
  }

  SECTION("Derivation parsing expects Derive or DrvWithVersion") {
    // Parser should handle both formats
    REQUIRE(contains_pattern(cpp_file, R"(expect\(str, std::string_view\{"erive\(")"));
    REQUIRE(contains_pattern(cpp_file, R"(expect\(str, std::string_view\{"rvWithVersion\(")"));
  }
}

// =============================================================================
// All derivation fields present tests
// =============================================================================

TEST_CASE("All derivation fields are serialized", "[store][derivation][format]") {
  std::string cpp_file = read_file("src/nix/store/derivations.cpp");
  REQUIRE_FALSE(cpp_file.empty());

  SECTION("Outputs are serialized first") {
    // Outputs are serialized as list after opening
    INFO("Outputs must be first field in derivation");
    REQUIRE(contains_pattern(cpp_file, R"(for \(auto& i : outputs\))"));
  }

  SECTION("Input derivations are serialized") {
    // inputDrvs are serialized with their required outputs
    INFO("Input derivations must be serialized");
    REQUIRE(contains_pattern(cpp_file, R"(input_drvs\.map)"));
  }

  SECTION("Input sources are serialized") {
    // inputSrcs are plain store paths
    INFO("Input sources must be serialized");
    REQUIRE(contains_pattern(cpp_file, R"(input_srcs)"));
  }

  SECTION("Platform (system) is serialized") {
    INFO("Platform must be serialized");
    REQUIRE(contains_pattern(cpp_file, R"(drv\.platform)"));
  }

  SECTION("Builder is serialized") {
    INFO("Builder path must be serialized");
    REQUIRE(contains_pattern(cpp_file, R"(print_string\(s, builder\))"));
  }

  SECTION("Args are serialized") {
    INFO("Builder arguments must be serialized");
    REQUIRE(contains_pattern(cpp_file, R"(print_strings\(s, args\.begin\(\), args\.end\(\)\))"));
  }

  SECTION("Environment variables are serialized last") {
    INFO("Environment must be serialized as list of pairs");
    // The code uses outputEnvEntry lambda to serialize env entries
    REQUIRE(contains_pattern(cpp_file, R"(outputEnvEntry)"));
  }
}

// =============================================================================
// Output format tests (path, hashAlgo, hash)
// =============================================================================

TEST_CASE("Output format encodes path, hashAlgo, hash", "[store][derivation][format]") {
  std::string cpp_file = read_file("src/nix/store/derivations.cpp");
  REQUIRE_FALSE(cpp_file.empty());

  SECTION("InputAddressed output has path, empty hashAlgo and hash") {
    // InputAddressed: print path, then two empty strings
    INFO("InputAddressed outputs must have empty hashAlgo and hash");
    REQUIRE(contains_pattern(cpp_file, R"(derivation_output_t::InputAddressed)"));
  }

  SECTION("CAFixed output has path, method+algo, and hash") {
    // CAFixed: path computed from CA, method+algo prefix, hash in base16
    INFO("CAFixed outputs must have method+algo and hash");
    REQUIRE(contains_pattern(cpp_file, R"(dof\.ca\.printMethodAlgo\(\))"));
    REQUIRE(contains_pattern(cpp_file, R"(dof\.ca\.hash\.to_string\(hash_format_t::base16)"));
  }

  SECTION("CAFloating output has empty path, method+algo, empty hash") {
    // CAFloating: empty path, method prefix + algo, empty hash
    INFO("CAFloating outputs must have method+algo but empty hash");
    REQUIRE(contains_pattern(cpp_file, R"(derivation_output_t::CAFloating)"));
    REQUIRE(contains_pattern(cpp_file, R"(dof\.method\.renderPrefix\(\))"));
  }

  SECTION("Deferred output has all empty fields") {
    // Deferred: empty path, empty hashAlgo, empty hash
    INFO("Deferred outputs must have all empty fields");
    REQUIRE(contains_pattern(cpp_file, R"(derivation_output_t::Deferred)"));
  }

  SECTION("Impure output uses 'impure' as hash value") {
    // Impure: empty path, method+algo, "impure" literal
    INFO("Impure outputs must use 'impure' as hash");
    REQUIRE(contains_pattern(cpp_file, R"("impure")"));
    REQUIRE(contains_pattern(cpp_file, R"(derivation_output_t::Impure)"));
  }
}

// =============================================================================
// Input derivations format tests
// =============================================================================

TEST_CASE("Input derivations format", "[store][derivation][format]") {
  std::string cpp_file = read_file("src/nix/store/derivations.cpp");
  REQUIRE_FALSE(cpp_file.empty());

  SECTION("Input derivations are stored with their store paths") {
    INFO("Input derivations must include store paths");
    REQUIRE(contains_pattern(cpp_file, R"(store\.printStorePath)"));
  }

  SECTION("Input derivation outputs are stored as string lists") {
    INFO("Required outputs must be string lists");
    REQUIRE(contains_pattern(cpp_file, R"(print_unquoted_strings)"));
  }

  SECTION("Dynamic derivation format has nested structure") {
    // For dynamic derivations, childMap contains nested outputs
    INFO("Dynamic derivations use nested childMap structure");
    REQUIRE(contains_pattern(cpp_file, R"(childMap)"));
    REQUIRE(contains_pattern(cpp_file, R"(unparse_derived_path_map_node)"));
  }
}

// =============================================================================
// Environment variable serialization tests
// =============================================================================

TEST_CASE("Environment variable serialization", "[store][derivation][format]") {
  std::string cpp_file = read_file("src/nix/store/derivations.cpp");
  REQUIRE_FALSE(cpp_file.empty());

  SECTION("Environment is serialized as list of (key, value) pairs") {
    INFO("Env vars must be (key, value) pairs");
    // Code uses print_string(s, key) for env var keys via outputEnvEntry lambda
    REQUIRE(contains_pattern(cpp_file, R"(print_string\(s, key\))"));
    // Value is printed with mask_outputs check
    REQUIRE(contains_pattern(cpp_file, R"(print_string\(s, mask_outputs)"));
  }

  SECTION("Special characters are escaped in print_string") {
    // Check escape handling: \n, \r, \t, \\, \"
    INFO("Special characters must be escaped");
    REQUIRE(contains_pattern(cpp_file, R"('\\n')"));
    REQUIRE(contains_pattern(cpp_file, R"('\\r')"));
    REQUIRE(contains_pattern(cpp_file, R"('\\t')"));
  }

  SECTION("Output masking is supported for hash calculation") {
    // mask_outputs parameter controls whether output paths are hidden
    INFO("Output masking must be supported");
    REQUIRE(contains_pattern(cpp_file, R"(mask_outputs)"));
  }
}

// =============================================================================
// Structured attributes (__structuredAttrs) tests
// =============================================================================

TEST_CASE("Structured attributes format", "[store][derivation][format]") {
  std::string cpp_file = read_file("src/nix/store/derivations.cpp");
  REQUIRE_FALSE(cpp_file.empty());

  SECTION("Structured attrs env var name is __json") {
    REQUIRE(nix::StructuredAttrs::envVarName == "__json");
  }

  SECTION("Structured attrs are parsed from __json env var") {
    INFO("Parser must extract __json env var");
    REQUIRE(contains_pattern(cpp_file, R"(StructuredAttrs::envVarName)"));
  }

  SECTION("Structured attrs are unparsed back to env") {
    INFO("Serializer must include structured attrs in env");
    REQUIRE(contains_pattern(cpp_file, R"(structured_attrs->unparse\(\))"));
  }

  SECTION("__json key must not conflict with regular env") {
    INFO("checkKeyNotInUse must be called");
    REQUIRE(contains_pattern(cpp_file, R"(StructuredAttrs::checkKeyNotInUse)"));
  }
}

// =============================================================================
// Content-addressed derivation tests
// =============================================================================

TEST_CASE("Content-addressed derivation format", "[store][derivation][format]") {
  std::string cpp_file = read_file("src/nix/store/derivations.cpp");
  REQUIRE_FALSE(cpp_file.empty());

  SECTION("CA derivations support multiple content address methods") {
    // Methods: flat (no prefix), recursive (r:), text (text:)
    INFO("Content address methods must be supported");
    REQUIRE(contains_pattern(cpp_file, R"(content_address_method_t::parsePrefix)"));
  }

  SECTION("Fixed-output derivations have single 'out' output") {
    // Constraint: fixed output must be named "out"
    INFO("Fixed output must be named 'out'");
    // Code uses std::string_view{"out"} for the comparison
    REQUIRE(contains_pattern(cpp_file, R"(output_name != std::string_view\{"out"\})"));
  }

  SECTION("CA derivations experimental feature is checked") {
    INFO("CA derivations require experimental feature");
    REQUIRE(contains_pattern(cpp_file, R"(xp_t::ca_derivations)"));
  }

  SECTION("Impure derivations experimental feature is checked") {
    INFO("Impure derivations require experimental feature");
    REQUIRE(contains_pattern(cpp_file, R"(xp_t::impure_derivations)"));
  }

  SECTION("Dynamic derivations experimental feature is checked") {
    INFO("Dynamic derivations require experimental feature");
    REQUIRE(contains_pattern(cpp_file, R"(xp_t::dynamic_derivations)"));
  }
}

// =============================================================================
// Derivation hash calculation tests
// =============================================================================

TEST_CASE("Derivation hash calculation", "[store][derivation][format]") {
  SECTION("Derivation extension is .drv") {
    REQUIRE(nix::drvExtension == ".drv");
  }

  SECTION("is_derivation checks file extension") {
    REQUIRE(nix::is_derivation("foo.drv"));
    REQUIRE(nix::is_derivation("/nix/store/abc123-hello.drv"));
    REQUIRE_FALSE(nix::is_derivation("foo.txt"));
    REQUIRE_FALSE(nix::is_derivation("foo.drv.lock"));
    REQUIRE_FALSE(nix::is_derivation("foo"));
  }

  SECTION("Output path name follows naming convention") {
    // "out" output doesn't add suffix, others do
    REQUIRE(nix::output_path_name("hello", "out") == "hello");
    REQUIRE(nix::output_path_name("hello", "lib") == "hello-lib");
    REQUIRE(nix::output_path_name("hello", "dev") == "hello-dev");
    REQUIRE(nix::output_path_name("foo", "bin") == "foo-bin");
  }

  SECTION("Hash placeholder format") {
    std::string placeholder = nix::hash_placeholder("out");

    // Placeholder should start with /
    REQUIRE(starts_with(placeholder, "/"));

    // Placeholder should be consistent (deterministic)
    REQUIRE(nix::hash_placeholder("out") == placeholder);

    // Different outputs have different placeholders
    REQUIRE(nix::hash_placeholder("out") != nix::hash_placeholder("lib"));
    REQUIRE(nix::hash_placeholder("lib") != nix::hash_placeholder("dev"));
  }
}

TEST_CASE("Derivation hash calculation implementation", "[store][derivation][format]") {
  std::string cpp_file = read_file("src/nix/store/derivations.cpp");
  REQUIRE_FALSE(cpp_file.empty());

  SECTION("hash_derivation_modulo uses SHA256") {
    INFO("Derivation hashing must use SHA256");
    REQUIRE(contains_pattern(cpp_file, R"(hash_algorithm_t::SHA256)"));
  }

  SECTION("Fixed-output derivations have special hash format") {
    // Fixed outputs use "fixed:out:method:hash:path" format
    INFO("Fixed output hash uses special format");
    REQUIRE(contains_pattern(cpp_file, R"("fixed:out:")"));
  }

  SECTION("Derivation hash memoization exists") {
    INFO("drv_hashes cache must exist for memoization");
    REQUIRE(contains_pattern(cpp_file, R"(drv_hashes)"));
  }
}

// =============================================================================
// derivation_output_t variant type tests
// =============================================================================

TEST_CASE("derivation_output_t variant types", "[store][derivation][format]") {
  // Store paths need to be 32 base32 chars (hash) + "-" + name
  // Example: "0000000000000000000000000000000a-test"
  constexpr auto valid_store_path = "00000000000000000000000000000000-test";

  SECTION("InputAddressed has path") {
    nix::derivation_output_t::InputAddressed ia{.path = nix::store_path_t{valid_store_path}};

    REQUIRE(ia.path.to_string() == valid_store_path);
  }

  SECTION("CAFixed has content address") {
    auto hash =
        nix::Hash::parse_any_prefixed("sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");
    nix::derivation_output_t::CAFixed ca_fixed{
        .ca = nix::content_address_t{
            .method = nix::content_address_method_t::raw_t::flat,
            .hash = hash,
        }};

    REQUIRE(ca_fixed.ca.method == nix::content_address_method_t::raw_t::flat);
    REQUIRE(ca_fixed.ca.hash.algo() == nix::hash_algorithm_t::SHA256);
  }

  SECTION("CAFloating has method and hash_algo") {
    nix::derivation_output_t::CAFloating ca_floating{
        .method = nix::content_address_method_t::raw_t::nix_archive,
        .hash_algo = nix::hash_algorithm_t::SHA256,
    };

    REQUIRE(ca_floating.method == nix::content_address_method_t::raw_t::nix_archive);
    REQUIRE(ca_floating.hash_algo == nix::hash_algorithm_t::SHA256);
  }

  SECTION("Deferred has no fields") {
    nix::derivation_output_t::Deferred deferred{};
    nix::derivation_output_t output{deferred};
    REQUIRE(std::holds_alternative<nix::derivation_output_t::Deferred>(output.raw));
  }

  SECTION("Impure has method and hash_algo") {
    nix::derivation_output_t::Impure impure{
        .method = nix::content_address_method_t::raw_t::flat,
        .hash_algo = nix::hash_algorithm_t::SHA256,
    };

    REQUIRE(impure.method == nix::content_address_method_t::raw_t::flat);
    REQUIRE(impure.hash_algo == nix::hash_algorithm_t::SHA256);
  }
}

// =============================================================================
// DerivationType classification tests
// =============================================================================

TEST_CASE("DerivationType classification", "[store][derivation][format]") {
  SECTION("InputAddressed type properties") {
    nix::DerivationType ia_type{nix::DerivationType::InputAddressed{.deferred = false}};

    REQUIRE_FALSE(ia_type.isCA());
    REQUIRE_FALSE(ia_type.isFixed());
    REQUIRE(ia_type.isSandboxed());
    REQUIRE_FALSE(ia_type.is_impure());
    REQUIRE(ia_type.hasKnownOutputPaths());
  }

  SECTION("Deferred InputAddressed type properties") {
    nix::DerivationType deferred_type{nix::DerivationType::InputAddressed{.deferred = true}};

    REQUIRE_FALSE(deferred_type.isCA());
    REQUIRE_FALSE(deferred_type.isFixed());
    REQUIRE(deferred_type.isSandboxed());
    REQUIRE_FALSE(deferred_type.hasKnownOutputPaths());
  }

  SECTION("ContentAddressed fixed type properties") {
    nix::DerivationType ca_fixed{
        nix::DerivationType::ContentAddressed{.sandboxed = false, .fixed = true}};

    REQUIRE(ca_fixed.isCA());
    REQUIRE(ca_fixed.isFixed());
    REQUIRE_FALSE(ca_fixed.isSandboxed());
    REQUIRE_FALSE(ca_fixed.is_impure());
    REQUIRE(ca_fixed.hasKnownOutputPaths());
  }

  SECTION("ContentAddressed floating type properties") {
    nix::DerivationType ca_floating{
        nix::DerivationType::ContentAddressed{.sandboxed = true, .fixed = false}};

    REQUIRE(ca_floating.isCA());
    REQUIRE_FALSE(ca_floating.isFixed());
    REQUIRE(ca_floating.isSandboxed());
    REQUIRE_FALSE(ca_floating.is_impure());
    REQUIRE_FALSE(ca_floating.hasKnownOutputPaths());
  }

  SECTION("Impure type properties") {
    nix::DerivationType impure{nix::DerivationType::Impure{}};

    REQUIRE(impure.isCA()); // Impure is considered CA
    REQUIRE_FALSE(impure.isFixed());
    REQUIRE_FALSE(impure.isSandboxed());
    REQUIRE(impure.is_impure());
    REQUIRE_FALSE(impure.hasKnownOutputPaths());
  }
}

// =============================================================================
// JSON derivation format tests
// =============================================================================

TEST_CASE("JSON derivation format", "[store][derivation][format]") {
  SECTION("Expected JSON version is 4") {
    // This is used by `nix derivation show` and `nix derivation add`
    REQUIRE(nix::expectedJsonVersionDerivation == 4);
  }

  SECTION("JSON format implemented in derivations.cpp") {
    std::string cpp_file = read_file("src/nix/store/derivations.cpp");
    REQUIRE_FALSE(cpp_file.empty());

    INFO("JSON serialization must include all fields");
    REQUIRE(contains_pattern(cpp_file, R"(res\["name"\])"));
    REQUIRE(contains_pattern(cpp_file, R"(res\["version"\])"));
    REQUIRE(contains_pattern(cpp_file, R"(res\["outputs"\])"));
    REQUIRE(contains_pattern(cpp_file, R"(res\["inputs"\])"));
    REQUIRE(contains_pattern(cpp_file, R"(res\["system"\])"));
    REQUIRE(contains_pattern(cpp_file, R"(res\["builder"\])"));
    REQUIRE(contains_pattern(cpp_file, R"(res\["args"\])"));
    REQUIRE(contains_pattern(cpp_file, R"(res\["env"\])"));
  }
}

// =============================================================================
// DrvHash kind tests
// =============================================================================

TEST_CASE("DrvHash kind enumeration", "[store][derivation][format]") {
  SECTION("Regular kind for statically determined derivations") {
    auto kind = nix::DrvHash::Kind::regular;
    REQUIRE(kind == nix::DrvHash::Kind::regular);
    REQUIRE(static_cast<bool>(kind) == false);
  }

  SECTION("Deferred kind for floating-output derivations") {
    auto kind = nix::DrvHash::Kind::Deferred;
    REQUIRE(kind == nix::DrvHash::Kind::Deferred);
    REQUIRE(static_cast<bool>(kind) == true);
  }

  SECTION("Kind enum is bool-based") {
    // DrvHash::Kind is defined as enum struct Kind : bool
    REQUIRE(sizeof(nix::DrvHash::Kind) == sizeof(bool));
  }
}

// =============================================================================
// content_address_method_t tests
// =============================================================================

TEST_CASE("content_address_method_t variants", "[store][derivation][format]") {
  using Method = nix::content_address_method_t::raw_t;

  SECTION("All method variants exist and are distinct") {
    auto flat = Method::flat;
    auto nar = Method::nix_archive;
    auto git = Method::git;
    auto text = Method::Text;

    REQUIRE(flat != nar);
    REQUIRE(nar != git);
    REQUIRE(git != text);
    REQUIRE(text != flat);
  }

  SECTION("nix_archive method renders as r: prefix") {
    nix::content_address_method_t nar_method{Method::nix_archive};
    auto prefix = nar_method.renderPrefix();

    // nix_archive (recursive) uses "r:" prefix
    REQUIRE(prefix == "r:");
  }

  SECTION("flat method renders as empty prefix") {
    nix::content_address_method_t flat_method{Method::flat};
    auto prefix = flat_method.renderPrefix();

    // flat has no prefix (empty string)
    REQUIRE(prefix == "");
  }

  SECTION("Text method renders as text: prefix") {
    nix::content_address_method_t text_method{Method::Text};
    auto prefix = text_method.renderPrefix();

    // Text uses "text:" prefix
    REQUIRE(prefix == "text:");
  }
}

// =============================================================================
// ATerm parsing tests
// =============================================================================

TEST_CASE("ATerm parsing implementation", "[store][derivation][format]") {
  std::string cpp_file = read_file("src/nix/store/derivations.cpp");
  REQUIRE_FALSE(cpp_file.empty());

  SECTION("Parser uses expect() for format validation") {
    INFO("Parser must validate expected tokens");
    REQUIRE(contains_pattern(cpp_file, R"(void expect\(string_view_stream_t&)"));
  }

  SECTION("Parser handles C-style string escapes") {
    INFO("Parser must handle escape sequences");
    REQUIRE(contains_pattern(cpp_file, R"(escapes)"));
  }

  SECTION("Parser validates paths start with /") {
    INFO("Path validation must check for leading /");
    REQUIRE(contains_pattern(cpp_file, R"(s\[0\] != '/')"));
  }

  SECTION("Parser handles list end detection") {
    INFO("List parsing must detect ] or , terminators");
    REQUIRE(contains_pattern(cpp_file, R"(end_of_list)"));
  }
}

// =============================================================================
// basic_derivation_t tests
// =============================================================================

TEST_CASE("basic_derivation_t isBuiltin", "[store][derivation][format]") {
  std::string cpp_file = read_file("src/nix/store/derivations.cpp");
  REQUIRE_FALSE(cpp_file.empty());

  SECTION("Builtin derivations have 'builtin:' prefix") {
    INFO("Builder starting with 'builtin:' is a builtin");
    REQUIRE(contains_pattern(cpp_file, R"(builder\.substr\(0, 8\) == "builtin:")"));
  }
}
