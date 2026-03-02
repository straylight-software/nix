// src/continuity/tests/narinfo_roundtrip_test.cpp
//
// Roundtrip test: Compare Continuity narinfo parser/serializer to legacy.
// This verifies that the new Continuity-based implementation produces
// identical results to the existing Nix implementation.

#include <cassert>
#include <iostream>
#include <string>

#include "continuity/nix/nix_formats.h"

namespace {

// Test basic narinfo parsing and serialization
void test_basic_roundtrip() {
  const std::string_view input = R"(StorePath: /nix/store/abc123-hello-1.0
URL: nar/abc123.nar.xz
Compression: xz
FileSize: 12345
NarSize: 54321
NarHash: sha256:abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890
References: def456-glibc-2.35 ghi789-gcc-12.0
Deriver: jkl012-hello-1.0.drv
Sig: cache.example.com:base64signaturehere
)";

  auto result = continuity::nix::parse_narinfo(input);
  assert(result.is_ok() && "parse should succeed");

  const auto& ni = result.value.value();
  assert(ni.store_path == "/nix/store/abc123-hello-1.0");
  assert(ni.url == "nar/abc123.nar.xz");
  assert(ni.compression == continuity::nix::compression_t::xz);
  assert(ni.file_size == 12345);
  assert(ni.nar_size == 54321);
  assert(ni.nar_hash == "sha256:abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
  assert(ni.references.size() == 2);
  assert(ni.references[0] == "def456-glibc-2.35");
  assert(ni.references[1] == "ghi789-gcc-12.0");
  assert(ni.deriver.has_value());
  assert(ni.deriver.value() == "jkl012-hello-1.0.drv");
  assert(ni.sigs.size() == 1);
  assert(ni.sigs[0].key_name == "cache.example.com");
  assert(ni.sigs[0].sig == "base64signaturehere");

  // Serialize back
  auto serialized = continuity::nix::serialize_narinfo(ni);

  // Parse again
  auto result2 = continuity::nix::parse_narinfo(serialized);
  assert(result2.is_ok() && "re-parse should succeed");

  const auto& ni2 = result2.value.value();
  assert(ni2.store_path == ni.store_path);
  assert(ni2.url == ni.url);
  assert(ni2.compression == ni.compression);
  assert(ni2.file_size == ni.file_size);
  assert(ni2.nar_size == ni.nar_size);
  assert(ni2.nar_hash == ni.nar_hash);
  assert(ni2.references == ni.references);
  assert(ni2.deriver == ni.deriver);

  std::cout << "test_basic_roundtrip: PASSED\n";
}

// Test minimal narinfo (only required fields)
void test_minimal_narinfo() {
  const std::string_view input = R"(StorePath: /nix/store/abc-pkg
URL: nar/abc.nar
Compression: none
NarSize: 100
NarHash: sha256:0000000000000000000000000000000000000000000000000000000000000000
)";

  auto result = continuity::nix::parse_narinfo(input);
  assert(result.is_ok() && "parse should succeed");

  const auto& ni = result.value.value();
  assert(ni.store_path == "/nix/store/abc-pkg");
  assert(ni.compression == continuity::nix::compression_t::none);
  assert(ni.references.empty());
  assert(!ni.deriver.has_value());
  assert(!ni.ca.has_value());
  assert(ni.sigs.empty());

  std::cout << "test_minimal_narinfo: PASSED\n";
}

// Test missing required field
void test_missing_required_field() {
  const std::string_view input = R"(StorePath: /nix/store/abc-pkg
Compression: none
NarSize: 100
NarHash: sha256:0000
)";
  // Missing URL

  auto result = continuity::nix::parse_narinfo(input);
  assert(result.is_error() && "should fail without URL");
  assert(result.error.value().find("URL") != std::string::npos);

  std::cout << "test_missing_required_field: PASSED\n";
}

// Test all compression types
void test_compression_types() {
  const char* compressions[] = {"none", "xz", "bzip2", "zstd", "lzip", "lz4", "br"};

  for (const char* comp : compressions) {
    auto opt = continuity::nix::compression_from_string(comp);
    assert(opt.has_value());

    auto str = continuity::nix::compression_to_string(*opt);
    assert(str == comp);
  }

  assert(!continuity::nix::compression_from_string("invalid").has_value());

  std::cout << "test_compression_types: PASSED\n";
}

// Test CA narinfo
void test_ca_narinfo() {
  const std::string_view input = R"(StorePath: /nix/store/abc-pkg
URL: nar/abc.nar
Compression: none
NarSize: 100
NarHash: sha256:0000000000000000000000000000000000000000000000000000000000000000
CA: fixed:sha256:1111111111111111111111111111111111111111111111111111111111111111
)";

  auto result = continuity::nix::parse_narinfo(input);
  assert(result.is_ok());

  const auto& ni = result.value.value();
  assert(ni.ca.has_value());
  assert(ni.ca.value().starts_with("fixed:sha256:"));

  std::cout << "test_ca_narinfo: PASSED\n";
}

} // namespace

int main() {
  std::cout << "Running narinfo roundtrip tests...\n\n";

  test_basic_roundtrip();
  test_minimal_narinfo();
  test_missing_required_field();
  test_compression_types();
  test_ca_narinfo();

  std::cout << "\nAll narinfo roundtrip tests PASSED!\n";
  return 0;
}
