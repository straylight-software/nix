// straylight::nix::url::bench::url_shim
//
// Implementation of URL parsing shim for benchmarking

#include "url_shim.h"

#include <ada.h>

#include <boost/url.hpp>

// Include straylight primitives - this depends on the ada backend being compiled
// We include the header directly since the library may be built with either backend
#include <straylight/nix/url/url.h>
#include <straylight/nix/url/url_fast.h>

namespace straylight::nix::url::bench {

// ─────────────────────────────────────────────────────────────────────────────
// Boost backend
// ─────────────────────────────────────────────────────────────────────────────

namespace boost_backend {

url_result parse(std::string_view url) {
  url_result result;

  auto parsed = boost::urls::parse_uri(url);
  if (!parsed) {
    // Try with reference (allows relative)
    parsed = boost::urls::parse_uri_reference(url);
    if (!parsed) {
      return result;
    }
  }

  result.valid = true;
  result.scheme = std::string(parsed->scheme());
  result.host = std::string(parsed->host());
  if (parsed->has_port()) {
    result.port = parsed->port_number();
  }
  result.path = std::string(parsed->path());
  result.query = std::string(parsed->query());
  result.fragment = std::string(parsed->fragment());

  return result;
}

bool can_parse(std::string_view url) noexcept {
  auto res = boost::urls::parse_uri(url);
  if (res) {
    return true;
  }
  return static_cast<bool>(boost::urls::parse_uri_reference(url));
}

} // namespace boost_backend

// ─────────────────────────────────────────────────────────────────────────────
// Ada backend
// ─────────────────────────────────────────────────────────────────────────────

namespace ada_backend {

url_result parse(std::string_view url) {
  url_result result;

  auto parsed = ada::parse<ada::url_aggregator>(url);
  if (!parsed) {
    return result;
  }

  result.valid = true;
  result.scheme = std::string(parsed->get_protocol());
  // Remove trailing : from protocol
  if (!result.scheme.empty() && result.scheme.back() == ':') {
    result.scheme.pop_back();
  }
  result.host = std::string(parsed->get_hostname());
  if (parsed->has_port()) {
    // Ada's get_port() returns string_view, need to convert
    auto port_str = parsed->get_port();
    if (!port_str.empty()) {
      result.port = static_cast<std::uint16_t>(std::stoul(std::string(port_str)));
    }
  }
  result.path = std::string(parsed->get_pathname());
  result.query = std::string(parsed->get_search());
  // Remove leading ? from query
  if (!result.query.empty() && result.query.front() == '?') {
    result.query = result.query.substr(1);
  }
  result.fragment = std::string(parsed->get_hash());
  // Remove leading # from fragment
  if (!result.fragment.empty() && result.fragment.front() == '#') {
    result.fragment = result.fragment.substr(1);
  }

  return result;
}

bool can_parse(std::string_view url) noexcept {
  return ada::can_parse(url);
}

} // namespace ada_backend

// ─────────────────────────────────────────────────────────────────────────────
// Straylight backend
// ─────────────────────────────────────────────────────────────────────────────

namespace straylight_backend {

url_result parse(std::string_view url_str) {
  url_result result;

  auto parsed = url::parse(url_str);
  if (!parsed) {
    return result;
  }

  result.valid = true;
  result.scheme = parsed->scheme;
  if (parsed->auth) {
    result.host = parsed->auth->host;
    result.port = parsed->auth->port;
  }
  result.path = parsed->render_path(false);
  // Render query
  if (!parsed->query.empty()) {
    result.query = url::encode_query(parsed->query);
  }
  result.fragment = parsed->fragment;

  return result;
}

bool can_parse(std::string_view url) noexcept {
  return url::can_parse(url);
}

} // namespace straylight_backend

// ─────────────────────────────────────────────────────────────────────────────
// Straylight fast backend (url_view - lazy/zero-copy)
// ─────────────────────────────────────────────────────────────────────────────

namespace straylight_fast_backend {

url_result parse(std::string_view url_str) {
  url_result result;

  auto parsed = url::url_view::parse(url_str);
  if (!parsed) {
    return result;
  }

  result.valid = true;
  result.scheme = std::string(parsed->scheme());
  result.host = std::string(parsed->host());
  result.port = parsed->port();
  result.path = std::string(parsed->path_encoded());
  result.query = std::string(parsed->query_encoded());
  result.fragment = std::string(parsed->fragment_encoded());

  return result;
}

bool can_parse(std::string_view url) noexcept {
  return url::url_view::can_parse(url);
}

} // namespace straylight_fast_backend

// ─────────────────────────────────────────────────────────────────────────────
// URL workload generation
// ─────────────────────────────────────────────────────────────────────────────

std::string generate_url(url_category cat) {
  switch (cat) {
    case url_category::github_https:
      return "https://github.com/NixOS/nixpkgs/archive/refs/heads/master.tar.gz";

    case url_category::github_ssh:
      return "git@github.com:NixOS/nixpkgs.git"; // SCP-style

    case url_category::gitlab_https:
      return "https://gitlab.com/some-org/some-project/-/archive/main/project.tar.gz";

    case url_category::sourcehut:
      return "https://git.sr.ht/~user/repo/archive/main.tar.gz";

    case url_category::http_cache:
      return "https://cache.nixos.org/nar/0aw3n8k4zr5n2b7gq5r6c8w0y4m1x3v9-nixpkgs.tar.xz";

    case url_category::s3_bucket:
      return "s3://nix-cache-bucket/nar/hash.nar?region=us-east-1&endpoint=s3.amazonaws.com";

    case url_category::file_local:
      return "file:///nix/store/0aw3n8k4zr5n2b7gq5r6c8w0y4m1x3v9-bash-5.2/bin/bash";

    case url_category::flake_github:
      return "github:NixOS/nixpkgs/nixos-unstable?dir=pkgs";

    case url_category::flake_path:
      return "path:/home/user/projects/my-flake?dir=subdir";

    case url_category::flake_tarball:
      return "tarball+https://github.com/NixOS/nixpkgs/archive/master.tar.gz?narHash=sha256-abc";

    case url_category::ipv6_literal:
      return "http://[2001:db8::1]:8080/api/v1/packages?format=json";

    case url_category::unicode_path:
      return "https://example.com/path%20with%20spaces/file%2Fname.tar.gz";

    case url_category::long_query:
      return "https://api.example.com/v1/fetch?"
             "repo=nixpkgs&rev=abc123&ref=main&submodules=true&"
             "shallow=false&allRefs=false&deepClone=false&"
             "narHash=sha256-abcdefghijklmnopqrstuvwxyz123456&"
             "lastModified=1234567890&revCount=50000";

    case url_category::deep_path:
      return "https://example.com/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t/file.txt";
  }
  return "";
}

std::vector<std::string>
generate_workload(const std::vector<std::pair<url_category, std::size_t>>& distribution) {
  std::vector<std::string> urls;
  for (const auto& [cat, count] : distribution) {
    for (std::size_t i = 0; i < count; ++i) {
      urls.push_back(generate_url(cat));
    }
  }
  return urls;
}

namespace workloads {

std::vector<std::string> nix_build() {
  // Typical build: lots of cache fetches, some github
  return generate_workload({
      {url_category::http_cache, 100},
      {url_category::github_https, 20},
      {url_category::file_local, 50},
      {url_category::s3_bucket, 10},
  });
}

std::vector<std::string> flake_eval() {
  // Flake evaluation: many flake refs
  return generate_workload({
      {url_category::flake_github, 50},
      {url_category::flake_path, 30},
      {url_category::github_https, 40},
      {url_category::flake_tarball, 20},
  });
}

std::vector<std::string> cache_ops() {
  // Binary cache operations
  return generate_workload({
      {url_category::http_cache, 200},
      {url_category::s3_bucket, 50},
  });
}

std::vector<std::string> edge_cases() {
  // Stress test with difficult URLs
  return generate_workload({
      {url_category::ipv6_literal, 50},
      {url_category::unicode_path, 50},
      {url_category::long_query, 50},
      {url_category::deep_path, 50},
  });
}

std::vector<std::string> mixed_realistic() {
  // Mixed realistic workload reflecting actual Nix usage
  return generate_workload({
      {url_category::http_cache, 40},
      {url_category::github_https, 25},
      {url_category::file_local, 15},
      {url_category::flake_github, 10},
      {url_category::s3_bucket, 5},
      {url_category::ipv6_literal, 2},
      {url_category::unicode_path, 2},
      {url_category::long_query, 1},
  });
}

} // namespace workloads

} // namespace straylight::nix::url::bench
