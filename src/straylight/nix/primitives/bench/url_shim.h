// straylight::nix::primitives::bench::url_shim
//
// URL parsing shim for benchmarking different backends
//
// This provides a unified interface to benchmark:
//   1. boost::url (RFC 3986, current nix backend)
//   2. ada (WHATWG, Node.js/Cloudflare)
//   3. straylight::nix::primitives (our wrapper)
//
// Each backend is called directly to measure true parsing performance.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace straylight::nix::primitives::bench {

// ─────────────────────────────────────────────────────────────────────────────
// Simplified URL result (minimal allocation overhead for benchmarking)
// ─────────────────────────────────────────────────────────────────────────────

struct url_result {
  std::string scheme;
  std::string host;
  std::optional<std::uint16_t> port;
  std::string path;
  std::string query;
  std::string fragment;

  // Valid parse?
  bool valid = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Backend parsers (each implemented in url_shim.cpp)
// ─────────────────────────────────────────────────────────────────────────────

namespace boost_backend {
// Parse using boost::url
[[nodiscard]] url_result parse(std::string_view url);

// Quick validation only (no allocation)
[[nodiscard]] bool can_parse(std::string_view url) noexcept;
} // namespace boost_backend

namespace ada_backend {
// Parse using ada (WHATWG)
[[nodiscard]] url_result parse(std::string_view url);

// Quick validation only (no allocation)
[[nodiscard]] bool can_parse(std::string_view url) noexcept;
} // namespace ada_backend

namespace straylight_backend {
// Parse using straylight::nix::primitives::parse()
[[nodiscard]] url_result parse(std::string_view url);

// Quick validation only
[[nodiscard]] bool can_parse(std::string_view url) noexcept;
} // namespace straylight_backend

namespace straylight_fast_backend {
// Parse using straylight::nix::primitives::url_view (lazy/zero-copy)
[[nodiscard]] url_result parse(std::string_view url);

// Quick validation only
[[nodiscard]] bool can_parse(std::string_view url) noexcept;
} // namespace straylight_fast_backend

// ─────────────────────────────────────────────────────────────────────────────
// Workload generation for realistic benchmarks
// ─────────────────────────────────────────────────────────────────────────────

// URL categories that Nix actually encounters
enum class url_category : std::uint8_t {
  // Fetcher URLs
  github_https, // https://github.com/user/repo
  github_ssh,   // git@github.com:user/repo (needs normalization)
  gitlab_https, // https://gitlab.com/user/repo
  sourcehut,    // https://git.sr.ht/~user/repo

  // Store URLs
  http_cache, // https://cache.nixos.org/hash.narinfo
  s3_bucket,  // s3://bucket/path?region=us-east-1
  file_local, // file:///nix/store/hash-name

  // Flake references
  flake_github,  // github:NixOS/nixpkgs/master
  flake_path,    // path:/home/user/project
  flake_tarball, // tarball+https://example.com/archive.tar.gz

  // Edge cases
  ipv6_literal, // http://[::1]:8080/path
  unicode_path, // https://example.com/path%20with%20spaces
  long_query,   // URL with many query parameters
  deep_path,    // URL with deeply nested path
};

// Generate a sample URL for a category
[[nodiscard]] std::string generate_url(url_category cat);

// Generate a batch of URLs for benchmarking
[[nodiscard]] std::vector<std::string>
generate_workload(const std::vector<std::pair<url_category, std::size_t>>& distribution);

// Pre-built workloads representing common usage patterns
namespace workloads {

// Typical Nix build: mostly GitHub + cache URLs
[[nodiscard]] std::vector<std::string> nix_build();

// Flake evaluation: many flake refs
[[nodiscard]] std::vector<std::string> flake_eval();

// Cache operations: binary cache URLs
[[nodiscard]] std::vector<std::string> cache_ops();

// Edge cases: stress test with difficult URLs
[[nodiscard]] std::vector<std::string> edge_cases();

// Mixed realistic workload
[[nodiscard]] std::vector<std::string> mixed_realistic();

} // namespace workloads

} // namespace straylight::nix::primitives::bench
