// straylight // nix // store // tests
//
// Derivation parsing and serialization benchmarks
//
// These benchmarks measure the performance of real-world derivation operations
// that happen constantly during builds:
//   1. Parse small derivation (few deps, small env)
//   2. Parse large derivation (100+ inputDrvs, large env)
//   3. Parse derivation with structured attrs (__structuredAttrs)
//   4. Serialize derivation to ATerm format
//   5. Derivation hash calculation
//
// Derivation parsing is on the critical path for every build operation.

#include <random>
#include <string>
#include <vector>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include "nix/store/derivations.h"
#include "nix/store/globals.h"
#include "nix/store/path.h"
#include "nix/store/store-api.h"
#include "nix/util/hash.h"

namespace {

// =============================================================================
// Realistic .drv content generators
// =============================================================================

// Generate a valid nix32 hash (32 chars from nix32 alphabet)
std::string make_nix32_hash(uint32_t seed) {
  // Nix base32 alphabet: 0123456789abcdfghijklmnpqrsvwxyz (no e, o, t, u)
  static constexpr char alphabet[] = "0123456789abcdfghijklmnpqrsvwxyz";
  std::mt19937 rng(seed);
  std::string hash;
  hash.reserve(32);
  for (int i = 0; i < 32; ++i) {
    hash += alphabet[rng() % 32];
  }
  return hash;
}

// Generate a store path like /nix/store/<hash>-<name>
std::string make_store_path(uint32_t seed, const std::string& name) {
  return "/nix/store/" + make_nix32_hash(seed) + "-" + name;
}

// Generate a small derivation (typical simple package)
// Pattern: few outputs, 5-10 inputDrvs, small env
std::string generate_small_derivation() {
  std::string drv;

  // Small package with out, lib outputs
  std::string out_path = make_store_path(100, "hello-2.12.1");
  std::string lib_path = make_store_path(101, "hello-2.12.1-lib");

  drv += "Derive([";
  drv += "(\"lib\",\"" + lib_path + "\",\"\",\"\"),";
  drv += "(\"out\",\"" + out_path + "\",\"\",\"\")";
  drv += "],[";

  // 8 input derivations (typical for a simple package)
  std::vector<std::string> input_names = {"bash-5.2",     "coreutils-9.3", "gcc-13.2.0",
                                          "glibc-2.38",   "gnumake-4.4",   "gnutar-1.35",
                                          "stdenv-linux", "gzip-1.12"};
  for (size_t i = 0; i < input_names.size(); ++i) {
    if (i > 0) {
      drv += ",";
    }
    drv += "(\"" + make_store_path(200 + i, input_names[i]) + ".drv\",[\"out\"])";
  }

  drv += "],[";
  // 2 input sources
  drv += "\"" + make_store_path(300, "hello-2.12.1.tar.gz") + "\",";
  drv += "\"" + make_store_path(301, "setup.sh") + "\"";
  drv += "],";

  // System
  drv += "\"x86_64-linux\",";

  // Builder
  drv += "\"/nix/store/" + make_nix32_hash(400) + "-bash-5.2/bin/bash\",";

  // Args
  drv += "[\"-e\",\"/nix/store/" + make_nix32_hash(401) + "-builder.sh\"],";

  // Environment variables (typical small set)
  drv += "[";
  drv += "(\"buildInputs\",\"\"),";
  drv += "(\"builder\",\"/nix/store/" + make_nix32_hash(400) + "-bash-5.2/bin/bash\"),";
  drv += "(\"configureFlags\",\"--prefix=$out\"),";
  drv += "(\"lib\",\"" + lib_path + "\"),";
  drv += "(\"name\",\"hello-2.12.1\"),";
  drv += "(\"nativeBuildInputs\",\"\"),";
  drv += "(\"out\",\"" + out_path + "\"),";
  drv += "(\"outputs\",\"out lib\"),";
  drv += "(\"pname\",\"hello\"),";
  drv += "(\"src\",\"" + make_store_path(300, "hello-2.12.1.tar.gz") + "\"),";
  drv += "(\"stdenv\",\"" + make_store_path(207, "stdenv-linux") + "\"),";
  drv += "(\"system\",\"x86_64-linux\"),";
  drv += "(\"version\",\"2.12.1\")";
  drv += "])";

  return drv;
}

// Generate a large derivation (typical complex package like firefox, chromium)
// Pattern: many outputs, 100+ inputDrvs, large env with many paths
std::string generate_large_derivation(size_t num_input_drvs = 150) {
  std::string drv;

  // Multiple outputs (like a complex package)
  std::vector<std::pair<std::string, std::string>> outputs = {
      {"bin", make_store_path(1000, "firefox-120.0-bin")},
      {"dev", make_store_path(1001, "firefox-120.0-dev")},
      {"doc", make_store_path(1002, "firefox-120.0-doc")},
      {"lib", make_store_path(1003, "firefox-120.0-lib")},
      {"out", make_store_path(1004, "firefox-120.0")},
  };

  drv += "Derive([";
  for (size_t i = 0; i < outputs.size(); ++i) {
    if (i > 0) {
      drv += ",";
    }
    drv += "(\"" + outputs[i].first + "\",\"" + outputs[i].second + "\",\"\",\"\")";
  }
  drv += "],[";

  // Many input derivations (simulate large dependency tree)
  std::vector<std::string> dep_names = {
      "alsa-lib",      "at-spi2-atk",   "atk",          "bzip2",      "cairo",
      "cups",          "dbus",          "dbus-glib",    "expat",      "ffmpeg",
      "fontconfig",    "freetype",      "fribidi",      "gdk-pixbuf", "glib",
      "gtk3",          "harfbuzz",      "icu",          "libdrm",     "libepoxy",
      "libevent",      "libffi",        "libGL",        "libICE",     "libjpeg",
      "libpng",        "libpulseaudio", "libSM",        "libva",      "libvpx",
      "libwebp",       "libX11",        "libXau",       "libxcb",     "libXcomposite",
      "libXcursor",    "libXdamage",    "libXdmcp",     "libXext",    "libXfixes",
      "libXi",         "libXinerama",   "libxkbcommon", "libXrandr",  "libXrender",
      "libXScrnSaver", "libXt",         "mesa",         "nspr",       "nss",
      "pango",         "pcre2",         "pixman",       "sqlite",     "systemd",
      "wayland",       "xorg-libxcb",   "zlib",         "rust",       "cargo",
      "nodejs",        "python3",       "llvm",         "clang",      "cmake",
      "ninja",         "pkg-config",    "which",        "perl",       "m4",
      "autoconf",      "automake",      "libtool",      "gettext",    "bison",
      "flex",          "gperf",         "yasm",         "nasm",       "cbindgen",
  };

  for (size_t i = 0; i < num_input_drvs; ++i) {
    if (i > 0) {
      drv += ",";
    }
    std::string name = (i < dep_names.size()) ? dep_names[i] + "-1.0" : "dep-" + std::to_string(i);
    // Mix of ["out"] and ["out", "dev", "lib"] outputs
    if (i % 4 == 0) {
      drv += "(\"" + make_store_path(2000 + i, name) + ".drv\",[\"dev\",\"lib\",\"out\"])";
    } else if (i % 3 == 0) {
      drv += "(\"" + make_store_path(2000 + i, name) + ".drv\",[\"dev\",\"out\"])";
    } else {
      drv += "(\"" + make_store_path(2000 + i, name) + ".drv\",[\"out\"])";
    }
  }
  drv += "],[";

  // Many input sources
  for (int i = 0; i < 20; ++i) {
    if (i > 0) {
      drv += ",";
    }
    drv += "\"" + make_store_path(3000 + i, "source-" + std::to_string(i)) + "\"";
  }
  drv += "],";

  drv += "\"x86_64-linux\",";
  drv += "\"/nix/store/" + make_nix32_hash(4000) + "-bash-5.2/bin/bash\",";
  drv += "[\"-e\",\"/nix/store/" + make_nix32_hash(4001) + "-builder.sh\"],";

  // Large environment (many variables with long path lists)
  drv += "[";

  // Build inputs with many paths
  drv += "(\"buildInputs\",\"";
  for (size_t i = 0; i < 30; ++i) {
    if (i > 0) {
      drv += " ";
    }
    drv += make_store_path(5000 + i, "input-" + std::to_string(i));
  }
  drv += "\"),";

  // Native build inputs
  drv += "(\"nativeBuildInputs\",\"";
  for (size_t i = 0; i < 20; ++i) {
    if (i > 0) {
      drv += " ";
    }
    drv += make_store_path(5100 + i, "native-" + std::to_string(i));
  }
  drv += "\"),";

  // Propagated build inputs
  drv += "(\"propagatedBuildInputs\",\"";
  for (size_t i = 0; i < 15; ++i) {
    if (i > 0) {
      drv += " ";
    }
    drv += make_store_path(5200 + i, "prop-" + std::to_string(i));
  }
  drv += "\"),";

  // Many configure flags
  drv += "(\"configureFlags\",\"--enable-official-branding --enable-application=browser "
         "--with-system-jpeg --with-system-zlib --with-system-bz2 --with-system-png "
         "--with-system-libevent --with-system-libvpx --with-system-icu --enable-system-ffi "
         "--enable-system-pixman --enable-alsa --enable-jack --enable-pulseaudio "
         "--disable-tests --disable-debug --enable-optimize --enable-release "
         "--enable-rust-simd --enable-av1\"),";

  // Standard outputs
  for (const auto& [name, path] : outputs) {
    drv += "(\"" + name + "\",\"" + path + "\"),";
  }

  drv += "(\"outputs\",\"out bin lib dev doc\"),";
  drv += "(\"pname\",\"firefox\"),";
  drv += "(\"version\",\"120.0\"),";
  drv += "(\"name\",\"firefox-120.0\"),";
  drv += "(\"system\",\"x86_64-linux\"),";

  // More typical environment variables
  drv += "(\"dontStrip\",\"\"),";
  drv += "(\"enableParallelBuilding\",\"1\"),";
  drv += "(\"hardeningDisable\",\"format\"),";
  drv += "(\"meta\",\"{\\\"description\\\":\\\"Firefox web browser\\\","
         "\\\"homepage\\\":\\\"https://www.mozilla.org/firefox/\\\","
         "\\\"license\\\":{\\\"shortName\\\":\\\"MPL-2.0\\\"},"
         "\\\"platforms\\\":[\\\"x86_64-linux\\\",\\\"aarch64-linux\\\"]}\"),";

  drv += "(\"builder\",\"/nix/store/" + make_nix32_hash(4000) + "-bash-5.2/bin/bash\")";
  drv += "])";

  return drv;
}

// Generate a derivation with structured attrs (__structuredAttrs = true)
// This uses JSON encoding in __json env var
std::string generate_structured_attrs_derivation() {
  std::string drv;

  std::string out_path = make_store_path(6000, "structured-attrs-pkg");
  std::string bin_path = make_store_path(6001, "structured-attrs-pkg-bin");
  std::string dev_path = make_store_path(6002, "structured-attrs-pkg-dev");

  drv += "Derive([";
  drv += "(\"bin\",\"" + bin_path + "\",\"\",\"\"),";
  drv += "(\"dev\",\"" + dev_path + "\",\"\",\"\"),";
  drv += "(\"out\",\"" + out_path + "\",\"\",\"\")";
  drv += "],[";

  // Input derivations
  for (int i = 0; i < 15; ++i) {
    if (i > 0) {
      drv += ",";
    }
    drv += "(\"" + make_store_path(7000 + i, "dep-" + std::to_string(i)) + ".drv\",[\"out\"])";
  }
  drv += "],[";

  // Input sources
  drv += "\"" + make_store_path(7100, "source.tar.gz") + "\"";
  drv += "],";

  drv += "\"x86_64-linux\",";
  drv += "\"/bin/bash\",";
  drv += "[\"-c\",\"echo hello > $out\"],";

  // Environment with __json structured attrs
  drv += "[";

  // The __json contains a complex JSON structure
  // This is a realistic structured attrs block
  drv += "(\"__json\",\"{";
  drv += "\\\"__darwinAllowLocalNetworking\\\":true,";
  drv += "\\\"__impureHostDeps\\\":[\\\"/usr/bin/ditto\\\",\\\"/usr/bin/xcrun\\\"],";
  drv += "\\\"__noChroot\\\":false,";
  drv += "\\\"__sandboxProfile\\\":\\\"(version 1)\\\\n(allow default)\\\",";
  drv += "\\\"allowSubstitutes\\\":true,";
  drv += "\\\"builder\\\":\\\"/bin/bash\\\",";
  drv += "\\\"exportReferencesGraph\\\":{";
  drv += "\\\"refs1\\\":[\\\"" + make_store_path(8000, "ref1") + "\\\"],";
  drv += "\\\"refs2\\\":[\\\"" + make_store_path(8001, "ref2") + "\\\"]";
  drv += "},";
  drv += "\\\"impureEnvVars\\\":[\\\"HOME\\\",\\\"USER\\\",\\\"DISPLAY\\\"],";
  drv += "\\\"name\\\":\\\"structured-attrs-pkg\\\",";
  drv += "\\\"outputChecks\\\":{";
  drv += "\\\"bin\\\":{";
  drv += "\\\"disallowedReferences\\\":[\\\"" + make_store_path(8100, "forbidden1") + "\\\"],";
  drv += "\\\"disallowedRequisites\\\":[\\\"" + make_store_path(8101, "forbidden2") + "\\\"]";
  drv += "},";
  drv += "\\\"dev\\\":{";
  drv += "\\\"maxClosureSize\\\":104857600,";
  drv += "\\\"maxSize\\\":10485760";
  drv += "},";
  drv += "\\\"out\\\":{";
  drv += "\\\"allowedReferences\\\":[\\\"" + make_store_path(8200, "allowed1") + "\\\"],";
  drv +=
      "\\\"allowedRequisites\\\":[\\\"" + make_store_path(8201, "allowed2") + "\\\",\\\"bin\\\"]";
  drv += "}";
  drv += "},";
  drv += "\\\"outputs\\\":[\\\"out\\\",\\\"bin\\\",\\\"dev\\\"],";
  drv += "\\\"passAsFile\\\":[\\\"buildCommand\\\"],";
  drv += "\\\"preferLocalBuild\\\":true,";
  drv += "\\\"requiredSystemFeatures\\\":[\\\"kvm\\\",\\\"big-parallel\\\"],";
  drv += "\\\"system\\\":\\\"x86_64-linux\\\"";
  drv += "}\"),";

  // Output paths in env (required for parsing)
  drv += "(\"bin\",\"" + bin_path + "\"),";
  drv += "(\"dev\",\"" + dev_path + "\"),";
  drv += "(\"out\",\"" + out_path + "\")";
  drv += "])";

  return drv;
}

// Store directory path for benchmarking
// This must be static to ensure lifetime exceeds store_dir_config_t references
static const std::string kStoreDir = "/nix/store";

// Get a store_dir_config_t for benchmarking
nix::store_dir_config_t get_bench_store_config() {
  return nix::store_dir_config_t{.store_dir = kStoreDir};
}

} // namespace

// =============================================================================
// Benchmarks
// =============================================================================

TEST_CASE("Derivation parsing benchmarks", "[benchmark][store][derivation]") {
  auto store_config = get_bench_store_config();

  // Pre-generate derivation strings
  std::string small_drv = generate_small_derivation();
  std::string large_drv = generate_large_derivation(150);
  std::string structured_drv = generate_structured_attrs_derivation();

  SECTION("Parse small derivation (8 deps, 13 env vars)") {
    INFO("Derivation size: " << small_drv.size() << " bytes");

    BENCHMARK("parse_derivation/small") {
      std::string drv_copy = small_drv;
      auto drv = nix::parse_derivation(store_config, std::move(drv_copy), "hello-2.12.1");
      return drv.outputs.size();
    };
  }

  SECTION("Parse large derivation (150 deps, large env)") {
    INFO("Derivation size: " << large_drv.size() << " bytes");

    BENCHMARK("parse_derivation/large") {
      std::string drv_copy = large_drv;
      auto drv = nix::parse_derivation(store_config, std::move(drv_copy), "firefox-120.0");
      return drv.outputs.size();
    };
  }

  SECTION("Parse derivation with structured attrs") {
    INFO("Derivation size: " << structured_drv.size() << " bytes");

    BENCHMARK("parse_derivation/structured_attrs") {
      std::string drv_copy = structured_drv;
      auto drv = nix::parse_derivation(store_config, std::move(drv_copy), "structured-attrs-pkg");
      return drv.outputs.size();
    };
  }
}

TEST_CASE("Derivation serialization benchmarks", "[benchmark][store][derivation]") {
  auto store_config = get_bench_store_config();

  // Parse derivations once to get derivation_t objects
  std::string small_drv_str = generate_small_derivation();
  std::string large_drv_str = generate_large_derivation(150);
  std::string structured_drv_str = generate_structured_attrs_derivation();

  auto small_drv = nix::parse_derivation(store_config, std::string(small_drv_str), "hello-2.12.1");
  auto large_drv = nix::parse_derivation(store_config, std::string(large_drv_str), "firefox-120.0");
  auto structured_drv =
      nix::parse_derivation(store_config, std::string(structured_drv_str), "structured-attrs-pkg");

  SECTION("Serialize small derivation to ATerm") {
    BENCHMARK("unparse/small") {
      auto s = small_drv.unparse(store_config, false);
      return s.size();
    };
  }

  SECTION("Serialize large derivation to ATerm") {
    BENCHMARK("unparse/large") {
      auto s = large_drv.unparse(store_config, false);
      return s.size();
    };
  }

  SECTION("Serialize structured attrs derivation to ATerm") {
    BENCHMARK("unparse/structured_attrs") {
      auto s = structured_drv.unparse(store_config, false);
      return s.size();
    };
  }

  SECTION("Serialize with output masking (for hashing)") {
    BENCHMARK("unparse/large/masked") {
      auto s = large_drv.unparse(store_config, true);
      return s.size();
    };
  }
}

TEST_CASE("Derivation round-trip benchmarks", "[benchmark][store][derivation]") {
  auto store_config = get_bench_store_config();

  std::string small_drv_str = generate_small_derivation();
  std::string large_drv_str = generate_large_derivation(150);

  SECTION("Parse + serialize round-trip small") {
    BENCHMARK("roundtrip/small") {
      std::string drv_copy = small_drv_str;
      auto drv = nix::parse_derivation(store_config, std::move(drv_copy), "hello-2.12.1");
      auto s = drv.unparse(store_config, false);
      return s.size();
    };
  }

  SECTION("Parse + serialize round-trip large") {
    BENCHMARK("roundtrip/large") {
      std::string drv_copy = large_drv_str;
      auto drv = nix::parse_derivation(store_config, std::move(drv_copy), "firefox-120.0");
      auto s = drv.unparse(store_config, false);
      return s.size();
    };
  }
}

TEST_CASE("Derivation scaling benchmarks", "[benchmark][store][derivation]") {
  auto store_config = get_bench_store_config();

  SECTION("Parse derivation scaling by input count") {
    // Test how parsing scales with number of inputDrvs
    for (size_t input_count : {10, 50, 100, 200, 500}) {
      std::string drv_str = generate_large_derivation(input_count);

      BENCHMARK("parse/inputs/" + std::to_string(input_count)) {
        std::string drv_copy = drv_str;
        auto drv = nix::parse_derivation(store_config, std::move(drv_copy), "test-pkg");
        return drv.input_drvs.map.size();
      };
    }
  }
}

TEST_CASE("Derivation type detection benchmark", "[benchmark][store][derivation]") {
  auto store_config = get_bench_store_config();

  std::string drv_str = generate_large_derivation(100);
  auto drv = nix::parse_derivation(store_config, std::string(drv_str), "test-pkg");

  SECTION("Derivation type() classification") {
    BENCHMARK("type") {
      auto t = drv.type();
      return t.hasKnownOutputPaths();
    };
  }

  SECTION("Output names extraction") {
    BENCHMARK("outputNames") {
      auto names = drv.outputNames();
      return names.size();
    };
  }

  SECTION("isBuiltin check") {
    BENCHMARK("isBuiltin") {
      return drv.isBuiltin();
    };
  }
}

TEST_CASE("Derivation helper benchmarks", "[benchmark][store][derivation]") {
  SECTION("is_derivation filename check") {
    std::string drv_name = "/nix/store/abc123def456ghi789jkl012mno345pq-hello-2.12.1.drv";
    std::string non_drv = "/nix/store/abc123def456ghi789jkl012mno345pq-hello-2.12.1";

    BENCHMARK("is_derivation/true") {
      return nix::is_derivation(drv_name);
    };

    BENCHMARK("is_derivation/false") {
      return nix::is_derivation(non_drv);
    };
  }

  SECTION("output_path_name generation") {
    BENCHMARK("output_path_name/out") {
      return nix::output_path_name("firefox-120.0", "out");
    };

    BENCHMARK("output_path_name/lib") {
      return nix::output_path_name("firefox-120.0", "lib");
    };
  }

  SECTION("hash_placeholder generation") {
    BENCHMARK("hash_placeholder") {
      return nix::hash_placeholder("out");
    };
  }
}
