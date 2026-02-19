// straylight::nix::primitives::url
//
// URL parsing implementation with ada/boost backend

#include "url.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <sstream>

#if defined(STRAYLIGHT_URL_BACKEND_ADA)
#  include <ada.h>
#else
#  include <boost/url.hpp>
#endif

namespace straylight::nix::primitives {

// ─────────────────────────────────────────────────────────────────────────────
// Hex encoding tables
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr std::array<char, 16> kHexDigits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                             '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

constexpr int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Percent encoding
// ─────────────────────────────────────────────────────────────────────────────

std::string percent_decode(std::string_view input) {
  std::string result;
  result.reserve(input.size());

  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size()) {
      int hi = hex_value(input[i + 1]);
      int lo = hex_value(input[i + 2]);
      if (hi >= 0 && lo >= 0) {
        result.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    result.push_back(input[i]);
  }

  return result;
}

std::string percent_encode(std::string_view input, std::string_view keep) {
  std::string result;
  result.reserve(input.size());

  for (char c : input) {
    if (is_unreserved(c) || keep.find(c) != std::string_view::npos) {
      result.push_back(c);
    } else {
      result.push_back('%');
      result.push_back(kHexDigits[static_cast<unsigned char>(c) >> 4]);
      result.push_back(kHexDigits[static_cast<unsigned char>(c) & 0x0F]);
    }
  }

  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Zone ID extraction for IPv6 (ada doesn't support RFC 6874)
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

zone_id_extraction extract_zone_id(std::string_view url) {
  // Look for IPv6 address with zone ID: [fe80::1%25eth0]
  // The %25 is percent-encoded '%'
  auto bracket_start = url.find('[');
  if (bracket_start == std::string_view::npos) {
    return {std::string(url), ""};
  }

  auto bracket_end = url.find(']', bracket_start);
  if (bracket_end == std::string_view::npos) {
    return {std::string(url), ""};
  }

  auto ipv6_part = url.substr(bracket_start + 1, bracket_end - bracket_start - 1);
  auto zone_pos = ipv6_part.find("%25");
  if (zone_pos == std::string_view::npos) {
    return {std::string(url), ""};
  }

  // Extract zone ID
  std::string zone_id(ipv6_part.substr(zone_pos + 3));

  // Rebuild URL without zone ID
  std::string clean_url;
  clean_url.reserve(url.size());
  clean_url.append(url.substr(0, bracket_start + 1 + zone_pos));
  clean_url.push_back(']');
  clean_url.append(url.substr(bracket_end + 1));

  return {std::move(clean_url), std::move(zone_id)};
}

void restore_zone_id(authority& auth, std::string_view zone_id) {
  if (zone_id.empty() || auth.type != host_type::ipv6) {
    return;
  }
  // Append zone ID to host: "::1" -> "::1%eth0"
  auth.host.push_back('%');
  auth.host.append(zone_id);
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// authority
// ─────────────────────────────────────────────────────────────────────────────

std::string authority::to_string() const {
  std::ostringstream os;

  if (user) {
    os << percent_encode(*user);
    if (password) {
      os << ':' << percent_encode(*password);
    }
    os << '@';
  }

  switch (type) {
    case host_type::ipv6:
    case host_type::ipv_future:
      os << '[' << host << ']';
      break;
    default:
      os << percent_encode(host);
      break;
  }

  if (port) {
    os << ':' << *port;
  }

  return os.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// url
// ─────────────────────────────────────────────────────────────────────────────

std::string url::to_string() const {
  std::ostringstream os;

  os << scheme << ':';

  if (auth) {
    os << "//" << auth->to_string();
  }

  os << render_path(true);

  if (!query.empty()) {
    os << '?' << encode_query(query);
  }

  if (!fragment.empty()) {
    os << '#' << percent_encode(fragment);
  }

  return os.str();
}

std::string url::render_path(bool encode) const {
  if (path.empty()) {
    return "";
  }

  std::ostringstream os;
  bool first = true;
  for (const auto& segment : path) {
    if (!first) {
      os << '/';
    }
    first = false;
    os << (encode ? percent_encode(segment, ":@") : segment);
  }

  return os.str();
}

url url::canonicalize() const {
  url result = *this;

  std::vector<std::string> canonical;
  for (const auto& seg : path) {
    if (seg == ".") {
      continue;
    }
    if (seg == "..") {
      if (!canonical.empty() && canonical.back() != "") {
        canonical.pop_back();
      }
    } else {
      canonical.push_back(seg);
    }
  }

  result.path = std::move(canonical);
  return result;
}

bool url::is_special_scheme() const {
  auto parts = parse_scheme(scheme);
  return primitives::is_special_scheme(parts.transport);
}

// ─────────────────────────────────────────────────────────────────────────────
// Query string utilities
// ─────────────────────────────────────────────────────────────────────────────

query_params parse_query(std::string_view query) {
  query_params result;

  while (!query.empty()) {
    auto amp_pos = query.find('&');
    auto pair = query.substr(0, amp_pos);
    query = (amp_pos == std::string_view::npos) ? "" : query.substr(amp_pos + 1);

    if (pair.empty()) {
      continue;
    }

    auto eq_pos = pair.find('=');
    if (eq_pos == std::string_view::npos) {
      // Key without value - skip (like Nix does)
      continue;
    }

    result.emplace_back(percent_decode(pair.substr(0, eq_pos)),
                        percent_decode(pair.substr(eq_pos + 1)));
  }

  return result;
}

std::string encode_query(const query_params& params) {
  std::ostringstream os;
  bool first = true;

  for (const auto& [key, value] : params) {
    if (!first) {
      os << '&';
    }
    first = false;
    os << percent_encode(key, ":@/?") << '=' << percent_encode(value, ":@/?");
  }

  return os.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// Git URL normalization
// ─────────────────────────────────────────────────────────────────────────────

std::string normalize_git_url(std::string_view input) {
  // SCP-style: user@host:path -> ssh://user@host/path
  if (input.find("://") == std::string_view::npos) {
    auto at_pos = input.find('@');
    auto colon_pos = input.find(':');

    if (at_pos != std::string_view::npos && colon_pos != std::string_view::npos &&
        at_pos < colon_pos && !input.starts_with('/')) {
      std::string result = "ssh://";
      result.append(input.substr(0, colon_pos));
      result.push_back('/');
      result.append(input.substr(colon_pos + 1));
      return result;
    }
  }

  return std::string(input);
}

// ─────────────────────────────────────────────────────────────────────────────
// Backend-specific parsing
// ─────────────────────────────────────────────────────────────────────────────

#if defined(STRAYLIGHT_URL_BACKEND_ADA)

// ═══════════════════════════════════════════════════════════════════════════
// ADA (WHATWG) BACKEND
// ═══════════════════════════════════════════════════════════════════════════

namespace {

host_type ada_host_type(ada::url_host_type t) {
  switch (t) {
    case ada::url_host_type::IPV4:
      return host_type::ipv4;
    case ada::url_host_type::IPV6:
      return host_type::ipv6;
    default:
      return host_type::name;
  }
}

url from_ada(const ada::url_aggregator& parsed, std::string_view zone_id) {
  url result;

  // Scheme (ada includes trailing ':', remove it)
  auto scheme_str = parsed.get_protocol();
  if (!scheme_str.empty() && scheme_str.back() == ':') {
    result.scheme = std::string(scheme_str.substr(0, scheme_str.size() - 1));
  } else {
    result.scheme = std::string(scheme_str);
  }

  // Authority
  if (parsed.has_hostname()) {
    authority auth;

    // Use get_hostname() not get_host() (host includes port in ada)
    auth.host = std::string(parsed.get_hostname());
    auth.type = ada_host_type(parsed.host_type);

    // Restore zone ID if we extracted one
    detail::restore_zone_id(auth, zone_id);

    if (parsed.has_non_empty_username()) {
      auth.user = std::string(parsed.get_username());
    }
    if (parsed.has_non_empty_password()) {
      auth.password = std::string(parsed.get_password());
    }

    auto port_str = parsed.get_port();
    if (!port_str.empty()) {
      std::uint16_t port_val = 0;
      auto [ptr, ec] =
          std::from_chars(port_str.data(), port_str.data() + port_str.size(), port_val);
      if (ec == std::errc{}) {
        auth.port = port_val;
      }
    }

    result.auth = std::move(auth);
  }

  // Path - split and decode
  auto path_str = parsed.get_pathname();
  std::string_view path_view = path_str;

  while (!path_view.empty()) {
    auto slash_pos = path_view.find('/');
    if (slash_pos == 0) {
      result.path.emplace_back("");
      path_view = path_view.substr(1);
    } else if (slash_pos == std::string_view::npos) {
      result.path.push_back(percent_decode(path_view));
      break;
    } else {
      result.path.push_back(percent_decode(path_view.substr(0, slash_pos)));
      path_view = path_view.substr(slash_pos + 1);
    }
  }

  // Query - ada returns with leading '?', we need to decode
  auto search = parsed.get_search();
  if (!search.empty()) {
    if (search[0] == '?') {
      search = search.substr(1);
    }
    result.query = parse_query(search);
  }

  // Fragment - ada returns with leading '#'
  auto hash = parsed.get_hash();
  if (!hash.empty() && hash[0] == '#') {
    result.fragment = percent_decode(hash.substr(1));
  } else {
    result.fragment = percent_decode(hash);
  }

  return result;
}

} // namespace

bool can_parse(std::string_view input) noexcept {
  // Handle zone ID extraction for validation too
  auto [clean_url, zone_id] = detail::extract_zone_id(input);
  return ada::can_parse(clean_url);
}

parse_result parse(std::string_view input) {
  // Extract zone ID before parsing (ada rejects RFC 6874)
  auto [clean_url, zone_id] = detail::extract_zone_id(input);

  auto parsed = ada::parse<ada::url_aggregator>(clean_url);
  if (!parsed) {
    return std::unexpected(parse_error{
        .message = "invalid URL",
        .position = 0,
    });
  }

  return from_ada(*parsed, zone_id);
}

parse_result parse_lenient(std::string_view input) {
  // Ada is already more lenient (WHATWG), but we may need to handle
  // Nix-specific quirks like unencoded spaces
  // For now, just use regular parse
  return parse(input);
}

parse_result parse_relative(std::string_view input, const url& base) {
  // Convert base to string and use ada's resolution
  auto base_str = base.to_string();
  auto base_parsed = ada::parse<ada::url_aggregator>(base_str);
  if (!base_parsed) {
    return std::unexpected(parse_error{
        .message = "invalid base URL",
        .position = 0,
    });
  }

  auto resolved = ada::parse<ada::url_aggregator>(input, &*base_parsed);
  if (!resolved) {
    return std::unexpected(parse_error{
        .message = "invalid relative URL",
        .position = 0,
    });
  }

  return from_ada(*resolved, "");
}

#else // STRAYLIGHT_URL_BACKEND_BOOST

// ═══════════════════════════════════════════════════════════════════════════
// BOOST (RFC 3986) BACKEND
// ═══════════════════════════════════════════════════════════════════════════

namespace {

host_type boost_host_type(boost::urls::host_type t) {
  switch (t) {
    case boost::urls::host_type::ipv4:
      return host_type::ipv4;
    case boost::urls::host_type::ipv6:
      return host_type::ipv6;
    case boost::urls::host_type::ipvfuture:
      return host_type::ipv_future;
    default:
      return host_type::name;
  }
}

url from_boost(boost::urls::url_view parsed) {
  url result;

  result.scheme = std::string(parsed.scheme());

  if (parsed.has_authority()) {
    authority auth;
    auth.host = std::string(parsed.host());
    auth.type = boost_host_type(parsed.host_type());

    if (parsed.has_userinfo()) {
      auth.user = std::string(parsed.user());
      if (parsed.has_password()) {
        auth.password = std::string(parsed.password());
      }
    }

    if (parsed.has_port()) {
      auth.port = parsed.port_number();
    }

    result.auth = std::move(auth);
  }

  // Path
  auto path_str = parsed.path();
  std::string_view path_view = path_str;

  while (!path_view.empty()) {
    auto slash_pos = path_view.find('/');
    if (slash_pos == 0) {
      result.path.emplace_back("");
      path_view = path_view.substr(1);
    } else if (slash_pos == std::string_view::npos) {
      result.path.push_back(percent_decode(path_view));
      break;
    } else {
      result.path.push_back(percent_decode(path_view.substr(0, slash_pos)));
      path_view = path_view.substr(slash_pos + 1);
    }
  }

  // Query
  if (parsed.has_query()) {
    result.query = parse_query(parsed.query());
  }

  // Fragment
  if (parsed.has_fragment()) {
    result.fragment = std::string(parsed.fragment());
  }

  return result;
}

} // namespace

bool can_parse(std::string_view input) noexcept {
  auto parsed = boost::urls::parse_uri(input);
  return parsed.has_value();
}

parse_result parse(std::string_view input) {
  try {
    auto parsed = boost::urls::parse_uri(input);
    if (!parsed) {
      return std::unexpected(parse_error{
          .message = parsed.error().message(),
          .position = 0,
      });
    }
    return from_boost(*parsed);
  } catch (const boost::system::system_error& e) {
    return std::unexpected(parse_error{
        .message = e.code().message(),
        .position = 0,
    });
  }
}

parse_result parse_lenient(std::string_view input) {
  // Boost doesn't have lenient mode, use regular parse
  return parse(input);
}

parse_result parse_relative(std::string_view input, const url& base) {
  try {
    boost::urls::url resolved;

    // Set up base URL
    resolved.set_scheme(base.scheme);
    if (base.auth) {
      resolved.set_host(base.auth->host);
      if (base.auth->user) {
        resolved.set_user(*base.auth->user);
      }
      if (base.auth->password) {
        resolved.set_password(*base.auth->password);
      }
      if (base.auth->port) {
        resolved.set_port_number(*base.auth->port);
      }
    }

    resolved.set_path(base.render_path(true));
    resolved.set_query(encode_query(base.query));
    resolved.set_fragment(base.fragment);

    // Resolve relative URL
    boost::urls::url_view rel_view(input);
    resolved.resolve(rel_view);

    return from_boost(resolved);
  } catch (const boost::system::system_error& e) {
    return std::unexpected(parse_error{
        .message = e.code().message(),
        .position = 0,
    });
  }
}

#endif // STRAYLIGHT_URL_BACKEND_*

} // namespace straylight::nix::primitives
