// straylight::nix::primitives::url
//
// High-performance URL parsing with switchable backend (ada/boost)
//
// Design:
//   - Header-only where possible
//   - constexpr for compile-time URL validation
//   - Unified API regardless of backend
//   - Handles WHATWG vs RFC 3986 differences transparently

#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config.h"

namespace straylight::nix::primitives {

// ─────────────────────────────────────────────────────────────────────────────
// URL components (backend-agnostic value types)
// ─────────────────────────────────────────────────────────────────────────────

enum class host_type : std::uint8_t {
  name,       // Registered name (domain)
  ipv4,       // IPv4 address
  ipv6,       // IPv6 address
  ipv_future, // IPvFuture (rare)
};

struct authority {
  host_type type = host_type::name;
  std::string host;
  std::optional<std::string> user;
  std::optional<std::string> password;
  std::optional<std::uint16_t> port;

  [[nodiscard]] auto operator<=>(const authority&) const = default;

  [[nodiscard]] std::string to_string() const;
};

struct url {
  std::string scheme;
  std::optional<authority> auth;
  std::vector<std::string> path;                          // Split on '/', percent-decoded
  std::vector<std::pair<std::string, std::string>> query; // Decoded key-value pairs
  std::string fragment;

  [[nodiscard]] auto operator<=>(const url&) const = default;

  // Render back to string (re-encodes as needed)
  [[nodiscard]] std::string to_string() const;

  // Render just the path portion
  [[nodiscard]] std::string render_path(bool encode = true) const;

  // Canonicalize (remove . and .. segments)
  [[nodiscard]] url canonicalize() const;

  // Check if this is a special scheme (http, https, ws, wss, ftp, file)
  [[nodiscard]] bool is_special_scheme() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Percent encoding (constexpr where possible)
// ─────────────────────────────────────────────────────────────────────────────

// Characters that don't need encoding (RFC 3986 unreserved)
[[nodiscard]] constexpr bool is_unreserved(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '.' || c == '_' || c == '~';
}

// Decode a percent-encoded string
[[nodiscard]] std::string percent_decode(std::string_view input);

// Encode a string, optionally keeping certain characters unencoded
[[nodiscard]] std::string percent_encode(std::string_view input, std::string_view keep = "");

// ─────────────────────────────────────────────────────────────────────────────
// URL parsing
// ─────────────────────────────────────────────────────────────────────────────

struct parse_error {
  std::string message;
  std::size_t position = 0; // Approximate error position
};

using parse_result = std::expected<url, parse_error>;

// Parse a URL string
[[nodiscard]] parse_result parse(std::string_view input);

// Parse with lenient mode (allows some non-standard Nix URLs)
[[nodiscard]] parse_result parse_lenient(std::string_view input);

// Parse relative URL against a base
[[nodiscard]] parse_result parse_relative(std::string_view input, const url& base);

// Quick validation without full parse
[[nodiscard]] bool can_parse(std::string_view input) noexcept;

// ─────────────────────────────────────────────────────────────────────────────
// Scheme utilities
// ─────────────────────────────────────────────────────────────────────────────

struct scheme_parts {
  std::optional<std::string_view> application; // e.g., "git" in "git+https"
  std::string_view transport;                  // e.g., "https" in "git+https"
};

// Parse compound schemes like "git+https" or "tarball+file"
[[nodiscard]] constexpr scheme_parts parse_scheme(std::string_view scheme) noexcept {
  if (auto pos = scheme.find('+'); pos != std::string_view::npos) {
    return {
        .application = scheme.substr(0, pos),
        .transport = scheme.substr(pos + 1),
    };
  }
  return {.application = std::nullopt, .transport = scheme};
}

// Check if a scheme name is valid per RFC 3986
[[nodiscard]] constexpr bool is_valid_scheme(std::string_view s) noexcept {
  if (s.empty()) {
    return false;
  }
  // Must start with letter
  char c = s[0];
  if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
    return false;
  }
  // Rest: letter, digit, +, -, .
  for (std::size_t i = 1; i < s.size(); ++i) {
    c = s[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '+' || c == '-' || c == '.';
    if (!ok) {
      return false;
    }
  }
  return true;
}

// Special schemes per WHATWG
[[nodiscard]] constexpr bool is_special_scheme(std::string_view s) noexcept {
  return s == "http" || s == "https" || s == "ws" || s == "wss" || s == "ftp" || s == "file";
}

// Default port for special schemes (0 = no default)
[[nodiscard]] constexpr std::uint16_t default_port(std::string_view scheme) noexcept {
  if (scheme == "http" || scheme == "ws")
    return 80;
  if (scheme == "https" || scheme == "wss")
    return 443;
  if (scheme == "ftp")
    return 21;
  return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Git URL helpers (SCP-style conversion)
// ─────────────────────────────────────────────────────────────────────────────

// Convert SCP-style URLs (git@github.com:user/repo) to proper URLs
[[nodiscard]] std::string normalize_git_url(std::string_view input);

// ─────────────────────────────────────────────────────────────────────────────
// Query string utilities
// ─────────────────────────────────────────────────────────────────────────────

using query_params = std::vector<std::pair<std::string, std::string>>;

// Parse query string into key-value pairs
[[nodiscard]] query_params parse_query(std::string_view query);

// Encode query params back to string
[[nodiscard]] std::string encode_query(const query_params& params);

// ─────────────────────────────────────────────────────────────────────────────
// Backend-specific: IPv6 Zone ID handling
// ─────────────────────────────────────────────────────────────────────────────
//
// WHATWG (ada) rejects RFC 6874 zone IDs like [fe80::1%25eth0]
// We handle this by extracting before parse, re-adding after

namespace detail {

struct zone_id_extraction {
  std::string url_without_zone;
  std::string zone_id; // Empty if none
};

[[nodiscard]] zone_id_extraction extract_zone_id(std::string_view url);
void restore_zone_id(authority& auth, std::string_view zone_id);

} // namespace detail

} // namespace straylight::nix::primitives
