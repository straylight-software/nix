// straylight::nix::primitives::url_fast
//
// Zero-copy / lazy URL parsing wrapper around ada
//
// Design goals:
//   - Minimal overhead over raw ada
//   - Lazy decoding (only decode when accessed)
//   - String views where possible (caller must ensure input lifetime)
//   - Fast path for common cases (no IPv6 zone ID)

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ada.h>

namespace straylight::nix::url {

// ─────────────────────────────────────────────────────────────────────────────
// url_view: Zero-copy lazy URL (views into ada's internal storage)
// ─────────────────────────────────────────────────────────────────────────────

class url_view {
public:
  // Parse a URL, returns nullopt on failure
  [[nodiscard]] static std::optional<url_view> parse(std::string_view input) noexcept;

  // Quick validation without full parse
  [[nodiscard]] static bool can_parse(std::string_view input) noexcept;

  // ─────────────────────────────────────────────────────────────────────────
  // Accessors (zero-copy, return views into ada's storage)
  // ─────────────────────────────────────────────────────────────────────────

  // Scheme without trailing ':'
  [[nodiscard]] std::string_view scheme() const noexcept;

  // Hostname (without port, without brackets for IPv6)
  [[nodiscard]] std::string_view host() const noexcept;

  // Port if present
  [[nodiscard]] std::optional<std::uint16_t> port() const noexcept;

  // Full authority string (user:pass@host:port)
  [[nodiscard]] std::string_view authority() const noexcept;

  // Path (still encoded)
  [[nodiscard]] std::string_view path_encoded() const noexcept;

  // Query string without leading '?' (still encoded)
  [[nodiscard]] std::string_view query_encoded() const noexcept;

  // Fragment without leading '#' (still encoded)
  [[nodiscard]] std::string_view fragment_encoded() const noexcept;

  // Username if present
  [[nodiscard]] std::string_view username() const noexcept;

  // Password if present
  [[nodiscard]] std::string_view password() const noexcept;

  // ─────────────────────────────────────────────────────────────────────────
  // Lazy decoded accessors (allocate on demand)
  // ─────────────────────────────────────────────────────────────────────────

  // Decoded path segments (lazy, cached)
  [[nodiscard]] const std::vector<std::string>& path() const;

  // Decoded query params (lazy, cached)
  [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& query() const;

  // Decoded fragment (lazy, cached)
  [[nodiscard]] const std::string& fragment() const;

  // ─────────────────────────────────────────────────────────────────────────
  // Serialization
  // ─────────────────────────────────────────────────────────────────────────

  // Full URL string (from ada, no allocation)
  [[nodiscard]] std::string_view href() const noexcept;

  // Convert to owned string
  [[nodiscard]] std::string to_string() const;

  // ─────────────────────────────────────────────────────────────────────────
  // Checks
  // ─────────────────────────────────────────────────────────────────────────

  [[nodiscard]] bool has_authority() const noexcept;
  [[nodiscard]] bool has_port() const noexcept;
  [[nodiscard]] bool has_query() const noexcept;
  [[nodiscard]] bool has_fragment() const noexcept;
  [[nodiscard]] bool has_credentials() const noexcept;

  // Is this a "special" scheme (http, https, ws, wss, ftp, file)?
  [[nodiscard]] bool is_special() const noexcept;

private:
  explicit url_view(ada::url_aggregator&& parsed) noexcept;

  ada::url_aggregator parsed_;

  // Lazy caches (mutable for const accessors)
  mutable std::optional<std::vector<std::string>> path_cache_;
  mutable std::optional<std::vector<std::pair<std::string, std::string>>> query_cache_;
  mutable std::optional<std::string> fragment_cache_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Inline implementations
// ─────────────────────────────────────────────────────────────────────────────

inline std::optional<url_view> url_view::parse(std::string_view input) noexcept {
  auto result = ada::parse<ada::url_aggregator>(input);
  if (!result) {
    return std::nullopt;
  }
  return url_view(std::move(*result));
}

inline bool url_view::can_parse(std::string_view input) noexcept {
  return ada::can_parse(input);
}

inline url_view::url_view(ada::url_aggregator&& parsed) noexcept : parsed_(std::move(parsed)) {}

inline std::string_view url_view::scheme() const noexcept {
  auto proto = parsed_.get_protocol();
  // Remove trailing ':'
  if (!proto.empty() && proto.back() == ':') {
    return proto.substr(0, proto.size() - 1);
  }
  return proto;
}

inline std::string_view url_view::host() const noexcept {
  return parsed_.get_hostname();
}

inline std::optional<std::uint16_t> url_view::port() const noexcept {
  if (!parsed_.has_port()) {
    return std::nullopt;
  }
  auto port_str = parsed_.get_port();
  if (port_str.empty()) {
    return std::nullopt;
  }
  std::uint16_t val = 0;
  for (char c : port_str) {
    val = static_cast<std::uint16_t>((val * 10) + (c - '0'));
  }
  return val;
}

inline std::string_view url_view::authority() const noexcept {
  return parsed_.get_host();
}

inline std::string_view url_view::path_encoded() const noexcept {
  return parsed_.get_pathname();
}

inline std::string_view url_view::query_encoded() const noexcept {
  auto q = parsed_.get_search();
  if (!q.empty() && q[0] == '?') {
    return q.substr(1);
  }
  return q;
}

inline std::string_view url_view::fragment_encoded() const noexcept {
  auto f = parsed_.get_hash();
  if (!f.empty() && f[0] == '#') {
    return f.substr(1);
  }
  return f;
}

inline std::string_view url_view::username() const noexcept {
  return parsed_.get_username();
}

inline std::string_view url_view::password() const noexcept {
  return parsed_.get_password();
}

inline std::string_view url_view::href() const noexcept {
  return parsed_.get_href();
}

inline std::string url_view::to_string() const {
  return std::string(parsed_.get_href());
}

inline bool url_view::has_authority() const noexcept {
  return parsed_.has_hostname();
}

inline bool url_view::has_port() const noexcept {
  return parsed_.has_port();
}

inline bool url_view::has_query() const noexcept {
  return parsed_.has_search();
}

inline bool url_view::has_fragment() const noexcept {
  return parsed_.has_hash();
}

inline bool url_view::has_credentials() const noexcept {
  return parsed_.has_non_empty_username() || parsed_.has_non_empty_password();
}

inline bool url_view::is_special() const noexcept {
  // Ada tracks this internally for special schemes
  auto s = scheme();
  return s == "http" || s == "https" || s == "ws" || s == "wss" || s == "ftp" || s == "file";
}

} // namespace straylight::nix::url
