#include "nix/util/canon-path.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "nix/util/file-path-impl.h"
#include "nix/util/util.h"

namespace nix {

const canon_path_t canon_path_t::root = canon_path_t("/"); // NOLINT(cert-err58-cpp)

namespace {

auto abs_path_pure(std::string_view path) -> std::string {
  return canon_path_inner<unix_path_trait_t>(path, [](auto& /*unused*/, auto& /*unused*/) {});
}

void ensure_no_null_bytes(std::string_view str) {
  if (std::memchr(str.data(), '\0', str.size()) != nullptr) [[unlikely]] {
    using namespace std::string_view_literals;
    auto msg = replace_strings(std::string(str), "\0"sv, "␀"sv);
    throw BadCanonPath("path segment '%s' must not contain null (\\0) bytes", msg);
  }
}

} // namespace

canon_path_t::canon_path_t(std::string_view raw) : path_(abs_path_pure(concat_strings("/", raw))) {
  ensure_no_null_bytes(raw);
}

canon_path_t::canon_path_t(const char* raw) : path_(abs_path_pure(concat_strings("/", raw))) {}

canon_path_t::canon_path_t(std::string_view raw, const canon_path_t& root)
    : path_(abs_path_pure(!raw.empty() && raw[0] == '/' ? raw
                                                        : concat_strings(root.abs(), "/", raw))) {
  ensure_no_null_bytes(raw);
}

canon_path_t::canon_path_t(const std::vector<std::string>& elems) : path_("/") {
  for (const auto& str : elems) {
    push(str);
  }
}

auto canon_path_t::parent() const -> std::optional<canon_path_t> {
  if (is_root()) {
    return std::nullopt;
  }
  return canon_path_t(unchecked_t(),
                      path_.substr(0, std::max(static_cast<size_t>(1), path_.rfind('/'))));
}

void canon_path_t::pop() {
  assert(!is_root());
  path_.resize(std::max(static_cast<size_t>(1), path_.rfind('/')));
}

auto canon_path_t::is_within(const canon_path_t& parent) const -> bool {
  return path_.size() >= parent.path_.size() && path_.starts_with(parent.path_) &&
         (parent.path_.size() <= 1 || path_.size() <= parent.path_.size() ||
          path_[parent.path_.size()] == '/');
}

auto canon_path_t::remove_prefix(const canon_path_t& prefix) const -> canon_path_t {
  assert(is_within(prefix));
  if (prefix.is_root()) {
    return *this;
  }
  if (path_.size() == prefix.path_.size()) {
    return root;
  }
  return {unchecked_t(), path_.substr(prefix.path_.size())};
}

void canon_path_t::extend(const canon_path_t& ext) {
  if (ext.is_root()) {
    return;
  }
  if (is_root()) {
    path_ += ext.rel();
  } else {
    path_ += ext.abs();
  }
}

auto canon_path_t::operator/(const canon_path_t& ext) const -> canon_path_t {
  auto res = *this;
  res.extend(ext);
  return res;
}

void canon_path_t::push(std::string_view component) {
  assert(!component.contains('/'));
  assert(component != "." && component != "..");
  ensure_no_null_bytes(component);
  if (!is_root()) {
    path_ += '/';
  }
  path_ += component;
}

auto canon_path_t::operator/(std::string_view component) const -> canon_path_t {
  auto res = *this;
  res.push(component);
  return res;
}

auto canon_path_t::is_allowed(const std::set<canon_path_t>& allowed) const -> bool {
  /* Check if `this` is an exact match or the parent of an
     allowed path. */
  auto lower = allowed.lower_bound(*this);
  if (lower != allowed.end()) {
    if (lower->is_within(*this)) {
      return true;
    }
  }

  /* Check if a parent of `this` is allowed. */
  auto current = *this;
  while (!current.is_root()) {
    current.pop();
    if (allowed.contains(current)) {
      return true;
    }
  }

  return false;
}

auto operator<<(std::ostream& stream, const canon_path_t& path) -> std::ostream& {
  stream << path.abs();
  return stream;
}

auto canon_path_t::make_relative(const canon_path_t& path) const -> std::string {
  auto ptr1 = begin();
  auto ptr2 = path.begin();

  for (; ptr1 != end() && ptr2 != path.end() && *ptr1 == *ptr2; ++ptr1, ++ptr2) {
    ;
  }

  if (ptr1 == end() && ptr2 == path.end()) {
    return ".";
  }
  if (ptr1 == end()) {
    return std::string(ptr2.remaining());
  }
  std::string res;
  while (ptr1 != end()) {
    ++ptr1;
    if (!res.empty()) {
      res += '/';
    }
    res += "..";
  }
  if (ptr2 != path.end()) {
    if (!res.empty()) {
      res += '/';
    }
    res += ptr2.remaining();
  }
  return res;
}

} // namespace nix
