#pragma once
///@file

#include <ranges>
#include <span>

#include "nix/util/canon-path.h"
#include "nix/util/error.h"
#include "nix/util/split.h"
#include "nix/util/util.h"
#include "nix/util/variant-wrapper.h"

namespace nix {

/**
 * Represents a parsed RFC3986 URL.
 *
 * @note All fields are already percent decoded.
 */
class parsed_url_t {
public:
  /**
   * Parsed representation of a URL authority.
   *
   * It consists of user information, hostname and an optional port number.
   * Note that passwords in the userinfo are not yet supported and are ignored.
   *
   * @todo Maybe support passwords in userinfo part of the url for auth.
   */
  class authority_t {
  public:
    enum class host_type_t : std::uint8_t {
      name, //< Registered name (can be empty)
      ipv4,
      ipv6,
      ipv_future
    };

    [[nodiscard]] static auto parse(std::string_view encoded_authority) -> authority_t;
    auto operator<=>(const authority_t& other) const = default;
    [[nodiscard]] auto to_string() const -> std::string;
    friend auto operator<<(std::ostream& os, const authority_t& self) -> std::ostream&;

    // Accessors
    [[nodiscard]] auto host_type() const -> host_type_t { return host_type_; }
    auto set_host_type(host_type_t value) -> void { host_type_ = value; }

    [[nodiscard]] auto host() const -> const std::string& { return host_; }
    auto set_host(std::string value) -> void { host_ = std::move(value); }

    [[nodiscard]] auto user() const -> const std::optional<std::string>& { return user_; }
    auto set_user(std::optional<std::string> value) -> void { user_ = std::move(value); }

    [[nodiscard]] auto password() const -> const std::optional<std::string>& { return password_; }
    auto set_password(std::optional<std::string> value) -> void { password_ = std::move(value); }

    [[nodiscard]] auto port() const -> const std::optional<uint16_t>& { return port_; }
    auto set_port(std::optional<uint16_t> value) -> void { port_ = value; }

  private:
    /**
     * Type of the host subcomponent, as specified by rfc3986 3.2.2. Host.
     */
    host_type_t host_type_ = host_type_t::name;

    /**
     * Host subcomponent. Either a registered name or IPv{4,6,Future} literal addresses.
     *
     * ipv6 enclosing brackets are already stripped. Percent encoded characters
     * in the hostname are decoded.
     */
    std::string host_;

    /** Percent-decoded user part of the userinfo. */
    std::optional<std::string> user_;

    /**
     * Password subcomponent of the authority (if specified).
     *
     * @warning As per the rfc3986, the password syntax is deprecated,
     * but it's necessary to make the parse -> to_string roundtrip.
     * We don't use it anywhere (at least intentionally).
     * @todo Warn about unused password subcomponent.
     */
    std::optional<std::string> password_;

    /** Port subcomponent (if specified). Default value is determined by the scheme. */
    std::optional<uint16_t> port_;
  };

  // Accessors
  [[nodiscard]] auto scheme() const -> const std::string& { return scheme_; }
  auto set_scheme(std::string value) -> void { scheme_ = std::move(value); }

  [[nodiscard]] auto authority() const -> const std::optional<authority_t>& { return authority_; }
  auto set_authority(std::optional<authority_t> value) -> void { authority_ = std::move(value); }

  [[nodiscard]] auto path() const -> const std::vector<std::string>& { return path_; }
  [[nodiscard]] auto path() -> std::vector<std::string>& { return path_; }
  auto set_path(std::vector<std::string> value) -> void { path_ = std::move(value); }

  [[nodiscard]] auto query() const -> const string_map_t& { return query_; }
  [[nodiscard]] auto query() -> string_map_t& { return query_; }
  auto set_query(string_map_t value) -> void { query_ = std::move(value); }

  [[nodiscard]] auto fragment() const -> const std::string& { return fragment_; }
  auto set_fragment(std::string value) -> void { fragment_ = std::move(value); }

  /**
   * Render just the middle part of a URL, without the `//` which
   * indicates whether the authority is present.
   *
   * @note This is kind of an ad-hoc
   * operation, but it ends up coming up with some frequency, probably
   * due to the current design of `StoreReference` in `nix-store`.
   */
  [[nodiscard]] auto render_authority_and_path() const -> std::string;

  [[nodiscard]] auto to_string() const -> std::string;

  /**
   * Render the path to a string.
   *
   * @param encode Whether to percent encode path segments.
   */
  [[nodiscard]] auto render_path(bool encode = false) const -> std::string;

  auto operator<=>(const parsed_url_t& other) const noexcept = default;

  /**
   * Remove `.` and `..` path segments.
   */
  [[nodiscard]] auto canonicalise() -> parsed_url_t;

  /**
   * Get a range of path segments (the substrings separated by '/' characters).
   *
   * @param skip_empty Skip all empty path segments
   */
  [[nodiscard]] auto path_segments(bool skip_empty) const& {
    return std::views::filter(path_, [skip_empty](std::string_view segment) {
      if (skip_empty) {
        return !segment.empty();
      }
      return true;
    });
  }

private:
  std::string scheme_;

  /**
   * Optional parsed authority component of the URL.
   *
   * IMPORTANT: An empty authority (i.e. one with an empty host string) and
   * a missing authority (std::nullopt) are drastically different cases. This
   * is especially important for "file:///path/to/file" URLs defined by RFC8089.
   * The presence of the authority is indicated by `//` following the <scheme>:
   * part of the URL.
   */
  std::optional<authority_t> authority_;

  /**
   * @note Unlike Unix paths, URLs provide a way to escape path
   * separators, in the form of the `%2F` encoding of `/`. That means
   * that if one percent-decodes the path into a single string, that
   * decoding will be *lossy*, because `/` and `%2F` both become `/`.
   * The right thing to do is instead split up the path on `/`, and
   * then percent decode each part.
   *
   * For an example, the path
   * ```
   * foo/bar%2Fbaz/quux
   * ```
   * is parsed as
   * ```
   * {"foo, "bar/baz", "quux"}
   * ```
   *
   * We're doing splitting and joining that assumes the separator (`/` in this case) only goes
   * *between* elements.
   *
   * That means the parsed representation will begin with an empty
   * element to make an initial `/`, and will end with an ementy
   * element to make a trailing `/`. That means that elements of this
   * vector mostly, but *not always*, correspond to segments of the
   * path.
   *
   * Examples:
   *
   * - ```
   *   https://foo.com/bar
   *   ```
   *   has path
   *   ```
   *   {"", "bar"}
   *   ```
   *
   * - ```
   *   https://foo.com/bar/
   *   ```
   *   has path
   *   ```
   *   {"", "bar", ""}
   *   ```
   *
   * - ```
   *   https://foo.com//bar///
   *   ```
   *   has path
   *   ```
   *   {"", "", "bar", "", "", ""}
   *   ```
   *
   * - ```
   *   https://foo.com
   *   ```
   *   has path
   *   ```
   *   {""}
   *   ```
   *
   * - ```
   *   https://foo.com/
   *   ```
   *   has path
   *   ```
   *   {"", ""}
   *   ```
   *
   * - ```
   *   tel:01234
   *   ```
   *   has path `{"01234"}` (and no authority)
   *
   * - ```
   *   foo:/01234
   *   ```
   *   has path `{"", "01234"}` (and no authority)
   *
   * Note that both trailing and leading slashes are, in general,
   * semantically significant.
   *
   * For trailing slashes, the main example affecting many schemes is
   * that `../baz` resolves against a base URL different depending on
   * the presence/absence of a trailing slash:
   *
   * - `https://foo.com/bar` is `https://foo.com/baz`
   *
   * - `https://foo.com/bar/` is `https://foo.com/bar/baz`
   *
   * See `parse_url_relative` for more details.
   *
   * For leading slashes, there are some requirements to be aware of.
   *
   * - When there is an authority, the path *must* start with a leading
   *   slash. Otherwise the path will not be separated from the
   *   authority, and will not round trip though the parser:
   *
   *   ```
   *   {.scheme="https", .authority.host = "foo", .path={"bad"}}
   *   ```
   *   will render to `https://foobar`. but that would parse back as as
   *   ```
   *   {.scheme="https", .authority.host = "foobar", .path={}}
   *   ```
   *
   * - When there is no authority, the path must *not* begin with two
   *   slashes. Otherwise, there will be another parser round trip
   *   issue:
   *
   *   ```
   *   {.scheme="https", .path={"", "", "bad"}}
   *   ```
   *   will render to `https://bad`. but that would parse back as as
   *   ```
   *   {.scheme="https", .authority.host = "bad", .path={}}
   *   ```
   *
   * These invariants will be checked in `to_string` and
   * `render_authority_and_path`.
   */
  std::vector<std::string> path_;

  string_map_t query_;

  std::string fragment_;
};

auto operator<<(std::ostream& os, const parsed_url_t& url) -> std::ostream&;

make_error(BadURL, Error); // NOLINT(readability-identifier-naming)

[[nodiscard]] auto percent_decode(std::string_view in) -> std::string;
[[nodiscard]] auto percent_encode(std::string_view s, std::string_view keep = "") -> std::string;

/**
 * Get the path part of the URL as an absolute or relative Path.
 *
 * @throws if any path component contains an slash (which would have
 * been escaped `%2F` in the rendered URL). This is because OS file
 * paths have no escape sequences --- file names cannot contain a
 * `/`.
 */
[[nodiscard]] auto render_url_path_ensure_legal(const std::vector<std::string>& url_path) -> Path;

/**
 * Percent encode path. `%2F` for "interior slashes" is the most
 * important.
 */
[[nodiscard]] auto encode_url_path(std::span<const std::string> url_path) -> std::string;

/**
 * @param lenient @see parse_url
 */
[[nodiscard]] auto decode_query(std::string_view query, bool lenient = false) -> string_map_t;

[[nodiscard]] auto encode_query(const string_map_t& query) -> std::string;

/**
 * Parse a URL into a parsed_url_t.
 *
 * @parm lenient Also allow some long-supported Nix URIs that are not quite compliant with RFC3986.
 * Here are the deviations:
 * - Fragments can contain unescaped (not URL encoded) '^', '"' or space literals.
 * - Queries may contain unescaped '"' or spaces.
 *
 * @note ipv6 ZoneId literals (RFC4007) are represented in URIs according to RFC6874.
 *
 * @throws BadURL
 *
 * The WHATWG specification of the URL constructor in Java Script is
 * also a useful reference:
 * https://url.spec.whatwg.org/#concept-basic-url-parser. Note, however,
 * that it includes various scheme-specific normalizations / extra steps
 * that we do not implement.
 */
[[nodiscard]] auto parse_url(std::string_view url, bool lenient = false) -> parsed_url_t;

/**
 * Like `parse_url`, but also accepts relative URLs, which are resolved
 * against the given base URL.
 *
 * This is specified in [IETF RFC 3986, section
 * 5](https://datatracker.ietf.org/doc/html/rfc3986#section-5)
 *
 * @throws BadURL
 *
 * Behavior should also match the `new URL(url, base)` JavaScript
 * constructor, except for extra steps specific to the HTTP scheme. See
 * `parse_url` for link to the relevant WHATWG standard.
 */
[[nodiscard]] auto parse_url_relative(std::string_view url, const parsed_url_t& base)
    -> parsed_url_t;

/**
 * Although that's not really standardized anywhere, an number of tools
 * use a scheme of the form 'x+y' in urls, where y is the "transport layer"
 * scheme, and x is the "application layer" scheme.
 *
 * For example git uses `git+https` to designate remotes using a git
 * protocol over http.
 */
class parsed_url_scheme_t {
public:
  [[nodiscard]] auto application() const -> const std::optional<std::string_view>& {
    return application_;
  }
  auto set_application(std::optional<std::string_view> value) -> void { application_ = value; }

  [[nodiscard]] auto transport() const -> std::string_view { return transport_; }
  auto set_transport(std::string_view value) -> void { transport_ = value; }

private:
  std::optional<std::string_view> application_;
  std::string_view transport_;
};

[[nodiscard]] auto parse_url_scheme(std::string_view scheme) -> parsed_url_scheme_t;

/**
 * Detects scp-style uris (e.g. `git@github.com:NixOS/nix`) and fixes
 * them by removing the `:` and assuming a scheme of `ssh://`. Also
 * drops `git+` from the scheme (e.g. `git+https://` to `https://`)
 * and changes absolute paths into `file://` URLs.
 */
[[nodiscard]] auto fix_git_url(std::string url) -> parsed_url_t;

/**
 * Whether a string is valid as RFC 3986 scheme name.
 * Colon `:` is part of the URI; not the scheme name, and therefore rejected.
 * See https://www.rfc-editor.org/rfc/rfc3986#section-3.1
 *
 * Does not check whether the scheme is understood, as that's context-dependent.
 */
[[nodiscard]] auto is_valid_scheme_name(std::string_view scheme) -> bool;

/**
 * Either a parsed_url_t or a verbatim string. This is necessary because in certain cases URI must
 * be passed verbatim (e.g. in builtin fetchers), since those are specified by the user. In those
 * cases normalizations performed by the parsed_url_t might be surprising and undesirable, since Nix
 * must be a universal client that has to work with various broken services that might interpret
 * URLs in quirky and non-standard ways.
 *
 * One of those examples is space-as-plus encoding that is very widespread, but it's
 * not strictly RFC3986 compliant. We must preserve that information verbatim.
 *
 * Though we perform parsing and validation for internal needs.
 */
class verbatim_url_t {
public:
  using raw_t = std::variant<std::string, parsed_url_t>;

  verbatim_url_t(std::string_view s) : raw_(std::string{s}) {}

  verbatim_url_t(std::string s) : raw_(std::move(s)) {}

  verbatim_url_t(parsed_url_t url) : raw_(std::move(url)) {}

  /**
   * Get the encoded URL (if specified) verbatim or encode the parsed URL.
   */
  [[nodiscard]] auto to_string() const -> std::string {
    return std::visit(overloaded{[](const std::string& str) { return str; },
                                 [](const parsed_url_t& url) { return url.to_string(); }},
                      raw_);
  }

  [[nodiscard]] auto parsed() const -> parsed_url_t {
    return std::visit(overloaded{[](const std::string& str) { return parse_url(str); },
                                 [](const parsed_url_t& url) { return url; }},
                      raw_);
  }

  [[nodiscard]] auto scheme() const& -> std::string_view {
    return std::visit(
        overloaded{[](std::string_view str) {
                     auto scheme = split_prefix_to(str, ':');
                     if (!scheme) {
                       throw BadURL("URL '%s' doesn't have a scheme", str);
                     }
                     return *scheme;
                   },
                   [](const parsed_url_t& url) -> std::string_view { return url.scheme(); }},
        raw_);
  }

  /**
   * Get the last non-empty path segment from the URL.
   *
   * This is useful for extracting filenames from URLs.
   * For example, "https://example.com/path/to/file.txt?query=value"
   * returns "file.txt".
   *
   * @return The last non-empty path segment, or std::nullopt if no such segment exists.
   */
  [[nodiscard]] auto last_path_segment() const -> std::optional<std::string>;

  [[nodiscard]] auto raw() const -> const raw_t& { return raw_; }

private:
  raw_t raw_;
};

auto operator<<(std::ostream& os, const verbatim_url_t& url) -> std::ostream&;

} // namespace nix
