// straylight::nix::primitives
//
// Build configuration for primitive types

#pragma once

#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// URL Backend Selection
// ─────────────────────────────────────────────────────────────────────────────
//
// STRAYLIGHT_URL_BACKEND_ADA    - Use ada (WHATWG URL Standard)
// STRAYLIGHT_URL_BACKEND_BOOST  - Use boost::url (RFC 3986)
//
// Default: ada (faster, WHATWG-compliant, used by Node.js/Cloudflare)

#if !defined(STRAYLIGHT_URL_BACKEND_ADA) && !defined(STRAYLIGHT_URL_BACKEND_BOOST)
#  define STRAYLIGHT_URL_BACKEND_ADA 1
#endif

#if defined(STRAYLIGHT_URL_BACKEND_ADA) && defined(STRAYLIGHT_URL_BACKEND_BOOST)
#  error "Cannot define both STRAYLIGHT_URL_BACKEND_ADA and STRAYLIGHT_URL_BACKEND_BOOST"
#endif

namespace straylight::nix::primitives {

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time backend introspection
// ─────────────────────────────────────────────────────────────────────────────

enum class url_backend : std::uint8_t {
  ada,
  boost,
};

#if defined(STRAYLIGHT_URL_BACKEND_ADA)
inline constexpr url_backend active_url_backend = url_backend::ada;
inline constexpr const char* url_backend_name = "ada";
inline constexpr bool url_backend_is_whatwg = true;
#else
inline constexpr url_backend active_url_backend = url_backend::boost;
inline constexpr const char* url_backend_name = "boost::url";
inline constexpr bool url_backend_is_whatwg = false;
#endif

} // namespace straylight::nix::primitives
