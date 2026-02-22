// straylight // nix // store // tests
//
// NAR Info Format Compatibility Tests
//
// These tests verify that narinfo format parsing is compatible with binary caches
// like cache.nixos.org. The narinfo format is the wire format for NAR metadata
// in binary caches, so format compatibility is critical for interoperability.
//
// Format specification (from cache.nixos.org):
//   StorePath: /nix/store/<hash>-<name>
//   URL: nar/<hash>.nar.xz
//   Compression: xz
//   FileHash: sha256:<base32>
//   FileSize: <bytes>
//   NarHash: sha256:<base32>
//   NarSize: <bytes>
//   References: <basename> <basename> ...
//   Deriver: <basename>
//   Sig: <keyname>:<base64sig>
//   CA: <content-address>

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "nix/store/nar-info.h"
#include "nix/store/store-dir-config.h"

namespace {

// =============================================================================
// Test helpers
// =============================================================================

// Valid 32-character base32 hash (160 bits)
constexpr const char* VALID_HASH_PART = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

// Valid SHA256 hash in nix32 format (52 chars)
constexpr const char* VALID_SHA256_NIX32 = "0000000000000000000000000000000000000000000000000000";

// Valid SHA256 hash in base16 format (64 chars)
constexpr const char* VALID_SHA256_HEX =
    "0000000000000000000000000000000000000000000000000000000000000000";

nix::store_dir_config_t make_store_config() {
  static const std::string store_dir = "/nix/store";
  return nix::store_dir_config_t{store_dir};
}

std::string make_store_path(const std::string& name) {
  return "/nix/store/" + std::string(VALID_HASH_PART) + "-" + name;
}

std::string make_minimal_narinfo(const std::string& name = "test") {
  // Note: the parser expects "store_path_t" as field name (refactored from "StorePath")
  return "store_path_t: " + make_store_path(name) +
         "\n"
         "URL: nar/" +
         std::string(VALID_HASH_PART) +
         ".nar\n"
         "NarHash: sha256:" +
         std::string(VALID_SHA256_HEX) +
         "\n"
         "NarSize: 1234\n";
}

} // namespace

// =============================================================================
// Required Fields Tests
// =============================================================================

TEST_CASE("narinfo: required fields - StorePath is mandatory", "[store][narinfo][format]") {
  auto config = make_store_config();

  SECTION("missing StorePath throws") {
    auto input = std::string{"URL: nar/test.nar\n"
                             "NarHash: sha256:" +
                             std::string(VALID_SHA256_HEX) +
                             "\n"
                             "NarSize: 1234\n"};

    REQUIRE_THROWS_AS(nix::nar_info_t(config, input, "test.narinfo"), nix::Error);
  }

  SECTION("present StorePath parses") {
    auto input = make_minimal_narinfo();
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE(info.path.name() == "test");
  }
}

TEST_CASE("narinfo: required fields - URL is mandatory", "[store][narinfo][format]") {
  auto config = make_store_config();

  SECTION("missing URL throws") {
    auto input = std::string{"store_path_t: " + make_store_path("test") +
                             "\n"
                             "NarHash: sha256:" +
                             std::string(VALID_SHA256_HEX) +
                             "\n"
                             "NarSize: 1234\n"};

    REQUIRE_THROWS_AS(nix::nar_info_t(config, input, "test.narinfo"), nix::Error);
  }
}

TEST_CASE("narinfo: required fields - NarHash is mandatory", "[store][narinfo][format]") {
  auto config = make_store_config();

  SECTION("missing NarHash throws") {
    auto input = std::string{"store_path_t: " + make_store_path("test") +
                             "\n"
                             "URL: nar/test.nar\n"
                             "NarSize: 1234\n"};

    REQUIRE_THROWS_AS(nix::nar_info_t(config, input, "test.narinfo"), nix::Error);
  }
}

TEST_CASE("narinfo: required fields - NarSize is mandatory", "[store][narinfo][format]") {
  auto config = make_store_config();

  SECTION("missing NarSize throws") {
    auto input = std::string{"store_path_t: " + make_store_path("test") +
                             "\n"
                             "URL: nar/test.nar\n"
                             "NarHash: sha256:" +
                             std::string(VALID_SHA256_HEX) + "\n"};

    REQUIRE_THROWS_AS(nix::nar_info_t(config, input, "test.narinfo"), nix::Error);
  }

  SECTION("NarSize of zero throws") {
    auto input = std::string{"store_path_t: " + make_store_path("test") +
                             "\n"
                             "URL: nar/test.nar\n"
                             "NarHash: sha256:" +
                             std::string(VALID_SHA256_HEX) +
                             "\n"
                             "NarSize: 0\n"};

    REQUIRE_THROWS_AS(nix::nar_info_t(config, input, "test.narinfo"), nix::Error);
  }
}

TEST_CASE("narinfo: required fields - all required fields present", "[store][narinfo][format]") {
  auto config = make_store_config();

  auto input = make_minimal_narinfo("hello");
  auto info = nix::nar_info_t(config, input, "test.narinfo");

  REQUIRE(info.path.name() == "hello");
  REQUIRE(info.url == "nar/" + std::string(VALID_HASH_PART) + ".nar");
  REQUIRE(info.nar_size == 1234);
  REQUIRE(info.nar_hash.algo() == nix::hash_algorithm_t::SHA256);
}

// =============================================================================
// Optional Fields Tests
// =============================================================================

TEST_CASE("narinfo: optional fields - Compression defaults to bzip2", "[store][narinfo][format]") {
  auto config = make_store_config();

  auto input = make_minimal_narinfo();
  auto info = nix::nar_info_t(config, input, "test.narinfo");

  REQUIRE(info.compression == "bzip2");
}

TEST_CASE("narinfo: optional fields - FileHash and FileSize", "[store][narinfo][format]") {
  auto config = make_store_config();

  auto input = make_minimal_narinfo() + "FileHash: sha256:" + std::string(VALID_SHA256_HEX) +
               "\n"
               "FileSize: 5678\n";

  auto info = nix::nar_info_t(config, input, "test.narinfo");

  REQUIRE(info.fileHash.has_value());
  REQUIRE(info.fileHash->algo() == nix::hash_algorithm_t::SHA256);
  REQUIRE(info.file_size == 5678);
}

TEST_CASE("narinfo: optional fields - References", "[store][narinfo][format]") {
  auto config = make_store_config();

  SECTION("empty references") {
    auto input = make_minimal_narinfo() + "References: \n";
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE(info.references.empty());
  }

  SECTION("single reference") {
    auto ref = std::string(VALID_HASH_PART) + "-dep";
    auto input = make_minimal_narinfo() + "References: " + ref + "\n";
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE(info.references.size() == 1);
    REQUIRE(info.references.begin()->name() == "dep");
  }

  SECTION("multiple references") {
    auto ref1 = std::string(VALID_HASH_PART) + "-dep1";
    auto ref2 = std::string(VALID_HASH_PART) + "-dep2";
    auto ref3 = std::string(VALID_HASH_PART) + "-dep3";
    auto input = make_minimal_narinfo() + "References: " + ref1 + " " + ref2 + " " + ref3 + "\n";
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE(info.references.size() == 3);
  }

  SECTION("duplicate References line throws") {
    auto ref = std::string(VALID_HASH_PART) + "-dep";
    auto input = make_minimal_narinfo() + "References: " + ref +
                 "\n"
                 "References: " +
                 ref + "\n";
    REQUIRE_THROWS_AS(nix::nar_info_t(config, input, "test.narinfo"), nix::Error);
  }
}

TEST_CASE("narinfo: optional fields - Deriver", "[store][narinfo][format]") {
  auto config = make_store_config();

  SECTION("valid deriver") {
    auto drv = std::string(VALID_HASH_PART) + "-test.drv";
    auto input = make_minimal_narinfo() + "Deriver: " + drv + "\n";
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE(info.deriver.has_value());
    REQUIRE(info.deriver->is_derivation());
  }

  SECTION("unknown-deriver is ignored") {
    auto input = make_minimal_narinfo() + "Deriver: unknown-deriver\n";
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE_FALSE(info.deriver.has_value());
  }
}

TEST_CASE("narinfo: optional fields - CA (content address)", "[store][narinfo][format]") {
  auto config = make_store_config();

  SECTION("fixed:sha256 content address") {
    auto input =
        make_minimal_narinfo() + "CA: fixed:sha256:" + std::string(VALID_SHA256_HEX) + "\n";
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE(info.ca.has_value());
  }

  SECTION("duplicate CA line throws") {
    auto input = make_minimal_narinfo() + "CA: fixed:sha256:" + std::string(VALID_SHA256_HEX) +
                 "\n"
                 "CA: fixed:sha256:" +
                 std::string(VALID_SHA256_HEX) + "\n";
    REQUIRE_THROWS_AS(nix::nar_info_t(config, input, "test.narinfo"), nix::Error);
  }
}

// =============================================================================
// Signature Format Tests
// =============================================================================

TEST_CASE("narinfo: signature format - keyname:base64sig", "[store][narinfo][format]") {
  auto config = make_store_config();

  SECTION("single signature") {
    auto input = make_minimal_narinfo() + "Sig: "
                                          "cache.nixos.org-1:"
                                          "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                                          "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n";
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE(info.sigs.size() == 1);
    REQUIRE(info.sigs.count("cache.nixos.org-1:"
                            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                            "AAAAAAAAAAAAAAAAAA") == 1);
  }

  SECTION("signature contains colon separator") {
    auto input = make_minimal_narinfo() + "Sig: my-key:somesignaturebase64\n";
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE(info.sigs.size() == 1);
    // Verify the full signature including keyname and base64 is stored
    auto it = info.sigs.begin();
    REQUIRE(it->find(':') != std::string::npos);
  }
}

TEST_CASE("narinfo: signature format - multiple signatures", "[store][narinfo][format]") {
  auto config = make_store_config();

  auto input =
      make_minimal_narinfo() +
      "Sig: "
      "key1:"
      "sig1aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
      "Sig: "
      "key2:"
      "sig2aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
      "Sig: "
      "key3:"
      "sig3aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n";
  auto info = nix::nar_info_t(config, input, "test.narinfo");

  REQUIRE(info.sigs.size() == 3);
  // Signatures are stored in a set
  bool has_key1 = false, has_key2 = false, has_key3 = false;
  for (const auto& sig : info.sigs) {
    if (sig.find("key1:") == 0)
      has_key1 = true;
    if (sig.find("key2:") == 0)
      has_key2 = true;
    if (sig.find("key3:") == 0)
      has_key3 = true;
  }
  REQUIRE(has_key1);
  REQUIRE(has_key2);
  REQUIRE(has_key3);
}

// =============================================================================
// Compression Types Tests
// =============================================================================

TEST_CASE("narinfo: compression types", "[store][narinfo][format]") {
  auto config = make_store_config();

  SECTION("none compression") {
    auto input = make_minimal_narinfo() + "Compression: none\n";
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE(info.compression == "none");
  }

  SECTION("xz compression") {
    auto input = make_minimal_narinfo() + "Compression: xz\n";
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE(info.compression == "xz");
  }

  SECTION("zstd compression") {
    auto input = make_minimal_narinfo() + "Compression: zstd\n";
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE(info.compression == "zstd");
  }

  SECTION("bzip2 compression") {
    auto input = make_minimal_narinfo() + "Compression: bzip2\n";
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE(info.compression == "bzip2");
  }

  SECTION("br compression (brotli)") {
    auto input = make_minimal_narinfo() + "Compression: br\n";
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE(info.compression == "br");
  }

  SECTION("lzip compression") {
    auto input = make_minimal_narinfo() + "Compression: lzip\n";
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE(info.compression == "lzip");
  }
}

// =============================================================================
// Field Order Tests
// =============================================================================

TEST_CASE("narinfo: field order doesn't matter", "[store][narinfo][format]") {
  auto config = make_store_config();

  SECTION("reversed order") {
    auto input = std::string{"NarSize: 1234\n"
                             "NarHash: sha256:" +
                             std::string(VALID_SHA256_HEX) +
                             "\n"
                             "URL: nar/test.nar\n"
                             "store_path_t: " +
                             make_store_path("test") + "\n"};
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE(info.path.name() == "test");
    REQUIRE(info.nar_size == 1234);
  }

  SECTION("interleaved optional fields") {
    auto ref = std::string(VALID_HASH_PART) + "-dep";
    auto input = std::string{"Compression: xz\n"
                             "store_path_t: " +
                             make_store_path("test") +
                             "\n"
                             "References: " +
                             ref +
                             "\n"
                             "URL: nar/test.nar\n"
                             "FileSize: 5678\n"
                             "NarHash: sha256:" +
                             std::string(VALID_SHA256_HEX) +
                             "\n"
                             "Sig: mykey:mysig\n"
                             "NarSize: 1234\n"
                             "FileHash: sha256:" +
                             std::string(VALID_SHA256_HEX) + "\n"};
    auto info = nix::nar_info_t(config, input, "test.narinfo");

    REQUIRE(info.compression == "xz");
    REQUIRE(info.references.size() == 1);
    REQUIRE(info.file_size == 5678);
    REQUIRE(info.sigs.size() == 1);
    REQUIRE(info.nar_size == 1234);
    REQUIRE(info.fileHash.has_value());
  }
}

// =============================================================================
// Cache.nixos.org Format Compatibility Tests
// =============================================================================

TEST_CASE("narinfo: cache.nixos.org format compatibility", "[store][narinfo][format]") {
  auto config = make_store_config();

  // This mimics the actual format from cache.nixos.org
  // Note: we use "store_path_t" instead of "StorePath" due to refactoring
  SECTION("full cache.nixos.org style narinfo") {
    auto input =
        std::string{"store_path_t: /nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-hello-2.12\n"
                    "URL: nar/1234567890abcdef.nar.xz\n"
                    "Compression: xz\n"
                    "FileHash: sha256:" +
                    std::string(VALID_SHA256_HEX) +
                    "\n"
                    "FileSize: 45678\n"
                    "NarHash: sha256:" +
                    std::string(VALID_SHA256_HEX) +
                    "\n"
                    "NarSize: 123456\n"
                    "References: " +
                    std::string(VALID_HASH_PART) + "-glibc-2.38 " + std::string(VALID_HASH_PART) +
                    "-gcc-libs-13.2\n"
                    "Deriver: " +
                    std::string(VALID_HASH_PART) +
                    "-hello-2.12.drv\n"
                    "Sig: "
                    "cache.nixos.org-1:"
                    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                    "AAAAAAAAAA\n"};

    auto info = nix::nar_info_t(config, input, "test.narinfo");

    REQUIRE(info.path.name() == "hello-2.12");
    REQUIRE(info.url == "nar/1234567890abcdef.nar.xz");
    REQUIRE(info.compression == "xz");
    REQUIRE(info.fileHash.has_value());
    REQUIRE(info.file_size == 45678);
    REQUIRE(info.nar_size == 123456);
    REQUIRE(info.references.size() == 2);
    REQUIRE(info.deriver.has_value());
    REQUIRE(info.deriver->is_derivation());
    REQUIRE(info.sigs.size() == 1);
  }

  SECTION("narinfo with multiple signatures (multi-cache scenario)") {
    auto input =
        make_minimal_narinfo("pkg") +
        "Compression: zstd\n"
        "Sig: "
        "cache.nixos.org-1:"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n"
        "Sig: "
        "mycache.example.com-1:"
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\n";

    auto info = nix::nar_info_t(config, input, "test.narinfo");

    REQUIRE(info.sigs.size() == 2);
  }
}

// =============================================================================
// Content-Addressed Narinfo Tests
// =============================================================================

TEST_CASE("narinfo: content-addressed store paths", "[store][narinfo][format]") {
  auto config = make_store_config();

  SECTION("fixed output hash (FOD)") {
    auto input =
        make_minimal_narinfo("source") + "CA: fixed:sha256:" + std::string(VALID_SHA256_HEX) + "\n";
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE(info.ca.has_value());
  }

  SECTION("text hash") {
    auto input = make_minimal_narinfo("builder.sh") +
                 "CA: text:sha256:" + std::string(VALID_SHA256_HEX) + "\n";
    auto info = nix::nar_info_t(config, input, "test.narinfo");
    REQUIRE(info.ca.has_value());
  }
}

// =============================================================================
// Round-trip Tests (parse -> serialize -> parse)
// =============================================================================

TEST_CASE("narinfo: round-trip serialization", "[store][narinfo][format]") {
  auto config = make_store_config();

  SECTION("minimal narinfo round-trips") {
    // Build a narinfo programmatically
    auto path = config.parseStorePath(make_store_path("test"));
    auto nar_hash = nix::Hash::parse_any_prefixed("sha256:" + std::string(VALID_SHA256_HEX));
    auto file_hash = nix::Hash::parse_any_prefixed("sha256:" + std::string(VALID_SHA256_HEX));

    nix::nar_info_t info1(config, path, nar_hash);
    info1.url = "nar/test.nar.xz";
    info1.compression = "xz";
    info1.fileHash = file_hash;
    info1.file_size = 1000;
    info1.nar_size = 2000;

    // Serialize
    auto serialized = info1.to_string(config);

    // Parse back
    auto info2 = nix::nar_info_t(config, serialized, "roundtrip.narinfo");

    // Verify equality
    REQUIRE(info2.path == info1.path);
    REQUIRE(info2.url == info1.url);
    REQUIRE(info2.compression == info1.compression);
    REQUIRE(info2.file_size == info1.file_size);
    REQUIRE(info2.nar_size == info1.nar_size);
    REQUIRE(info2.nar_hash == info1.nar_hash);
    REQUIRE(info2.fileHash.has_value());
    REQUIRE(info2.fileHash->algo() == info1.fileHash->algo());
  }

  SECTION("narinfo with references round-trips") {
    auto path = config.parseStorePath(make_store_path("test"));
    auto nar_hash = nix::Hash::parse_any_prefixed("sha256:" + std::string(VALID_SHA256_HEX));
    auto file_hash = nix::Hash::parse_any_prefixed("sha256:" + std::string(VALID_SHA256_HEX));

    nix::nar_info_t info1(config, path, nar_hash);
    info1.url = "nar/test.nar.xz";
    info1.compression = "xz";
    info1.fileHash = file_hash;
    info1.file_size = 1000;
    info1.nar_size = 2000;
    info1.references.insert(nix::store_path_t(std::string(VALID_HASH_PART) + "-dep1"));
    info1.references.insert(nix::store_path_t(std::string(VALID_HASH_PART) + "-dep2"));

    auto serialized = info1.to_string(config);
    auto info2 = nix::nar_info_t(config, serialized, "roundtrip.narinfo");

    REQUIRE(info2.references.size() == 2);
  }

  SECTION("narinfo with signatures round-trips") {
    auto path = config.parseStorePath(make_store_path("test"));
    auto nar_hash = nix::Hash::parse_any_prefixed("sha256:" + std::string(VALID_SHA256_HEX));
    auto file_hash = nix::Hash::parse_any_prefixed("sha256:" + std::string(VALID_SHA256_HEX));

    nix::nar_info_t info1(config, path, nar_hash);
    info1.url = "nar/test.nar.xz";
    info1.compression = "xz";
    info1.fileHash = file_hash;
    info1.file_size = 1000;
    info1.nar_size = 2000;
    info1.sigs.insert("key1:signature1");
    info1.sigs.insert("key2:signature2");

    auto serialized = info1.to_string(config);
    auto info2 = nix::nar_info_t(config, serialized, "roundtrip.narinfo");

    REQUIRE(info2.sigs.size() == 2);
    REQUIRE(info2.sigs.count("key1:signature1") == 1);
    REQUIRE(info2.sigs.count("key2:signature2") == 1);
  }
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST_CASE("narinfo: format error handling", "[store][narinfo][format]") {
  auto config = make_store_config();

  SECTION("invalid NarSize (non-numeric) throws") {
    auto input = std::string{"store_path_t: " + make_store_path("test") +
                             "\n"
                             "URL: nar/test.nar\n"
                             "NarHash: sha256:" +
                             std::string(VALID_SHA256_HEX) +
                             "\n"
                             "NarSize: not-a-number\n"};
    REQUIRE_THROWS_AS(nix::nar_info_t(config, input, "test.narinfo"), nix::Error);
  }

  SECTION("invalid FileSize (non-numeric) throws") {
    auto input = make_minimal_narinfo() + "FileSize: invalid\n";
    REQUIRE_THROWS_AS(nix::nar_info_t(config, input, "test.narinfo"), nix::Error);
  }

  SECTION("invalid hash format throws") {
    auto input = std::string{"store_path_t: " + make_store_path("test") +
                             "\n"
                             "URL: nar/test.nar\n"
                             "NarHash: notahash\n"
                             "NarSize: 1234\n"};
    REQUIRE_THROWS_AS(nix::nar_info_t(config, input, "test.narinfo"), nix::Error);
  }

  SECTION("line without colon throws") {
    auto input = make_minimal_narinfo() + "InvalidLineWithoutColon\n";
    REQUIRE_THROWS_AS(nix::nar_info_t(config, input, "test.narinfo"), nix::Error);
  }
}

// =============================================================================
// Serialization Format Tests
// =============================================================================

TEST_CASE("narinfo: serialization format verification", "[store][narinfo][format]") {
  auto config = make_store_config();

  auto path = config.parseStorePath(make_store_path("test-pkg"));
  auto nar_hash = nix::Hash::parse_any_prefixed("sha256:" + std::string(VALID_SHA256_HEX));
  auto file_hash = nix::Hash::parse_any_prefixed("sha256:" + std::string(VALID_SHA256_HEX));

  nix::nar_info_t info(config, path, nar_hash);
  info.url = "nar/test.nar.xz";
  info.compression = "xz";
  info.fileHash = file_hash;
  info.file_size = 1000;
  info.nar_size = 2000;
  info.references.insert(nix::store_path_t(std::string(VALID_HASH_PART) + "-dep"));
  info.sigs.insert("mykey:mysignature");

  auto output = info.to_string(config);

  SECTION("output contains required fields") {
    REQUIRE(output.find("store_path_t:") != std::string::npos);
    REQUIRE(output.find("URL:") != std::string::npos);
    REQUIRE(output.find("Compression:") != std::string::npos);
    REQUIRE(output.find("NarHash:") != std::string::npos);
    REQUIRE(output.find("NarSize:") != std::string::npos);
  }

  SECTION("output contains optional fields when set") {
    REQUIRE(output.find("FileHash:") != std::string::npos);
    REQUIRE(output.find("FileSize:") != std::string::npos);
    REQUIRE(output.find("References:") != std::string::npos);
    REQUIRE(output.find("Sig:") != std::string::npos);
  }

  SECTION("each line ends with newline") {
    // Count lines vs newlines
    size_t newline_count = 0;
    for (char c : output) {
      if (c == '\n')
        newline_count++;
    }
    // Each field should end with newline, and output should end with newline
    REQUIRE(newline_count >= 7); // At least the required + optional fields
    REQUIRE(output.back() == '\n');
  }

  SECTION("field format is 'Key: value\\n'") {
    // Verify the colon-space separator pattern
    REQUIRE(output.find(": ") != std::string::npos);
    // Verify no field has double colons (malformed)
    REQUIRE(output.find(":: ") == std::string::npos);
  }
}
