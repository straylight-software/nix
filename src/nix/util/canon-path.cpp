#include "nix/util/canon-path.h"

#include <cstring>

#include "nix/util/file-path-impl.h"
#include "nix/util/strings-inline.h"
#include "nix/util/util.h"

namespace nix {

const canon_path_t canon_path_t::root = canon_path_t("/");

static std::string abs_path_pure(std::string_view path) {
  return canon_path_inner<unix_path_trait_t>(path, [](auto&, auto&) {});
}

static void ensure_no_null_bytes(std::string_view s) {
  if (std::memchr(s.data(), '\0', s.size())) [[unlikely]] {
    using namespace std::string_view_literals;
    auto str = replace_strings(std::string(s), "\0"sv, "␀"sv);
    throw BadCanonPath("path segment '%s' must not contain null (\\0) bytes", str);
  }
}

canon_path_t::canon_path_t(std::string_view raw) : path(abs_path_pure(concat_strings("/", raw))) {
  ensure_no_null_bytes(raw);
}

canon_path_t::canon_path_t(const char* raw) : path(abs_path_pure(concat_strings("/", raw))) {}

canon_path_t::canon_path_t(std::string_view raw, const canon_path_t& root)
    : path(abs_path_pure(raw.size() > 0 && raw[0] == '/' ? raw
                                                       : concat_strings(root.abs(), "/", raw))) {
  ensure_no_null_bytes(raw);
}

canon_path_t::canon_path_t(const std::vector<std::string>& elems) : path("/") {
  for (auto& s : elems) {
    push(s);
}
}

std::optional<canon_path_t> canon_path_t::parent() const {
  if (is_root()) {
    return std::nullopt;
}
  return canon_path_t(unchecked_t(), path.substr(0, std::max((size_t)1, path.rfind('/'))));
}

void canon_path_t::pop() {
  assert(!is_root());
  path.resize(std::max((size_t)1, path.rfind('/')));
}

bool canon_path_t::is_within(const canon_path_t& parent) const {
  return !(path.size() < parent.path.size() || path.substr(0, parent.path.size()) != parent.path ||
           (parent.path.size() > 1 && path.size() > parent.path.size() &&
            path[parent.path.size()] != '/'));
}

canon_path_t canon_path_t::remove_prefix(const canon_path_t& prefix) const {
  assert(is_within(prefix));
  if (prefix.is_root()) {
    return *this;
}
  if (path.size() == prefix.path.size()) {
    return root;
}
  return canon_path_t(unchecked_t(), path.substr(prefix.path.size()));
}

void canon_path_t::extend(const canon_path_t& x) {
  if (x.is_root()) {
    return;
}
  if (is_root()) {
    path += x.rel();
  } else {
    path += x.abs();
}
}

canon_path_t canon_path_t::operator/(const canon_path_t& x) const {
  auto res = *this;
  res.extend(x);
  return res;
}

void canon_path_t::push(std::string_view c) {
  assert(c.find('/') == c.npos);
  assert(c != "." && c != "..");
  ensure_no_null_bytes(c);
  if (!is_root()) {
    path += '/';
}
  path += c;
}

canon_path_t canon_path_t::operator/(std::string_view c) const {
  auto res = *this;
  res.push(c);
  return res;
}

bool canon_path_t::is_allowed(const std::set<canon_path_t>& allowed) const {
  /* Check if `this` is an exact match or the parent of an
     allowed path. */
  auto lb = allowed.lower_bound(*this);
  if (lb != allowed.end()) {
    if (lb->is_within(*this)) {
      return true;
}
  }

  /* Check if a parent of `this` is allowed. */
  auto path = *this;
  while (!path.is_root()) {
    path.pop();
    if (allowed.count(path)) {
      return true;
}
  }

  return false;
}

std::ostream& operator<<(std::ostream& stream, const canon_path_t& path) {
  stream << path.abs();
  return stream;
}

std::string canon_path_t::make_relative(const canon_path_t& path) const {
  auto p1 = begin();
  auto p2 = path.begin();

  for (; p1 != end() && p2 != path.end() && *p1 == *p2; ++p1, ++p2) {
    ;
}

  if (p1 == end() && p2 == path.end()) {
    return ".";
  } else if (p1 == end()) {
    return std::string(p2.remaining);
  } else {
    std::string res;
    while (p1 != end()) {
      ++p1;
      if (!res.empty()) {
        res += '/';
}
      res += "..";
    }
    if (p2 != path.end()) {
      if (!res.empty()) {
        res += '/';
}
      res += p2.remaining;
    }
    return res;
  }
}

} // namespace nix
