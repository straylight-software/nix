#include "nix/util/url.h"

#include <boost/url.hpp>

#include "nix/util/canon-path.h"
#include "nix/util/file-system.h"
#include "nix/util/split.h"
#include "nix/util/strings-inline.h"
#include "nix/util/url-parts.h"
#include "nix/util/util.h"

namespace nix {

std::regex ref_regex(ref_regex_s, std::regex::ECMAScript);
std::regex rev_regex(rev_regex_s, std::regex::ECMAScript);

parsed_url_t::authority_t parsed_url_t::authority_t::parse(std::string_view encoded_authority) {
  auto parsed = boost::urls::parse_authority(encoded_authority);
  if (!parsed)
    throw BadURL("invalid URL authority: '%s': %s", encoded_authority, parsed.error().message());

  auto host_type = [&]() {
    switch (parsed->host_type()) {
      case boost::urls::host_type::ipv4:
        return host_type_t::ipv4;
      case boost::urls::host_type::ipv6:
        return host_type_t::ipv6;
      case boost::urls::host_type::ipvfuture:
        return host_type_t::ipv_future;
      case boost::urls::host_type::none:
      case boost::urls::host_type::name:
        return host_type_t::name;
    }
    unreachable();
  }();

  auto port = [&]() -> std::optional<uint16_t> {
    if (!parsed->has_port() || parsed->port() == "")
      return std::nullopt;
    /* If the port number is non-zero and representable. */
    if (auto portNumber = parsed->port_number())
      return portNumber;
    throw BadURL("port '%s' is invalid", parsed->port());
  }();

  authority_t result;
  result.set_host_type(host_type);
  result.set_host(std::string(parsed->host_address()));
  result.set_user(parsed->has_userinfo() ? std::optional<std::string>(std::string(parsed->user()))
                                         : std::nullopt);
  result.set_password(parsed->has_password()
                          ? std::optional<std::string>(std::string(parsed->password()))
                          : std::nullopt);
  result.set_port(port);
  return result;
}

std::ostream& operator<<(std::ostream& os, const parsed_url_t::authority_t& self) {
  if (self.user()) {
    os << percent_encode(*self.user());
    if (self.password())
      os << ":" << percent_encode(*self.password());
    os << "@";
  }

  using host_type_t = parsed_url_t::authority_t::host_type_t;
  switch (self.host_type()) {
    case host_type_t::name:
      os << percent_encode(self.host());
      break;
    case host_type_t::ipv4:
      os << self.host();
      break;
    case host_type_t::ipv6:
    case host_type_t::ipv_future:
      /* Reencode percent sign for RFC4007 ScopeId literals. */
      os << "[" << percent_encode(self.host(), ":") << "]";
  }

  if (self.port())
    os << ":" << *self.port();

  return os;
}

std::string parsed_url_t::authority_t::to_string() const {
  std::ostringstream oss;
  oss << *this;
  return std::move(oss).str();
}

/**
 * Additional characters that don't need URL encoding in the fragment.
 */
static constexpr boost::urls::grammar::lut_chars extra_allowed_chars_in_fragment = " \"^";

/**
 * Additional characters that don't need URL encoding in the query.
 */
static constexpr boost::urls::grammar::lut_chars extra_allowed_chars_in_query = " \"";

static std::string percent_encode_char_set(std::string_view s, auto char_set) {
  std::string res;
  for (auto c : s) {
    if (char_set(c))
      res += percent_encode(std::string_view{&c, &c + 1});
    else
      res += c;
  }
  return res;
}

static parsed_url_t from_boost_url_view(boost::urls::url_view url, bool lenient);

parsed_url_t parse_url(std::string_view url, bool lenient) try {
  /* Account for several non-standard properties of nix urls (for back-compat):
   *  - Allow unescaped spaces ' ' and '"' characters in queries.
   *  - Allow '"', ' ' and '^' characters in the fragment component.
   * We could write our own grammar for this, but fixing it up here seems
   * more concise, since the deviation is rather minor.
   *
   * If `!lenient` don't bother initializing, because we can just
   * parse `url` directly`.
   */
  std::string fixed_encoded_url;

  if (lenient) {
    fixed_encoded_url = [&] {
      std::string fixed;
      std::string_view view = url;

      if (auto before_query = split_prefix_to(view, '?')) {
        fixed += *before_query;
        fixed += '?';
        auto fragment_start = view.find('#');
        auto query_view = view.substr(0, fragment_start);
        auto fixed_query = percent_encode_char_set(query_view, extra_allowed_chars_in_query);
        fixed += fixed_query;
        view.remove_prefix(std::min(fragment_start, view.size()));
      }

      if (auto before_fragment = split_prefix_to(view, '#')) {
        fixed += *before_fragment;
        fixed += '#';
        auto fixed_fragment = percent_encode_char_set(view, extra_allowed_chars_in_fragment);
        fixed += fixed_fragment;
        return fixed;
      }

      fixed += view;
      return fixed;
    }();
  }

  return from_boost_url_view(boost::urls::url_view(lenient ? fixed_encoded_url : url), lenient);
} catch (boost::system::system_error& e) {
  throw BadURL("'%s' is not a valid URL: %s", url, e.code().message());
}

static parsed_url_t from_boost_url_view(boost::urls::url_view url_view, bool lenient) {
  if (!url_view.has_scheme())
    throw BadURL("'%s' doesn't have a scheme", url_view.buffer());

  auto scheme = url_view.scheme();
  auto authority = [&]() -> std::optional<parsed_url_t::authority_t> {
    if (url_view.has_authority())
      return parsed_url_t::authority_t::parse(url_view.authority().buffer());
    return std::nullopt;
  }();

  /* 3.2.2. Host (RFC3986):
   * If the URI scheme defines a default for host, then that default
   * applies when the host subcomponent is undefined or when the
   * registered name is empty (zero length).  For example, the "file" URI
   * scheme is defined so that no authority, an empty host, and
   * "localhost" all mean the end-user's machine, whereas the "http"
   * scheme considers a missing authority or empty host invalid. */
  auto transport_is_file = parse_url_scheme(scheme).transport() == "file";
  if (authority && authority->host().size() && transport_is_file)
    throw BadURL("file:// URL '%s' has unexpected authority '%s'", url_view.buffer(), *authority);

  auto fragment = url_view.fragment(); /* Does pct-decoding */

  boost::core::string_view encoded_path = url_view.encoded_path();
  if (transport_is_file && encoded_path.empty())
    encoded_path = "/";

  auto path = std::views::transform(split_string<std::vector<std::string_view>>(encoded_path, "/"),
                                    percent_decode) |
              std::ranges::to<std::vector<std::string>>();

  /* Get the raw query. Store URI supports smuggling doubly nested queries, where
     the inner &/? are pct-encoded. */
  auto query = std::string_view(url_view.encoded_query());

  parsed_url_t result;
  result.set_scheme(scheme);
  result.set_authority(authority);
  result.set_path(std::move(path));
  result.set_query(decode_query(query, lenient));
  result.set_fragment(std::string(fragment));
  return result;
}

parsed_url_t parse_url_relative(std::string_view url_s, const parsed_url_t& base) try {
  boost::urls::url resolved;

  try {
    resolved.set_scheme(base.scheme());
    if (base.authority()) {
      const auto& authority = *base.authority();
      resolved.set_host_address(authority.host());
      if (authority.user())
        resolved.set_user(*authority.user());
      if (authority.password())
        resolved.set_password(*authority.password());
      if (authority.port())
        resolved.set_port_number(*authority.port());
    }
    resolved.set_encoded_path(encode_url_path(base.path()));
    resolved.set_encoded_query(encode_query(base.query()));
    resolved.set_fragment(base.fragment());
  } catch (boost::system::system_error& e) {
    throw BadURL("'%s' is not a valid URL: %s", base.to_string(), e.code().message());
  }

  boost::urls::url_view url;
  try {
    url = url_s;
    resolved.resolve(url).value();
  } catch (boost::system::system_error& e) {
    throw BadURL("'%s' is not a valid URL: %s", url_s, e.code().message());
  }

  auto ret = from_boost_url_view(resolved, /*lenient=*/false);

  /* Hack: Boost `url_view` supports Zone IDs, but `url` does not.
     Just manually take the authority from the original URL to work
     around it. See https://github.com/boostorg/url/issues/919 for
     details. */
  if (!url.has_authority()) {
    ret.set_authority(base.authority());
  }

  /* Hack, work around fragment of base URL improperly being preserved
     https://github.com/boostorg/url/issues/920 */
  ret.set_fragment(url.has_fragment() ? std::string{url.fragment()} : "");

  return ret;
} catch (BadURL& e) {
  e.add_trace({}, "while resolving possibly-relative url '%s' against base URL '%s'", url_s, base);
  throw;
}

std::string percent_decode(std::string_view in) {
  auto pct_view = boost::urls::make_pct_string_view(in);
  if (pct_view.has_value())
    return pct_view->decode();
  auto error = pct_view.error();
  throw BadURL("invalid URI parameter '%s': %s", in, error.message());
}

std::string percent_encode(std::string_view s, std::string_view keep) {
  return boost::urls::encode(
      s, [keep](char c) { return boost::urls::unreserved_chars(c) || keep.find(c) != keep.npos; });
}

string_map_t decode_query(std::string_view query, bool lenient) try {
  /* When `lenient = true`, for back-compat unescaped characters are allowed. */
  std::string fixed_encoded_query;
  if (lenient) {
    fixed_encoded_query = percent_encode_char_set(query, extra_allowed_chars_in_query);
  }

  string_map_t result;

  auto encoded_query = boost::urls::params_encoded_view(lenient ? fixed_encoded_query : query);
  for (auto&& [key, value, value_specified] : encoded_query) {
    if (!value_specified) {
      warn("dubious URI query '%s' is missing equal sign '%s', ignoring", std::string_view(key),
           "=");
      continue;
    }

    result.emplace(key.decode(), value.decode());
  }

  return result;
} catch (boost::system::system_error& e) {
  throw BadURL("invalid URI query '%s': %s", query, e.code().message());
}

const static std::string allowed_in_query = ":@/?";
const static std::string allowed_in_path = ":@";

std::string encode_url_path(std::span<const std::string> url_path) {
  std::vector<std::string> encoded_path;
  for (auto& p : url_path)
    encoded_path.push_back(percent_encode(p, allowed_in_path));
  return concat_strings_sep("/", encoded_path);
}

std::string encode_query(const string_map_t& ss) {
  std::string res;
  bool first = true;
  for (auto& [name, value] : ss) {
    if (!first)
      res += '&';
    first = false;
    res += percent_encode(name, allowed_in_query);
    res += '=';
    res += percent_encode(value, allowed_in_query);
  }
  return res;
}

Path render_url_path_ensure_legal(const std::vector<std::string>& url_path) {
  for (const auto& comp : url_path) {
    /* This is only really valid for UNIX. Windows has more restrictions. */
    if (comp.contains('/'))
      throw BadURL("URL path component '%s' contains '/', which is not allowed in file names",
                   comp);
    if (comp.contains(char(0))) {
      using namespace std::string_view_literals;
      auto str = replace_strings(comp, "\0"sv, "␀"sv);
      throw BadURL("URL path component '%s' contains NUL byte which is not allowed", str);
    }
  }

  return concat_strings_sep("/", url_path);
}

std::string parsed_url_t::render_path(bool encode) const {
  if (encode)
    return encode_url_path(path_);
  return concat_strings_sep("/", path_);
}

std::string parsed_url_t::render_authority_and_path() const {
  std::string res;
  /* The following assertions correspond to 3.3. Path [rfc3986]. URL parser
     will never violate these properties, but hand-constructed ParsedURLs might. */
  if (authority_.has_value()) {
    /* If a URI contains an authority component, then the path component
       must either be empty or begin with a slash ("/") character. */
    assert(path_.empty() || path_.front().empty());
    res += authority_->to_string();
  } else if (std::ranges::equal(std::views::take(path_, 3), std::views::repeat("", 3))) {
    /* If a URI does not contain an authority component, then the path cannot begin
       with two slash characters ("//") */
    unreachable();
  }
  res += encode_url_path(path_);
  return res;
}

std::string parsed_url_t::to_string() const {
  std::string res;
  res += scheme_;
  res += ":";
  if (authority_.has_value())
    res += "//";
  res += render_authority_and_path();
  if (!query_.empty()) {
    res += "?";
    res += encode_query(query_);
  }
  if (!fragment_.empty()) {
    res += "#";
    res += percent_encode(fragment_);
  }
  return res;
}

std::ostream& operator<<(std::ostream& os, const parsed_url_t& url) {
  os << url.to_string();
  return os;
}

parsed_url_t parsed_url_t::canonicalise() {
  parsed_url_t res(*this);
  res.set_path(split_string<std::vector<std::string>>(canon_path_t(render_path()).abs(), "/"));
  return res;
}

/**
 * Parse a URL scheme of the form '(applicationScheme\+)?transportScheme'
 * into a tuple '(applicationScheme, transportScheme)'
 *
 * > parse_url_scheme("http") == parsed_url_scheme_t{ {}, "http"}
 * > parse_url_scheme("tarball+http") == parsed_url_scheme_t{ {"tarball"}, "http"}
 */
parsed_url_scheme_t parse_url_scheme(std::string_view scheme) {
  auto application = split_prefix_to(scheme, '+');
  auto transport = scheme;
  parsed_url_scheme_t result;
  result.set_application(application);
  result.set_transport(transport);
  return result;
}

parsed_url_t fix_git_url(std::string url) {
  std::regex scp_regex("([^/]*)@(.*):(.*)");
  if (!has_prefix(url, "/") && std::regex_match(url, scp_regex))
    url = std::regex_replace(url, scp_regex, "ssh://$1@$2/$3");
  if (!has_prefix(url, "file:") && !has_prefix(url, "git+file:") &&
      url.find("://") == std::string::npos) {
    parsed_url_t result;
    result.set_scheme("file");
    result.set_authority(parsed_url_t::authority_t{});
    result.set_path(split_string<std::vector<std::string>>(url, "/"));
    return result;
  }
  auto parsed = parse_url(url);
  // Drop the superfluous "git+" from the scheme.
  auto scheme = parse_url_scheme(parsed.scheme());
  if (scheme.application() == "git")
    parsed.set_scheme(std::string(scheme.transport()));
  return parsed;
}

// https://www.rfc-editor.org/rfc/rfc3986#section-3.1
bool is_valid_scheme_name(std::string_view s) {
  const static std::string scheme_name_regex = "(?:[a-z][a-z0-9+.-]*)";
  static std::regex regex(scheme_name_regex, std::regex::ECMAScript);

  return std::regex_match(s.begin(), s.end(), regex, std::regex_constants::match_default);
}

std::ostream& operator<<(std::ostream& os, const verbatim_url_t& url) {
  os << url.to_string();
  return os;
}

std::optional<std::string> verbatim_url_t::last_path_segment() const {
  try {
    auto parsed_url = parsed();
    auto segments = parsed_url.path_segments(/*skip_empty=*/true);
    if (std::ranges::empty(segments))
      return std::nullopt;
    return segments.back();
  } catch (BadURL&) {
    // Fall back to baseNameOf for unparsable URLs
    auto name = base_name_of(to_string());
    if (name.empty())
      return std::nullopt;
    return std::string{name};
  }
}

} // namespace nix
